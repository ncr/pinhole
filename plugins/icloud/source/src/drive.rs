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

use pinhole_core::synced::{Carried, TooBig, SYNCED_MAX};
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
    /// The folder as this round's `list` judged it (or `write` found or
    /// made it), with its files: none once a `list` finds none it takes.
    approved: Mutex<Option<Approved>>,
}

/// A folder taken for Pinhole's, and what is in it, together: a file's
/// document is never looked for in another folder.
struct Approved {
    /// Its docwsid.
    folder: String,
    /// Its files, by name: their docwsid.
    files: HashMap<String, String>,
}

impl Approved {
    fn new(folder: String, items: &[Value]) -> Self {
        let files = items
            .iter()
            .filter(|i| i["type"] == "FILE")
            .filter_map(|i| Some((full_name(i)?, i["docwsid"].as_str()?.to_owned())))
            .filter(|(name, _)| name != MARK)
            .collect();
        Approved { folder, files }
    }
}

impl Default for Drive {
    fn default() -> Self {
        Drive {
            http: Client::builder().timeout(Duration::from_secs(120)).gzip(true).build().expect("an HTTP client"),
            approved: Mutex::new(None),
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
        let too_big = || anyhow::Error::new(TooBig(what.clone()));
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

    /// The folder at the top of iCloud Drive, judged afresh, or made there
    /// (`make`) and marked: its ids and its items, None if there is none
    /// and it is not to be made. A folder of that name is Pinhole's only if
    /// nobody else shares it and Pinhole's mark is in it as Pinhole writes
    /// it, or it is empty (then marked: one Pinhole made, its mark not sent
    /// yet); any other is an error, and nothing is read from or written to
    /// it.
    fn find(&self, session: &Session, make: bool) -> Result<Option<(String, String, Vec<Value>)>> {
        let ids = |item: &Value| Some((item["drivewsid"].as_str()?.to_owned(), item["docwsid"].as_str()?.to_owned()));
        let top = self.details(session, ROOT)?;
        match top["items"].as_array().into_iter().flatten().find(|i| i["name"] == FOLDER && i["type"] == "FOLDER") {
            Some(item) => {
                let (drivewsid, docwsid) = ids(item).ok_or_else(|| anyhow!("iCloud Drive's {FOLDER} folder has no ids"))?;
                let folder = self.details(session, &drivewsid)?;
                let items = folder["items"].as_array().cloned().unwrap_or_default();
                if shared(item) || shared(&folder) {
                    return Err(anyhow!("iCloud Drive's {FOLDER} folder is shared: Pinhole keeps what it syncs only in a folder nobody else has"));
                }
                let mark = items.iter().find(|i| full_name(i).as_deref() == Some(MARK) && i["type"] == "FILE");
                match mark.and_then(|m| m["docwsid"].as_str()) {
                    Some(mark) if self.download(session, mark, MARK)?.starts_with(MARK_TEXT.as_bytes()) => {}
                    None if items.is_empty() => {
                        self.put(session, &docwsid, MARK, MARK_TEXT.as_bytes(), None)?;
                    }
                    _ => return Err(anyhow!("iCloud Drive's {FOLDER} folder is not one Pinhole made: rename it, and Pinhole makes its own")),
                }
                Ok(Some((drivewsid, docwsid, items)))
            }
            None if make => {
                let client = session.params.get("clientId").cloned().unwrap_or_else(pinhole_core::add::uuid);
                let r = self.json(session, "drivews", "/createFolders", &[], &json!({
                    "destinationDrivewsId": ROOT,
                    "folders": [{"clientId": client, "name": FOLDER}],
                }))?;
                let (drivewsid, docwsid) = ids(&r["folders"][0]).ok_or_else(|| anyhow!("iCloud Drive made no {FOLDER} folder: {r}"))?;
                self.put(session, &docwsid, MARK, MARK_TEXT.as_bytes(), None)?;
                Ok(Some((drivewsid, docwsid, Vec::new())))
            }
            None => Ok(None),
        }
    }

    /// The files there now, by name and version (its etag, which changes
    /// as the file does). The folder is judged again first, every round:
    /// one shared since, or its mark gone, is used no more.
    pub fn list(&self, session: &Session) -> Result<Vec<Carried>> {
        // Nothing of the last round's folder is used again unless it is
        // taken again now.
        *self.approved.lock().unwrap() = None;
        let Some((_, folder, items)) = self.find(session, false)? else { return Ok(Vec::new()) };
        let carried = items
            .iter()
            .filter(|i| i["type"] == "FILE")
            .filter_map(|i| Some(Carried { name: full_name(i)?, version: i["etag"].as_str()?.to_owned() }))
            .filter(|c| c.name != MARK)
            .collect();
        *self.approved.lock().unwrap() = Some(Approved::new(folder, &items));
        Ok(carried)
    }

    pub fn read(&self, session: &Session, name: &str) -> Result<Vec<u8>> {
        let id = self.approved.lock().unwrap().as_ref().and_then(|a| a.files.get(name).cloned());
        let id = id.ok_or_else(|| anyhow!("no {name} in iCloud Drive's {FOLDER}"))?;
        self.download(session, &id, name)
    }

    /// The document `id` (`name` in the folder).
    fn download(&self, session: &Session, id: &str, name: &str) -> Result<Vec<u8>> {
        let bytes = self.send(session, Method::GET, Some("docws"), &format!("/ws/{ZONE}/download/by_id"), &[("document_id", id)], None)?;
        let r: Value = serde_json::from_slice(&bytes).context("iCloud Drive's download answered with something other than JSON")?;
        let link = r["data_token"]["url"].as_str().ok_or_else(|| anyhow!("iCloud Drive gave no link to {name}"))?;
        self.send(session, Method::GET, None, link, &[], None)
    }

    /// This computer's file, made in the folder (and the folder with it),
    /// or written over where it is: the folder as this round's `list`
    /// judged it, else found and judged now.
    pub fn write(&self, session: &Session, name: &str, bytes: &[u8]) -> Result<()> {
        let judged = self.approved.lock().unwrap().as_ref().map(|a| (a.folder.clone(), a.files.get(name).cloned()));
        let (folder, existing) = match judged {
            Some(judged) => judged,
            None => {
                let (_, folder, items) = self.find(session, true)?.expect("made");
                let approved = Approved::new(folder.clone(), &items);
                let existing = approved.files.get(name).cloned();
                *self.approved.lock().unwrap() = Some(approved);
                (folder, existing)
            }
        };
        let id = self.put(session, &folder, name, bytes, existing.as_deref())?;
        if let Some(id) = id {
            // Kept with the folder it is in, if that is still the one taken.
            if let Some(approved) = self.approved.lock().unwrap().as_mut().filter(|a| a.folder == folder) {
                approved.files.insert(name.to_owned(), id);
            }
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
        /// The documents written, in order.
        written: Vec<String>,
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
                    self.written.push(doc.clone());
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
    fn a_folder_moved_away_takes_its_files_documents_with_it() {
        let fake = there(&[("Pinhole", "txt"), ("spawner-0123456789ab", "json")]);
        let session = serve(fake.clone());
        let drive = Drive::default();
        assert_eq!(drive.list(&session).unwrap().len(), 1);
        // Moved out of the top of iCloud Drive: none there now, and its
        // files are no more this computer's to write over or read.
        fake.lock().unwrap().folders.clear();
        assert!(drive.list(&session).unwrap().is_empty());
        assert!(drive.read(&session, "spawner-0123456789ab.json").is_err());
        drive.write(&session, "spawner-0123456789ab.json", b"{}").unwrap();
        // A new document in the folder made anew, not the old one's.
        let fake = fake.lock().unwrap();
        assert_eq!(fake.folders.len(), 1);
        assert!(fake.written.last().is_some_and(|doc| doc != "OLD1"), "{:?}", fake.written);
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
            let bytes = if format!("{name}.{ext}") == MARK { MARK_TEXT.as_bytes().to_vec() } else { b"{}".to_vec() };
            fake.files.insert(format!("OLD{n}"), (name.to_string(), ext.to_string(), 1, bytes));
        }
        Arc::new(Mutex::new(fake))
    }

    fn names(fake: &Arc<Mutex<FakeDrive>>) -> Vec<String> {
        let mut names: Vec<String> = fake.lock().unwrap().files.values().map(|f| format!("{}.{}", f.0, f.1)).collect();
        names.sort();
        names
    }

    #[test]
    fn the_folder_is_pinholes_only_if_marked() {
        // Made by Pinhole: marked as it is made; the mark is no
        // computer's file.
        let fake = Arc::new(Mutex::new(FakeDrive::default()));
        let session = serve(fake.clone());
        Drive::default().write(&session, "spawner-ab12.json", b"{}").unwrap();
        assert_eq!(names(&fake), [MARK, "spawner-ab12.json"]);
        assert_eq!(Drive::default().list(&session).unwrap().len(), 1);

        // Marked: its files read.
        let fake = there(&[("Pinhole", "txt"), ("fixer-0123456789cd", "json")]);
        let session = serve(fake.clone());
        let carried = Drive::default().list(&session).unwrap();
        assert_eq!(carried.iter().map(|c| c.name.as_str()).collect::<Vec<_>>(), ["fixer-0123456789cd.json"]);

        // Unmarked, holding anything: neither read nor written.
        for files in [&[("Tax return", "pdf")][..], &[("fixer-0123456789cd", "json")]] {
            let fake = there(files);
            let session = serve(fake.clone());
            let e = Drive::default().list(&session).unwrap_err();
            assert!(format!("{e}").contains("not one Pinhole made"), "{e}");
            assert!(Drive::default().write(&session, "spawner-ab12.json", b"{}").is_err());
            assert!(!names(&fake).contains(&MARK.to_owned()));
            assert!(!fake.lock().unwrap().requests.iter().any(|p| p.contains("upload") || p.contains("download")));
        }
        // A Pinhole.txt of someone else's: neither.
        let fake = there(&[("fixer-0123456789cd", "json")]);
        fake.lock().unwrap().files.insert("THEIRS".into(), ("Pinhole".into(), "txt".into(), 1, b"my notes".to_vec()));
        let session = serve(fake.clone());
        assert!(Drive::default().list(&session).is_err());
        assert!(Drive::default().write(&session, "spawner-ab12.json", b"{}").is_err());
        assert!(!fake.lock().unwrap().requests.iter().any(|p| p.contains("upload")));
        // Empty (Pinhole's own, its mark not sent when it was made): marked
        // and used.
        let fake = there(&[]);
        let session = serve(fake.clone());
        assert!(Drive::default().list(&session).unwrap().is_empty());
        assert_eq!(names(&fake), [MARK]);

        // Its mark taken away after a round: written to no more.
        let fake = there(&[("Pinhole", "txt"), ("fixer-0123456789cd", "json")]);
        let session = serve(fake.clone());
        let drive = Drive::default();
        drive.list(&session).unwrap();
        drive.write(&session, "spawner-ab12.json", b"{}").unwrap();
        fake.lock().unwrap().files.retain(|_, f| f.0 != "Pinhole");
        assert!(drive.list(&session).is_err());
        assert!(drive.write(&session, "spawner-ab12.json", b"{}").is_err());

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
        assert!(drive.write(&session, "spawner-ab12.json", b"{}").is_err());
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
            assert!(e.downcast_ref::<TooBig>().is_some() && format!("{e:#}").contains("more than 64 MB"), "{e:#}");
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
