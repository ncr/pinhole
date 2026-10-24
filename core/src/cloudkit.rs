//! The iCloud Photos web API: CloudKit's JSON web services on the
//! `ckdatabasews` host, as icloud.com uses them.

use std::time::{Duration, Instant};

use anyhow::{anyhow, Context, Result};
use reqwest::blocking::{Client, Response};
use reqwest::header::{HeaderMap, HeaderName, HeaderValue, CONTENT_TYPE, COOKIE, RANGE};
use reqwest::StatusCode;
use serde_json::{json, Value};
use url::Url;

use crate::auth::SessionProvider;
use crate::session::Session;

pub const PRIMARY_ZONE: &str = "PrimarySync";

/// The list query icloud.com uses for "Library": every asset, oldest first,
/// without hidden or deleted ones. It returns CPLAsset and CPLMaster records
/// in pairs.
pub const LIBRARY_LIST: &str = "CPLAssetAndMasterByAssetDateWithoutHiddenOrDeleted";
pub const LIBRARY_COUNT: &str = "CPLAssetByAssetDateWithoutHiddenOrDeleted";

/// The fields a grid and viewer need. Asking for fewer keeps pages small.
pub const DESIRED_KEYS: &[&str] = &[
    "resJPEGFullWidth", "resJPEGFullHeight", "resJPEGFullFileType", "resJPEGFullRes",
    "resJPEGLargeWidth", "resJPEGLargeHeight", "resJPEGLargeFileType", "resJPEGLargeRes",
    "resJPEGMedWidth", "resJPEGMedHeight", "resJPEGMedFileType", "resJPEGMedRes",
    "resJPEGThumbWidth", "resJPEGThumbHeight", "resJPEGThumbFileType", "resJPEGThumbRes",
    "resVidFullWidth", "resVidFullHeight", "resVidFullFileType", "resVidFullRes",
    "resVidMedWidth", "resVidMedHeight", "resVidMedFileType", "resVidMedRes",
    "resVidSmallWidth", "resVidSmallHeight", "resVidSmallFileType", "resVidSmallRes",
    "resOriginalWidth", "resOriginalHeight", "resOriginalFileType", "resOriginalRes",
    "resOriginalAltWidth", "resOriginalAltHeight", "resOriginalAltFileType", "resOriginalAltRes",
    "resOriginalVidComplWidth", "resOriginalVidComplHeight", "resOriginalVidComplFileType", "resOriginalVidComplRes",
    "itemType", "filenameEnc", "originalOrientation", "orientation",
    "isDeleted", "isExpunged", "isFavorite", "isHidden",
    "recordName", "recordType", "recordChangeTag", "masterRef",
    "assetDate", "addedDate", "duration", "timeZoneOffset",
    "assetSubtype", "assetSubtypeV2", "assetHDRType", "burstId", "burstFlags",
    "locationEnc", "locationLatitude", "locationLongitude",
    "vidComplDurValue", "vidComplDurScale",
];

/// Counters for the probe's report.
#[derive(Default, Debug, Clone)]
pub struct Traffic {
    pub requests: u64,
    pub bytes: u64,
    pub time: Duration,
    pub retries: u64,
    pub session_refreshes: u64,
}

pub struct PhotosClient {
    http: Client,
    provider: Box<dyn SessionProvider>,
    session: Session,
    pub traffic: Traffic,
}

impl PhotosClient {
    pub fn new(mut provider: Box<dyn SessionProvider>) -> Result<Self> {
        let session = provider.session()?;
        let http = Client::builder()
            .timeout(Duration::from_secs(120))
            .gzip(true)
            .build()?;
        Ok(Self { http, provider, session, traffic: Traffic::default() })
    }

    pub fn session(&self) -> &Session {
        &self.session
    }

