//! What is synced of the library (pinhole_core::synced) carried between
//! computers in a Pinhole folder of the account's iCloud Drive, as
//! icloud.com's Drive does it: `drivews` lists folders and makes them,
//! `docws` takes a file (a link to send it to, the file, then the document
//! made or written over) and gives one (a link to it). Each computer
//! writes its own file there and reads the others'
//! (docs/api-findings.md, iCloud Drive).

use std::collections::HashMap;
use std::io::Read;
use std::sync::Mutex;
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use reqwest::blocking::Client;
use reqwest::header::{HeaderValue, CONTENT_TYPE};
use reqwest::Method;
use serde_json::{json, Value};
use url::Url;

use pinhole_core::synced::{is_computers, Carried, SYNCED_MAX};
use crate::cloudkit::session_headers;
use crate::session::Session;

/// The folder, at the top of iCloud Drive.
pub const FOLDER: &str = "Pinhole";
const ROOT: &str = "FOLDER::com.apple.CloudDocs::root";
const ZONE: &str = "com.apple.CloudDocs";

/// Times a request that never got there is sent again.
const RETRIES: u64 = 2;

/// An error that says the request was never taken: the connection could
/// not be made, or was closed before it was sent. Not a timeout: one may
/// have got there.
fn unsent(e: &reqwest::Error) -> bool {
    !e.is_timeout() && (e.is_connect() || e.is_request())
}

pub struct Drive {
    http: Client,
    /// The folder once found or made: its drivewsid and docwsid.
    folder: Mutex<Option<(String, String)>>,
    /// Its files as last listed, by name: their docwsid.
    files: Mutex<HashMap<String, String>>,
}

impl Default for Drive {
    fn default() -> Self {
        Drive {
            http: Client::builder().timeout(Duration::from_secs(120)).gzip(true).build().expect("an HTTP client"),
            folder: Mutex::new(None),
            files: Mutex::new(HashMap::new()),
        }
    }
}

/// An item's name as Finder shows it: Drive keeps the extension apart.
fn full_name(item: &Value) -> Option<String> {
    let name = item["name"].as_str()?;
    Some(match item["extension"].as_str().filter(|e| !e.is_empty()) {
        Some(extension) => format!("{name}.{extension}"),
        None => name.to_owned(),
    })
}

impl Drive {
    /// One request to a Drive service (`drivews` or `docws`), or to a link
    /// it gave (`service` None): its body, or an error that says how it
    /// answered ("answered 401": the session is no longer good).
    fn send(&self, session: &Session, method: Method, service: Option<&str>, path: &str, query: &[(&str, &str)], body: Option<(Vec<u8>, &str)>) -> Result<Vec<u8>> {
        let mut url = match service {
            Some(service) => {
                let root = session.service(service).ok_or_else(|| anyhow!("the session has no {service} service"))?;
                let mut url = Url::parse(&format!("{root}{path}"))?;
                {
                    let mut q = url.query_pairs_mut();
                    for (k, v) in &session.params {
                        q.append_pair(k, v);
                    }
                }
                url
            }
            None => Url::parse(path)?,
        };
        if !query.is_empty() {
            let mut q = url.query_pairs_mut();
            for (k, v) in query {
                q.append_pair(k, v);
            }
        }
        // Named without its query, which can hold the session's token
        // (`upload/web`'s), and a link Drive gave by its host alone: an
        // error goes to the log and the window's status.
        let what = match service {
            Some(_) => format!("iCloud Drive {}", url.path()),
            None => format!("a link of iCloud Drive's ({})", url.host_str().unwrap_or("")),
        };
        let mut headers = session_headers(session, &url, true)?;
        if let Some((_, content_type)) = &body {
            headers.insert(CONTENT_TYPE, HeaderValue::from_str(content_type)?);
        }
        // A connection the pool kept that Apple had closed meanwhile ("connection
        // closed before message completed"), or one that could not be made: the
        // request never got there, so it is sent again at once, on a fresh
        // connection, a second time after a moment.
        let mut tries = 0;
        let response = loop {
            let mut request = self.http.request(method.clone(), url.clone()).headers(headers.clone());
            if let Some((bytes, _)) = &body {
                request = request.body(bytes.clone());
            }
            match request.send() {
                Ok(response) => break response,
                Err(e) if tries < RETRIES && unsent(&e) => {
                    tries += 1;
                    std::thread::sleep(Duration::from_millis(500 * (tries - 1)));
                }
                Err(e) => return Err(e.without_url()).with_context(|| what.clone()),
            }
        };
        let status = response.status();
        // Nothing Pinhole keeps there is bigger than a computer's file may
        // be: anything over that is not read into memory, past what says so.
        let too_big = || anyhow!("{what} answered with more than {} MB", SYNCED_MAX >> 20);
        if response.content_length().is_some_and(|n| n > SYNCED_MAX) {
            return Err(too_big());
        }
        let mut bytes = Vec::new();
        if let Err(e) = response.take(SYNCED_MAX + 1).read_to_end(&mut bytes) {
            // reqwest's own, as health() knows it, without its link.
            let e: anyhow::Error = match e.downcast::<reqwest::Error>() {
                Ok(e) => e.without_url().into(),
                Err(e) => e.into(),
            };
            return Err(e.context(what));
        }
        if bytes.len() as u64 > SYNCED_MAX {
            return Err(too_big());
        }
        if !status.is_success() {
            let text = String::from_utf8_lossy(&bytes);
            return Err(anyhow!("{what} answered {}: {}", status.as_u16(), text.chars().take(300).collect::<String>()));
        }
        Ok(bytes)
    }

