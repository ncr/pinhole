//! Adding photos and videos to iCloud Photos, as icloud.com does it
//! (docs/api-findings.md, Uploading): the `photosupload` service gives a
//! link for the file, the file is sent to it, `putAsset` makes the asset of
//! it, and `uploadStatus` says when iCloud has finished with it. The new
//! asset then comes down the change feed like any other.

use std::fs::File;
use std::io::Read;
use std::time::{Duration, Instant, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use reqwest::blocking::{Body, Client};
use reqwest::header::{HeaderValue, CONTENT_TYPE};
use serde_json::{json, Value};
use url::Url;

use pinhole_core::add::{extension, AddError, Added, Addition};
use crate::cloudkit::{session_headers, PRIMARY_ZONE};
use crate::session::Session;

/// What icloud.com takes (its iCPLUploadV2Requirements): photos and videos
/// up to 500 MB.
pub const ACCEPTS: &[&str] = &["jpg", "jpeg", "png", "heic", "heif", "tif", "tiff", "gif", "webp", "mov", "mp4", "m4v"];
const MAX_BYTES: u64 = 524_288_000;
const MIN_BYTES: u64 = 125;
/// How long iCloud may take over one file once it has it.
const PROCESSING: Duration = Duration::from_secs(15 * 60);

/// How long a file being sent may go without iCloud taking more of it, or
/// without an answer once it has it all.
const STALLED: Duration = Duration::from_secs(120);
/// Bytes a second, the slowest a file is still being sent at: a long
/// video at that takes hours, and is let take them.
const SLOWEST: u64 = 16 * 1024;

pub struct Uploader {
    http: Client,
    session: Session,
    stalled: Duration,
}

/// How a file being sent goes: so much of it sent, then iCloud's answer.
enum Sending {
    Sent(u64),
    Answered(reqwest::Result<reqwest::blocking::Response>),
}

impl Uploader {
    pub fn new(session: Session) -> Result<Self> {
        // No deadline for the whole client: a long video takes minutes,
        // and each file has one of its own.
        let http = Client::builder().connect_timeout(Duration::from_secs(30)).timeout(None).build()?;
        Ok(Uploader { http, session, stalled: STALLED })
    }

    /// One asset: its photo or video (a Live Photo's motion is left out,
    /// since the web cannot pair it), the asset's record name back.
    pub fn upload(&self, addition: &Addition, sent: &(dyn Fn(u64, u64) + Sync)) -> Result<Added> {
        let path = &addition.file;
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        if !ACCEPTS.contains(&extension(path).as_str()) {
            return Err(AddError::new("unsupported", format!("iCloud does not take {name}")).into());
        }
        let meta = path.metadata().with_context(|| format!("reading {}", path.display()))?;
        let size = meta.len();
        if size > MAX_BYTES {
            return Err(AddError::new("too_large", format!("{name} is over iCloud's 500 MB for a file")).into());
        }
        if size < MIN_BYTES {
            return Err(AddError::new("unsupported", format!("{name} is too small to be a photo")).into());
        }
        let modified = meta.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_millis() as i64);

        // A link to send the file to.
        let key = pinhole_core::add::uuid();
        let r = self.call("createUploadUrl", &json!({"zoneName": PRIMARY_ZONE, "assets": {&key: size}}))?;
        let link = r["uploadUrls"][&key].as_str().ok_or_else(|| anyhow!("iCloud gave no upload link: {r}"))?.to_owned();

        // The file.
        let file = File::open(path).with_context(|| format!("reading {}", path.display()))?;
        let url = Url::parse(&link)?;
        let mut headers = session_headers(&self.session, &url, true)?;
        headers.insert(CONTENT_TYPE, HeaderValue::from_static("text/plain"));
        // The body goes to reqwest for good: how far it has got, and then
        // its answer, come back over a channel, read here while another
        // thread sends. Nothing for `stalled` (iCloud taking no more of it,
        // or never answering) gives up on it here; reqwest's own deadline,
        // which a sending as slow as SLOWEST still meets, ends it there.
        let (tx, rx) = std::sync::mpsc::channel::<Sending>();
        let progress = tx.clone();
        let body = Counted { inner: file, done: 0, sent: move |n| { let _ = progress.send(Sending::Sent(n)); } };
        let deadline = self.stalled + Duration::from_secs(size / SLOWEST);
        let request = self.http.post(url).headers(headers).timeout(deadline).body(Body::sized(body, size));
        std::thread::spawn(move || {
            let _ = tx.send(Sending::Answered(request.send().map_err(reqwest::Error::without_url)));
        });
        let response = loop {
            match rx.recv_timeout(self.stalled) {
                Ok(Sending::Sent(n)) => sent(n, size),
                Ok(Sending::Answered(response)) => break response?,
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                    return Err(anyhow!("iCloud took nothing more of {name} for {} s", self.stalled.as_secs()));
                }
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return Err(anyhow!("the upload thread panicked")),
            }
        };
        let status = response.status();
        let text = response.text()?;
        if !status.is_success() {
            if text.contains("QUOTA_EXCEEDED") {
                return Err(AddError::new("over_quota", "iCloud storage is full").into());
            }
            return Err(anyhow!("iCloud's upload answered {status}: {}", text.chars().take(300).collect::<String>()));
        }
        let r: Value = serde_json::from_str(&text).context("iCloud's upload answered with something other than JSON")?;
        if r["isDuplicate"].as_bool() == Some(true) {
            return Ok(Added { duplicate: true, ..Added::default() });
        }
        let single = r.get("singleFile").cloned().ok_or_else(|| anyhow!("iCloud's upload said nothing of the file: {r}"))?;

        // The asset made of it.
        let r = self.call("putAsset", &json!({
            "zoneName": PRIMARY_ZONE,
            "files": [{
                "fileName": name,
                "lastModDate": modified,
                "timeZoneOffset": time_zone_offset(),
                "singleFileUploadRequest": single,
            }],
            "localTimeZoneId": time_zone_id(),
            "importGroup": addition.group,
        }))?;
        let put = &r[0];
        let (Some(job), Some(asset)) = (put["uploadJobId"].as_str(), put["cplAsset"].as_str()) else {
            let status = put["response"]["status"].as_i64().unwrap_or(0);
            let message = put["response"]["errorMessage"].as_str().unwrap_or("").to_owned();
            return Err(refused(status, &name, &message));
        };

        // Until iCloud has made it.
        let started = Instant::now();
        loop {
            std::thread::sleep(Duration::from_secs(1));
            let r = self.call("uploadStatus", &json!({"uploadJobIds": [job]}))?;
            let state = &r[job];
            if let Some(code) = state["errorCode"].as_i64() {
                return Err(refused(code, &name, ""));
            }
            if state["progress"].as_f64().is_some_and(|p| p >= 100.0) {
                break;
            }
            if started.elapsed() > PROCESSING {
                return Err(anyhow!("iCloud took over {} minutes with {name}", PROCESSING.as_secs() / 60));
            }
        }
        let note = addition.motion.as_ref().map(|_| format!("{name} was added as a still photo: iCloud on the web does not take a Live Photo's motion"));
        Ok(Added { id: Some(asset.to_owned()), duplicate: false, note })
    }

    /// POST to one of `photosupload`'s endpoints, as icloud.com does,
    /// trying again after Apple's 500s and 429s.
    fn call(&self, endpoint: &str, body: &Value) -> Result<Value> {
        let root = self.session.service("photosupload").ok_or_else(|| anyhow!("the session has no photosupload service"))?;
        let mut url = Url::parse(&format!("{}/photosupload/{endpoint}", root.trim_end_matches('/')))?;
        url.query_pairs_mut().append_pair("dsid", &self.session.dsid);
        let mut attempt = 0;
        loop {
            let mut headers = session_headers(&self.session, &url, true)?;
            headers.insert(CONTENT_TYPE, HeaderValue::from_static("text/plain;charset=UTF-8"));
            let response = self.http.post(url.clone()).headers(headers).timeout(Duration::from_secs(120)).body(body.to_string()).send()?;
            let status = response.status();
            let text = response.text()?;
            if status.is_success() {
                return serde_json::from_str(&text).with_context(|| format!("photosupload {endpoint} answered with something other than JSON"));
            }
            if matches!(status.as_u16(), 429 | 500) && attempt < 3 {
                attempt += 1;
                std::thread::sleep(Duration::from_millis((500 << attempt).min(10_000)));
                continue;
            }
            if matches!(status.as_u16(), 401 | 421) {
                return Err(anyhow!("photosupload {endpoint} answered {}", status.as_u16()));
            }
            return Err(refused(status.as_u16() as i64, endpoint, &text));
        }
    }
}