    fn database_url(&self, scope: &str, path: &str) -> Result<Url> {
        let root = self.session.service("ckdatabasews").ok_or_else(|| anyhow!("session has no ckdatabasews service"))?;
        let mut url = Url::parse(&format!("{root}/database/1/com.apple.photos.cloud/production/{scope}/{path}"))?;
        {
            let mut q = url.query_pairs_mut();
            for (k, v) in &self.session.params {
                q.append_pair(k, v);
            }
            q.append_pair("remapEnums", "true");
            q.append_pair("getCurrentSyncToken", "true");
        }
        Ok(url)
    }

    /// A handle for downloading resources from other threads. It carries a
    /// copy of the current session and never refreshes it.
    pub fn downloader(&self) -> Downloader {
        Downloader { http: self.http.clone(), session: self.session.clone() }
    }

    /// POST a CloudKit request. A rejected session is replaced once through
    /// the provider; 429 and 5xx back off and retry.
    pub fn post(&mut self, scope: &str, path: &str, body: &Value) -> Result<Value> {
        let mut refreshed = false;
        let mut attempt = 0;
        loop {
            let url = self.database_url(scope, path)?;
            let mut headers = session_headers(&self.session, &url, true)?;
            headers.insert(CONTENT_TYPE, HeaderValue::from_static("text/plain"));
            let started = Instant::now();
            let response = self.http.post(url).headers(headers).body(body.to_string()).send();
            let response = match response {
                Ok(r) => r,
                Err(e) if attempt < 4 => {
                    attempt += 1;
                    self.traffic.retries += 1;
                    eprintln!("network error, retrying: {e}");
                    std::thread::sleep(Duration::from_secs(2u64.pow(attempt)));
                    continue;
                }
                Err(e) => return Err(e.into()),
            };
            let status = response.status();
            let bytes = response.bytes()?;
            self.traffic.requests += 1;
            self.traffic.bytes += bytes.len() as u64;
            self.traffic.time += started.elapsed();

            match status {
                s if s.is_success() => return serde_json::from_slice(&bytes).context("CloudKit answered with something other than JSON"),
                StatusCode::UNAUTHORIZED | StatusCode::MISDIRECTED_REQUEST | StatusCode::FORBIDDEN if !refreshed => {
                    refreshed = true;
                    self.traffic.session_refreshes += 1;
                    self.session = self.provider.session()?;
                }
                s if (s == StatusCode::TOO_MANY_REQUESTS || s.is_server_error()) && attempt < 5 => {
                    attempt += 1;
                    self.traffic.retries += 1;
                    let wait = Duration::from_secs(2u64.pow(attempt));
                    eprintln!("CloudKit answered {s}, waiting {wait:?}");
                    std::thread::sleep(wait);
                }
                s => {
                    let text = String::from_utf8_lossy(&bytes);
                    return Err(anyhow!("CloudKit {path} answered {s}: {}", text.chars().take(300).collect::<String>()));
                }
            }
        }
    }

    pub fn zones(&mut self, scope: &str) -> Result<Value> {
        self.post(scope, "zones/list", &json!({}))
    }

    pub fn indexing_state(&mut self) -> Result<String> {
        let r = self.post("private", "records/query", &json!({
            "query": {"recordType": "CheckIndexingState"},
            "zoneID": {"zoneName": PRIMARY_ZONE},
        }))?;
        Ok(r["records"][0]["fields"]["state"]["value"].as_str().unwrap_or("?").to_owned())
    }

    /// Item count of one of iCloud's indexes, e.g. LIBRARY_COUNT or
    /// "CPLAssetInSmartAlbumByAssetDate:Favorite".
    pub fn count(&mut self, index: &str) -> Result<u64> {
        let r = self.post("private", "internal/records/query/batch", &json!({
            "batch": [{
                "resultsLimit": 1,
                "query": {
                    "filterBy": {"fieldName": "indexCountID", "fieldValue": {"type": "STRING_LIST", "value": [index]}, "comparator": "IN"},
                    "recordType": "HyperionIndexCountLookup",
                },
                "zoneWide": true,
                "zoneID": {"zoneName": PRIMARY_ZONE},
            }]
        }))?;
        r["batch"][0]["records"][0]["fields"]["itemCount"]["value"]
            .as_u64()
            .ok_or_else(|| anyhow!("no itemCount for {index}"))
    }