    fn json(&self, session: &Session, service: &str, path: &str, query: &[(&str, &str)], body: &Value) -> Result<Value> {
        let bytes = self.send(session, Method::POST, Some(service), path, query, Some((body.to_string().into_bytes(), "text/plain")))?;
        serde_json::from_slice(&bytes).with_context(|| format!("iCloud Drive {path} answered with something other than JSON"))
    }

    /// A folder, as Drive describes it, with its items.
    fn details(&self, session: &Session, drivewsid: &str) -> Result<Value> {
        let r = self.json(session, "drivews", "/retrieveItemDetailsInFolders", &[], &json!([{"drivewsid": drivewsid, "partialData": false}]))?;
        let folder = &r[0];
        if !folder["items"].is_array() {
            return Err(anyhow!("iCloud Drive has no folder {drivewsid}: {}", folder.to_string().chars().take(200).collect::<String>()));
        }
        Ok(folder.clone())
    }

    /// The folder, found at the top of iCloud Drive, or made there
    /// (`make`): None if there is none and it is not to be made. A folder
    /// of that name is Pinhole's only if nobody else shares it and Pinhole
    /// put its mark in it, or, from before the mark, it holds nothing but
    /// Pinhole's own files (then marked); anything else is an error, and
    /// nothing is read from or written to it.
    fn folder(&self, session: &Session, make: bool) -> Result<Option<(String, String)>> {
        if let Some(found) = self.folder.lock().unwrap().clone() {
            return Ok(Some(found));
        }
        let ids = |item: &Value| Some((item["drivewsid"].as_str()?.to_owned(), item["docwsid"].as_str()?.to_owned()));
        let top = self.details(session, ROOT)?;
        let found = match top["items"].as_array().into_iter().flatten().find(|i| i["name"] == FOLDER && i["type"] == "FOLDER") {
            Some(item) => {
                let (drivewsid, docwsid) = ids(item).ok_or_else(|| anyhow!("iCloud Drive's {FOLDER} folder has no ids"))?;
                let folder = self.details(session, &drivewsid)?;
                let items = folder["items"].as_array().cloned().unwrap_or_default();
                if shared(item) || shared(&folder) {
                    return Err(anyhow!("iCloud Drive's {FOLDER} folder is shared: Pinhole keeps what it syncs only in a folder nobody else has"));
                }
                if !items.iter().any(|i| full_name(i).as_deref() == Some(MARK) && i["type"] == "FILE") {
                    if !items.iter().all(|i| i["type"] == "FILE" && full_name(i).is_some_and(|n| is_computers(&n))) {
                        return Err(anyhow!("iCloud Drive's {FOLDER} folder holds what Pinhole did not put there: rename it, and Pinhole makes its own"));
                    }
                    self.put(session, &docwsid, MARK, MARK_TEXT.as_bytes(), None)?;
                }
                Some((drivewsid, docwsid))
            }
            None if make => {
                let client = session.params.get("clientId").cloned().unwrap_or_else(pinhole_core::add::uuid);
                let r = self.json(session, "drivews", "/createFolders", &[], &json!({
                    "destinationDrivewsId": ROOT,
                    "folders": [{"clientId": client, "name": FOLDER}],
                }))?;
                let made = ids(&r["folders"][0]).ok_or_else(|| anyhow!("iCloud Drive made no {FOLDER} folder: {r}"))?;
                self.put(session, &made.1, MARK, MARK_TEXT.as_bytes(), None)?;
                Some(made)
            }
            None => None,
        };
        *self.folder.lock().unwrap() = found.clone();
        Ok(found)
    }