/// An error code of photosupload's (the HTTP status, or a job's), said.
fn refused(code: i64, what: &str, message: &str) -> anyhow::Error {
    match code {
        413 => AddError::new("over_quota", "iCloud storage is full").into(),
        415 => AddError::new("unsupported", format!("iCloud does not take {what}")).into(),
        423 | 424 => AddError::new("protected", "Advanced Data Protection keeps iCloud.com out of the library, so nothing can be added from here").into(),
        409 => AddError::new("failed", format!("iCloud is adding {what} already")).into(),
        _ => anyhow!("photosupload refused {what} ({code}) {}", message.chars().take(300).collect::<String>()),
    }
}

/// As a browser's Date.getTimezoneOffset: minutes behind UTC.
fn time_zone_offset() -> i32 {
    -chrono::Local::now().offset().local_minus_utc() / 60
}

/// The IANA name of the time zone here: Europe/Warsaw.
fn time_zone_id() -> String {
    if let Ok(tz) = std::env::var("TZ") {
        let tz = tz.trim_start_matches(':');
        if tz.contains('/') {
            return tz.to_owned();
        }
    }
    std::fs::read_link("/etc/localtime")
        .ok()
        .and_then(|p| p.to_string_lossy().split_once("zoneinfo/").map(|(_, z)| z.to_owned()))
        .unwrap_or_else(|| "UTC".into())
}

