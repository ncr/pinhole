//! What is synced of the library (pinhole_core::synced) carried between
//! computers in a Pinhole folder of the account's iCloud Drive, as
//! icloud.com's Drive does it: `drivews` lists folders and makes them,
//! `docws` takes a file (a link to send it to, the file, then the document
//! made or written over) and gives one (a link to it). Each computer
//! writes its own file there and reads the others'
//! (docs/api-findings.md, iCloud Drive).

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use reqwest::blocking::Client;
use reqwest::header::{HeaderValue, CONTENT_TYPE};
use reqwest::Method;
use serde_json::{json, Value};
use url::Url;

use pinhole_core::synced::Carried;
use crate::cloudkit::session_headers;
use crate::session::Session;

/// The folder, at the top of iCloud Drive.
pub const FOLDER: &str = "Pinhole";
const ROOT: &str = "FOLDER::com.apple.CloudDocs::root";
const ZONE: &str = "com.apple.CloudDocs";

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
        let what = format!("iCloud Drive {}", url.path());
        let mut headers = session_headers(session, &url, true)?;
        let mut request = self.http.request(method, url);
        if let Some((bytes, content_type)) = body {
            headers.insert(CONTENT_TYPE, HeaderValue::from_str(content_type)?);
            request = request.body(bytes);
        }
        let response = request.headers(headers).send().with_context(|| what.clone())?;
        let status = response.status();
        let bytes = response.bytes().with_context(|| what.clone())?;
        if !status.is_success() {
            let text = String::from_utf8_lossy(&bytes);
            return Err(anyhow!("{what} answered {}: {}", status.as_u16(), text.chars().take(300).collect::<String>()));
        }
        Ok(bytes.to_vec())
    }

    fn json(&self, session: &Session, service: &str, path: &str, query: &[(&str, &str)], body: &Value) -> Result<Value> {
        let bytes = self.send(session, Method::POST, Some(service), path, query, Some((body.to_string().into_bytes(), "text/plain")))?;
        serde_json::from_slice(&bytes).with_context(|| format!("iCloud Drive {path} answered with something other than JSON"))
    }

    /// A folder's items.
    fn items(&self, session: &Session, drivewsid: &str) -> Result<Vec<Value>> {
        let r = self.json(session, "drivews", "/retrieveItemDetailsInFolders", &[], &json!([{"drivewsid": drivewsid, "partialData": false}]))?;
        let folder = &r[0];
        if folder.get("items").is_none() {
            return Err(anyhow!("iCloud Drive has no folder {drivewsid}: {}", folder.to_string().chars().take(200).collect::<String>()));
        }
        Ok(folder["items"].as_array().cloned().unwrap_or_default())
    }

    /// The folder, found at the top of iCloud Drive, or made there
    /// (`make`): None if there is none and it is not to be made.
    fn folder(&self, session: &Session, make: bool) -> Result<Option<(String, String)>> {
        if let Some(found) = self.folder.lock().unwrap().clone() {
            return Ok(Some(found));
        }
        let ids = |item: &Value| Some((item["drivewsid"].as_str()?.to_owned(), item["docwsid"].as_str()?.to_owned()));
        let top = self.items(session, ROOT)?;
        let mut found = top.iter().find(|i| i["type"] == "FOLDER" && i["name"] == FOLDER).and_then(ids);
        if found.is_none() && make {
            let client = session.params.get("clientId").cloned().unwrap_or_else(pinhole_core::add::uuid);
            let r = self.json(session, "drivews", "/createFolders", &[], &json!({
                "destinationDrivewsId": ROOT,
                "folders": [{"clientId": client, "name": FOLDER}],
            }))?;
            found = Some(ids(&r["folders"][0]).ok_or_else(|| anyhow!("iCloud Drive made no {FOLDER} folder: {r}"))?);
        }
        *self.folder.lock().unwrap() = found.clone();
        Ok(found)
    }

    /// The files there now, by name and version (its etag, which changes
    /// as the file does).
    pub fn list(&self, session: &Session) -> Result<Vec<Carried>> {
        let Some((folder, _)) = self.folder(session, false)? else { return Ok(Vec::new()) };
        let items = match self.items(session, &folder) {
            Ok(items) => items,
            // Gone (moved or deleted in Finder): looked for again next time.
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
        let existing = self.files.lock().unwrap().get(name).cloned();
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
            "document_id": existing.as_deref().unwrap_or(document),
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
        if let Some(id) = made["document_id"].as_str() {
            self.files.lock().unwrap().insert(name.to_owned(), id.to_owned());
        }
        Ok(())
    }
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
    }

    impl FakeDrive {
        fn answer(&mut self, path: &str, query: &str, body: &[u8]) -> Vec<u8> {
            self.requests.push(path.to_owned());
            let json_body = || serde_json::from_slice::<Value>(body).unwrap();
            let base = format!("http://127.0.0.1:{}", self.port);
            let answer = match path {
                "/retrieveItemDetailsInFolders" => {
                    let id = json_body()[0]["drivewsid"].as_str().unwrap().to_owned();
                    let items: Vec<Value> = if id == ROOT {
                        self.folders.iter().map(|(name, doc)| json!({"type": "FOLDER", "name": name,
                            "drivewsid": format!("FOLDER::{ZONE}::{doc}"), "docwsid": doc})).collect()
                    } else {
                        self.files.iter().map(|(doc, (name, ext, etag, _))| json!({"type": "FILE", "name": name, "extension": ext,
                            "docwsid": doc, "etag": format!("e{etag}")})).collect()
                    };
                    json!([{"drivewsid": id, "items": items}])
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

    #[test]
    fn a_file_is_named_with_the_extension_drive_keeps_apart() {
        assert_eq!(full_name(&json!({"name": "spawner-ab12", "extension": "json"})).as_deref(), Some("spawner-ab12.json"));
        assert_eq!(full_name(&json!({"name": "notes", "extension": ""})).as_deref(), Some("notes"));
        assert_eq!(full_name(&json!({"name": "notes"})).as_deref(), Some("notes"));
    }
}