    /// The files there now, by name and version (its etag, which changes
    /// as the file does).
    pub fn list(&self, session: &Session) -> Result<Vec<Carried>> {
        let Some((folder, _)) = self.folder(session, false)? else { return Ok(Vec::new()) };
        let details = self.details(session, &folder).and_then(|details| match shared(&details) {
            // Shared since it was found: nothing more read from it.
            true => Err(anyhow!("iCloud Drive's {FOLDER} folder is shared: Pinhole keeps what it syncs only in a folder nobody else has")),
            false => Ok(details),
        });
        let items = match details {
            Ok(details) => details["items"].as_array().cloned().unwrap_or_default(),
            // Gone (moved or deleted in Finder), or shared: looked for and
            // judged again next time.
            Err(e) => {
                *self.folder.lock().unwrap() = None;
                return Err(e);
            }
        };
        let mut files = self.files.lock().unwrap();
        files.clear();
        let mut carried = Vec::new();
        for item in items.iter().filter(|i| i["type"] == "FILE") {
            let (Some(name), Some(id), Some(etag)) = (full_name(item), item["docwsid"].as_str(), item["etag"].as_str()) else { continue };
            if name == MARK {
                continue;
            }
            files.insert(name.clone(), id.to_owned());
            carried.push(Carried { name, version: etag.to_owned() });
        }
        Ok(carried)
    }

    pub fn read(&self, session: &Session, name: &str) -> Result<Vec<u8>> {
        let id = self.files.lock().unwrap().get(name).cloned().ok_or_else(|| anyhow!("no {name} in iCloud Drive's {FOLDER}"))?;
        let bytes = self.send(session, Method::GET, Some("docws"), &format!("/ws/{ZONE}/download/by_id"), &[("document_id", &id)], None)?;
        let r: Value = serde_json::from_slice(&bytes).context("iCloud Drive's download answered with something other than JSON")?;
        let link = r["data_token"]["url"].as_str().ok_or_else(|| anyhow!("iCloud Drive gave no link to {name}"))?;
        self.send(session, Method::GET, None, link, &[], None)
    }

    /// This computer's file, made in the folder (and the folder with it),
    /// or written over where it is.
    pub fn write(&self, session: &Session, name: &str, bytes: &[u8]) -> Result<()> {
        let (_, folder) = self.folder(session, true)?.expect("made");
        let existing = self.files.lock().unwrap().get(name).cloned();
        let id = self.put(session, &folder, name, bytes, existing.as_deref())?;
        if let Some(id) = id {
            self.files.lock().unwrap().insert(name.to_owned(), id);
        }
        Ok(())
    }