/// A file being sent, saying how much has gone.
struct Counted<R, F> {
    inner: R,
    done: u64,
    sent: F,
}

impl<R: Read, F: FnMut(u64)> Read for Counted<R, F> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.done += n as u64;
        (self.sent)(self.done);
        Ok(n)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn photosupload_codes_become_reasons_the_window_knows() {
        let code = |n| refused(n, "IMG_1.HEIC", "").downcast::<AddError>().map(|e| e.code).ok();
        assert_eq!(code(413), Some("over_quota"));
        assert_eq!(code(415), Some("unsupported"));
        assert_eq!(code(423), Some("protected"));
        assert_eq!(code(424), Some("protected"));
        // Anything else is the source's, for `health` to judge.
        assert_eq!(code(502), None);
    }

    #[test]
    fn a_file_icloud_stops_taking_or_never_answers_is_given_up_on() {
        use std::io::{BufRead, BufReader, Read, Write};
        use std::net::TcpListener;
        // Where the file goes: a connection taken, and nothing ever said.
        let silent = TcpListener::bind("127.0.0.1:0").unwrap();
        let link = format!("http://127.0.0.1:{}/upload?token=t", silent.local_addr().unwrap().port());
        std::thread::spawn(move || {
            let held: Vec<_> = silent.incoming().collect();
            drop(held);
        });
        // photosupload, giving that link.
        let api = TcpListener::bind("127.0.0.1:0").unwrap();
        let root = format!("http://127.0.0.1:{}", api.local_addr().unwrap().port());
        std::thread::spawn(move || {
            for stream in api.incoming() {
                let Ok(mut stream) = stream else { return };
                let mut reader = BufReader::new(stream.try_clone().unwrap());
                let (mut line, mut length) = (String::new(), 0);
                reader.read_line(&mut line).unwrap();
                loop {
                    let mut header = String::new();
                    reader.read_line(&mut header).unwrap();
                    if header.trim().is_empty() {
                        break;
                    }
                    if let Some((name, value)) = header.split_once(':') {
                        if name.eq_ignore_ascii_case("content-length") {
                            length = value.trim().parse().unwrap();
                        }
                    }
                }
                let mut body = vec![0; length];
                reader.read_exact(&mut body).unwrap();
                let body: Value = serde_json::from_slice(&body).unwrap();
                let key = body["assets"].as_object().unwrap().keys().next().unwrap().clone();
                let answer = json!({"uploadUrls": {key: link}}).to_string();
                let _ = write!(stream, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{answer}", answer.len());
            }
        });
        let dir = std::env::temp_dir().join(format!("pinhole-upload-stall-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("IMG_1.jpg"), [0u8; 300]).unwrap();
        let session: Session = serde_json::from_value(json!({
            "apple_id": "a@example.com", "dsid": "1", "services": {"photosupload": root}, "params": {}, "headers": {},
            "cookies": [], "validated_at": ""
        })).unwrap();
        let mut uploader = Uploader::new(session).unwrap();
        uploader.stalled = Duration::from_millis(500);
        let started = Instant::now();
        let addition = Addition { file: dir.join("IMG_1.jpg"), motion: None, group: "g".into() };
        let e = uploader.upload(&addition, &|_, _| {}).unwrap_err();
        assert!(format!("{e}").contains("took nothing more"), "{e}");
        assert!(started.elapsed() < Duration::from_secs(10));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn what_is_too_big_or_not_taken_never_leaves_this_computer() {
        let dir = std::env::temp_dir().join(format!("pinhole-upload-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("notes.txt"), [0u8; 200]).unwrap();
        std::fs::write(dir.join("tiny.jpg"), [0u8; 10]).unwrap();
        // A session with no services: reaching the network would fail
        // otherwise than with these codes.
        let session: Session = serde_json::from_value(json!({
            "apple_id": "a@example.com", "dsid": "1", "services": {}, "params": {}, "headers": {},
            "cookies": [], "validated_at": ""
        })).unwrap();
        let uploader = Uploader::new(session).unwrap();
        for (file, want) in [("notes.txt", "unsupported"), ("tiny.jpg", "unsupported")] {
            let addition = Addition { file: dir.join(file), motion: None, group: "g".into() };
            let e = uploader.upload(&addition, &|_, _| {}).unwrap_err();
            assert_eq!(e.downcast_ref::<AddError>().map(|e| e.code), Some(want), "{file}");
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}