    /// One page of the library list, starting at `start_rank` (0 is the
    /// oldest asset). `results_limit` counts records, so two per asset.
    pub fn list_page(&mut self, start_rank: u64, results_limit: u32) -> Result<Value> {
        self.post("private", "records/query", &json!({
            "query": {
                "filterBy": [
                    {"fieldName": "startRank", "fieldValue": {"type": "INT64", "value": start_rank}, "comparator": "EQUALS"},
                    {"fieldName": "direction", "fieldValue": {"type": "STRING", "value": "ASCENDING"}, "comparator": "EQUALS"},
                ],
                "recordType": LIBRARY_LIST,
            },
            "resultsLimit": results_limit,
            "desiredKeys": DESIRED_KEYS,
            "zoneID": {"zoneName": PRIMARY_ZONE},
        }))
    }

    /// Records by name, in one request. Download URLs expire within the
    /// hour, so this is how an asset on screen gets fresh ones: look up its
    /// CPLAsset and CPLMaster records again.
    pub fn lookup(&mut self, record_names: &[&str]) -> Result<Value> {
        let records: Vec<Value> = record_names.iter().map(|n| json!({"recordName": n})).collect();
        self.post("private", "records/lookup", &json!({
            "records": records,
            "zoneID": {"zoneName": PRIMARY_ZONE},
            "desiredKeys": DESIRED_KEYS,
        }))
    }

    /// CloudKit's change feed for the library zone. Without a token it
    /// starts from the beginning; with one, it returns what changed since.
    pub fn zone_changes(&mut self, sync_token: Option<&str>, results_limit: u32) -> Result<Value> {
        let mut zone = json!({
            "zoneID": {"zoneName": PRIMARY_ZONE},
            "desiredKeys": DESIRED_KEYS,
            "desiredRecordTypes": ["CPLAsset", "CPLMaster"],
            "reverse": false,
            "resultsLimit": results_limit,
        });
        if let Some(token) = sync_token {
            zone["syncToken"] = json!(token);
        }
        self.post("private", "changes/zone", &json!({"zones": [zone]}))
    }
}

#[derive(Clone)]
pub struct Downloader {
    http: Client,
    session: Session,
}

impl Downloader {
    /// The same session on a connection pool of its own, optionally
    /// limited to HTTP/1.1.
    pub fn separate(&self, http1_only: bool) -> Result<Downloader> {
        let mut builder = Client::builder().timeout(Duration::from_secs(120)).gzip(true);
        if http1_only {
            builder = builder.http1_only();
        }
        Ok(Downloader { http: builder.build()?, session: self.session.clone() })
    }

    /// GET a resource download URL, optionally with a byte range and with or
    /// without the session cookies.
    pub fn fetch(&self, url: &str, range: Option<&str>, with_cookies: bool) -> Result<Response> {
        let url = Url::parse(url)?;
        let mut headers = session_headers(&self.session, &url, with_cookies)?;
        if let Some(r) = range {
            headers.insert(RANGE, HeaderValue::from_str(r)?);
        }
        Ok(self.http.get(url).headers(headers).send()?)
    }
}

fn session_headers(session: &Session, url: &Url, with_cookies: bool) -> Result<HeaderMap> {
    let mut headers = HeaderMap::new();
    for (k, v) in &session.headers {
        headers.insert(HeaderName::from_bytes(k.as_bytes())?, HeaderValue::from_str(v)?);
    }
    if with_cookies {
        if let Some(cookie) = session.cookie_header(url) {
            headers.insert(COOKIE, HeaderValue::from_str(&cookie)?);
        }
    }
    Ok(headers)
}