    /// A file put in the folder whose docwsid is `folder`: written over
    /// the document `existing`, else a new one. Its document id.
    fn put(&self, session: &Session, folder: &str, name: &str, bytes: &[u8], existing: Option<&str>) -> Result<Option<String>> {
        let validate = session
            .cookies
            .iter()
            .find(|c| c.name == "X-APPLE-WEBAUTH-VALIDATE")
            .and_then(|c| c.value.split(':').find_map(|part| part.strip_prefix("t=")).map(str::to_owned))
            .ok_or_else(|| anyhow!("the session has no token for iCloud Drive"))?;
        // A link to send it to.
        let r = self.json(session, "docws", &format!("/ws/{ZONE}/upload/web"), &[("token", &validate)], &json!({
            "filename": name, "type": "FILE", "content_type": "application/json", "size": bytes.len(),
        }))?;
        let (Some(document), Some(link)) = (r[0]["document_id"].as_str(), r[0]["url"].as_str()) else {
            return Err(anyhow!("iCloud Drive gave no link to send {name} to: {}", r.to_string().chars().take(200).collect::<String>()));
        };
        // It.
        let boundary = format!("pinhole{:016x}", rand::random::<u64>());
        let mut body = format!(
            "--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"; filename=\"{name}\"\r\nContent-Type: application/json\r\n\r\n"
        )
        .into_bytes();
        body.extend_from_slice(bytes);
        body.extend_from_slice(format!("\r\n--{boundary}--\r\n").as_bytes());
        let sent = self.send(session, Method::POST, None, link, &[], Some((body, &format!("multipart/form-data; boundary={boundary}"))))?;
        let sent: Value = serde_json::from_slice(&sent).context("iCloud Drive's upload answered with something other than JSON")?;
        let file = &sent["singleFile"];
        if file.is_null() {
            return Err(anyhow!("iCloud Drive said nothing of {name} sent: {}", sent.to_string().chars().take(200).collect::<String>()));
        }
        // The document: the one there written over, else a new one.
        let now = pinhole_core::synced::now_millis();
        let mut data = json!({
            "signature": file["fileChecksum"], "wrapping_key": file["wrappingKey"],
            "reference_signature": file["referenceChecksum"], "size": file["size"],
        });
        if !file["receipt"].is_null() {
            data["receipt"] = file["receipt"].clone();
        }
        let r = self.json(session, "docws", &format!("/ws/{ZONE}/update/documents"), &[], &json!({
            "data": data,
            "command": "add_file",
            "create_short_guid": true,
            "document_id": existing.unwrap_or(document),
            "path": {"starting_document_id": folder, "path": name},
            "allow_conflict": true,
            "file_flags": {"is_writable": true, "is_executable": false, "is_hidden": false},
            "mtime": now,
            "btime": now,
        }))?;
        let made = &r["results"][0]["document"];
        if r["status"]["status_code"].as_i64() != Some(0) || made["status"]["status_code"].as_i64() != Some(0) {
            return Err(anyhow!("iCloud Drive did not keep {name}: {}", r.to_string().chars().take(300).collect::<String>()));
        }
        Ok(made["document_id"].as_str().map(str::to_owned))
    }
}

/// Pinhole's mark in its folder: a folder of that name without it is not
/// taken for Pinhole's. Not `.json`, so no Pinhole takes it for a
/// computer's file.
const MARK: &str = "Pinhole.txt";
const MARK_TEXT: &str = "Pinhole keeps what it syncs between your computers in this folder (favourite folders, People's names). Do not share it.\n";

/// Whether Drive says an item is shared with someone, or someone's shared
/// with this account: a share of its own, or one it is in.
fn shared(item: &Value) -> bool {
    let counted = |name: &str| item.get(name).is_some_and(|n| n.as_u64() != Some(0));
    counted("shareCount") || counted("shareAliasCount") || item.get("shareID").is_some_and(|s| !s.is_null())
        || item.get("zone").is_some_and(|z| z != ZONE)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader, Read, Write};
    use std::net::TcpListener;
    use std::sync::Arc;

    /// iCloud Drive as far as the Pinhole folder goes: a top with folders,
    /// files with an etag that changes as they are written, links to send
    /// and get them. Every request is noted, by path.
    #[derive(Default)]
    struct FakeDrive {
        port: u16,
        folders: Vec<(String, String)>,
        /// docwsid → (name, extension, etag, bytes)
        files: HashMap<String, (String, String, u32, Vec<u8>)>,
        sent: HashMap<String, Vec<u8>>,
        next: u32,
        requests: Vec<String>,
        /// The Pinhole folder shared with someone.
        shared: bool,
    }

    impl FakeDrive {
        fn answer(&mut self, path: &str, query: &str, body: &[u8]) -> Vec<u8> {
            self.requests.push(path.to_owned());
            let json_body = || serde_json::from_slice::<Value>(body).unwrap();
            let base = format!("http://127.0.0.1:{}", self.port);
            let answer = match path {
                "/retrieveItemDetailsInFolders" => {
                    let id = json_body()[0]["drivewsid"].as_str().unwrap().to_owned();
                    let share = if self.shared { 1 } else { 0 };
                    let items: Vec<Value> = if id == ROOT {
                        self.folders.iter().map(|(name, doc)| json!({"type": "FOLDER", "name": name, "zone": ZONE,
                            "drivewsid": format!("FOLDER::{ZONE}::{doc}"), "docwsid": doc, "shareCount": share, "shareAliasCount": 0})).collect()
                    } else {
                        self.files.iter().map(|(doc, (name, ext, etag, _))| json!({"type": "FILE", "name": name, "extension": ext,
                            "docwsid": doc, "etag": format!("e{etag}")})).collect()
                    };
                    let share = if id == ROOT { 0 } else { share };
                    json!([{"drivewsid": id, "zone": ZONE, "shareCount": share, "shareAliasCount": 0, "items": items}])
                }
                "/createFolders" => {
                    let name = json_body()["folders"][0]["name"].as_str().unwrap().to_owned();
                    self.folders.push((name.clone(), "PIN".into()));
                    json!({"folders": [{"name": name, "drivewsid": format!("FOLDER::{ZONE}::PIN"), "docwsid": "PIN"}]})
                }
                p if p.ends_with("/upload/web") => {
                    assert!(query.contains("token=T0K"), "{query}");
                    self.next += 1;
                    json!([{"document_id": format!("D{}", self.next), "url": format!("{base}/content/D{}", self.next)}])
                }
                p if p.starts_with("/content/") => {
                    let text = String::from_utf8_lossy(body).into_owned();
                    let start = text.find("\r\n\r\n").unwrap() + 4;
                    let end = text.rfind("\r\n--").unwrap();
                    let key = p.trim_start_matches("/content/").to_owned();
                    self.sent.insert(key.clone(), body[start..end].to_vec());
                    json!({"singleFile": {"fileChecksum": key, "wrappingKey": "w", "referenceChecksum": "r", "size": end - start, "receipt": "x"}})
                }
                p if p.ends_with("/update/documents") => {
                    let r = json_body();
                    assert_eq!(r["path"]["starting_document_id"], "PIN");
                    let bytes = self.sent[r["data"]["signature"].as_str().unwrap()].clone();
                    let doc = r["document_id"].as_str().unwrap().to_owned();
                    let (name, ext) = r["path"]["path"].as_str().unwrap().rsplit_once('.').unwrap();
                    let etag = self.files.get(&doc).map_or(1, |f| f.2 + 1);
                    self.files.insert(doc.clone(), (name.into(), ext.into(), etag, bytes));
                    json!({"status": {"status_code": 0}, "results": [{"document": {"status": {"status_code": 0}, "document_id": doc}}]})
                }
                p if p.ends_with("/download/by_id") => {
                    let doc = query.split('&').find_map(|kv| kv.strip_prefix("document_id=")).unwrap();
                    json!({"data_token": {"url": format!("{base}/file/{doc}")}})
                }
                p if p.starts_with("/file/") => return self.files[p.trim_start_matches("/file/")].3.clone(),
                _ => panic!("no {path}"),
            };
            answer.to_string().into_bytes()
        }
    }

    fn serve(fake: Arc<Mutex<FakeDrive>>) -> Session {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        fake.lock().unwrap().port = port;
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { return };
                let mut reader = BufReader::new(stream.try_clone().unwrap());
                let (mut line, mut length) = (String::new(), 0);
                reader.read_line(&mut line).unwrap();
                let target = line.split(' ').nth(1).unwrap_or_default().to_owned();
                let (path, query) = target.split_once('?').unwrap_or((&target, ""));
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
                let answer = fake.lock().unwrap().answer(path, query, &body);
                let _ = write!(stream, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", answer.len());
                let _ = stream.write_all(&answer);
            }
        });
        let base = format!("http://127.0.0.1:{port}");
        serde_json::from_value(json!({
            "apple_id": "a@example.com", "dsid": "1", "services": {"drivews": base, "docws": base},
            "params": {"clientId": "C"}, "headers": {},
            "cookies": [{"name": "X-APPLE-WEBAUTH-VALIDATE", "value": "v=1:t=T0K", "domain": "127.0.0.1", "path": "/", "secure": false, "expires": null}],
            "validated_at": ""
        }))
        .unwrap()
    }

    #[test]
    fn a_request_whose_connection_closes_unanswered_is_sent_again() {
        let fake = Arc::new(Mutex::new(FakeDrive::default()));
        let session = serve(fake.clone());
        // In front of it: the first connection closed before an answer.
        let front = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = front.local_addr().unwrap().port();
        let back = fake.lock().unwrap().port;
        std::thread::spawn(move || {
            for (n, stream) in front.incoming().enumerate() {
                let Ok(mut stream) = stream else { return };
                if n == 0 {
                    let mut line = [0u8; 64];
                    let _ = stream.read(&mut line);
                    continue;
                }
                let mut server = std::net::TcpStream::connect(("127.0.0.1", back)).unwrap();
                let (mut a, mut b) = (stream.try_clone().unwrap(), server.try_clone().unwrap());
                std::thread::spawn(move || { let _ = std::io::copy(&mut a, &mut server); });
                let _ = std::io::copy(&mut b, &mut stream);
            }
        });
        let mut session = session;
        let base = format!("http://127.0.0.1:{port}");
        session.services.insert("drivews".into(), base);
        let drive = Drive::default();
        assert!(drive.list(&session).unwrap().is_empty());
        assert_eq!(fake.lock().unwrap().requests, ["/retrieveItemDetailsInFolders"]);
    }

    #[test]
    fn a_computers_file_is_made_in_the_folder_written_over_and_read_back() {
        let fake = Arc::new(Mutex::new(FakeDrive::default()));
        let session = serve(fake.clone());
        let drive = Drive::default();
        // No folder yet: nothing there, and nothing made for a look.
        assert!(drive.list(&session).unwrap().is_empty());
        assert!(fake.lock().unwrap().folders.is_empty());

        drive.write(&session, "spawner-ab12.json", b"{\"one\": 1}").unwrap();
        assert_eq!(fake.lock().unwrap().folders, [(FOLDER.to_owned(), "PIN".to_owned())]);
        let first = drive.list(&session).unwrap();
        assert_eq!(first, [Carried { name: "spawner-ab12.json".into(), version: "e1".into() }]);

        // Written again: the same document, another etag, no second file.
        drive.write(&session, "spawner-ab12.json", b"{\"two\": 2}").unwrap();
        let second = drive.list(&session).unwrap();
        assert_eq!(second, [Carried { name: "spawner-ab12.json".into(), version: "e2".into() }]);
        assert_eq!(drive.read(&session, "spawner-ab12.json").unwrap(), b"{\"two\": 2}");
        assert_eq!(fake.lock().unwrap().requests.iter().filter(|p| *p == "/createFolders").count(), 1);
    }

    /// A Pinhole folder there already, with these files in it.
    fn there(files: &[(&str, &str)]) -> Arc<Mutex<FakeDrive>> {
        let mut fake = FakeDrive { folders: vec![(FOLDER.to_owned(), "PIN".to_owned())], ..Default::default() };
        for (n, (name, ext)) in files.iter().enumerate() {
            fake.files.insert(format!("OLD{n}"), (name.to_string(), ext.to_string(), 1, b"{}".to_vec()));
        }
        Arc::new(Mutex::new(fake))
    }

    fn names(fake: &Arc<Mutex<FakeDrive>>) -> Vec<String> {
        let mut names: Vec<String> = fake.lock().unwrap().files.values().map(|f| format!("{}.{}", f.0, f.1)).collect();
        names.sort();
        names
    }

    #[test]
    fn the_folder_is_pinholes_only_if_marked_or_holding_only_its_files() {
        // Made by Pinhole: marked as it is made; the mark is no
        // computer's file.
        let fake = Arc::new(Mutex::new(FakeDrive::default()));
        let session = serve(fake.clone());
        Drive::default().write(&session, "spawner-ab12.json", b"{}").unwrap();
        assert_eq!(names(&fake), [MARK, "spawner-ab12.json"]);
        assert_eq!(Drive::default().list(&session).unwrap().len(), 1);

        // Pinhole's from before the mark: its files kept and read, the
        // folder marked.
        let fake = there(&[("fixer-0123456789cd", "json")]);
        let session = serve(fake.clone());
        let carried = Drive::default().list(&session).unwrap();
        assert_eq!(carried.iter().map(|c| c.name.as_str()).collect::<Vec<_>>(), ["fixer-0123456789cd.json"]);
        assert_eq!(names(&fake), [MARK, "fixer-0123456789cd.json"]);

        // Someone's own folder of that name: neither read nor written.
        let fake = there(&[("Tax return", "pdf"), ("fixer-0123456789cd", "json")]);
        let session = serve(fake.clone());
        let e = Drive::default().list(&session).unwrap_err();
        assert!(format!("{e}").contains("did not put there"), "{e}");
        assert!(Drive::default().write(&session, "spawner-ab12.json", b"{}").is_err());
        assert_eq!(names(&fake), ["Tax return.pdf", "fixer-0123456789cd.json"]);
        // JSON, but not a computer's.
        let fake = there(&[("budget", "json")]);
        let session = serve(fake.clone());
        assert!(Drive::default().list(&session).is_err());
        assert!(Drive::default().write(&session, "spawner-ab12.json", b"{}").is_err());
        assert_eq!(names(&fake), ["budget.json"]);

        // Shared with others, even marked: neither read nor written.
        let fake = there(&[("Pinhole", "txt"), ("fixer-0123456789cd", "json")]);
        fake.lock().unwrap().shared = true;
        let session = serve(fake.clone());
        let e = Drive::default().list(&session).unwrap_err();
        assert!(format!("{e}").contains("shared"), "{e}");
        assert!(Drive::default().write(&session, "spawner-ab12.json", b"{}").is_err());
        assert_eq!(names(&fake), [MARK, "fixer-0123456789cd.json"]);
        assert!(!fake.lock().unwrap().requests.iter().any(|p| p.contains("upload") || p.contains("download")));

        // Shared after it was found: read no more.
        let fake = there(&[("Pinhole", "txt")]);
        let session = serve(fake.clone());
        let drive = Drive::default();
        drive.list(&session).unwrap();
        fake.lock().unwrap().shared = true;
        assert!(drive.list(&session).is_err());
    }

    #[test]
    fn a_file_bigger_than_a_computers_may_be_is_not_read_into_memory() {
        let fake = Arc::new(Mutex::new(FakeDrive::default()));
        let session = serve(fake);
        // One that says it is a gigabyte, and one that does not say and
        // goes on past the most.
        let server = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = server.local_addr().unwrap().port();
        std::thread::spawn(move || {
            for (n, stream) in server.incoming().enumerate() {
                let Ok(mut stream) = stream else { return };
                let mut line = [0u8; 1024];
                let _ = stream.read(&mut line);
                if n == 0 {
                    let _ = write!(stream, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{{", 1u64 << 30);
                } else {
                    let _ = write!(stream, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
                    let chunk = vec![b' '; 1 << 20];
                    for _ in 0..(SYNCED_MAX >> 20) + 2 {
                        if stream.write_all(&chunk).is_err() {
                            break;
                        }
                    }
                }
            }
        });
        let link = format!("http://127.0.0.1:{port}/file");
        for _ in 0..2 {
            let e = Drive::default().send(&session, Method::GET, None, &link, &[], None).unwrap_err();
            assert!(format!("{e:#}").contains("more than 64 MB"), "{e:#}");
        }
    }

    #[test]
    fn a_failed_request_never_tells_the_link_it_was_sent_to() {
        let fake = Arc::new(Mutex::new(FakeDrive::default()));
        let session = serve(fake);
        // Nothing listens there.
        let closed = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
        let link = format!("http://127.0.0.1:{closed}/upload/SECRETPATH?token=SECRET");
        let e = Drive::default().send(&session, Method::POST, None, &link, &[], Some((b"x".to_vec(), "text/plain"))).unwrap_err();
        let said = format!("{e:#} {e:?}");
        assert!(!said.contains("SECRET"), "{said}");
        assert!(said.contains("127.0.0.1"), "{said}");
    }

    #[test]
    fn a_file_is_named_with_the_extension_drive_keeps_apart() {
        assert_eq!(full_name(&json!({"name": "spawner-ab12", "extension": "json"})).as_deref(), Some("spawner-ab12.json"));
        assert_eq!(full_name(&json!({"name": "notes", "extension": ""})).as_deref(), Some("notes"));
        assert_eq!(full_name(&json!({"name": "notes"})).as_deref(), Some("notes"));
    }
}
