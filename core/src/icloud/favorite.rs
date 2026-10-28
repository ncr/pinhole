//! Favourites as icloud.com sets them: `isFavorite` on each CPLAsset, by a
//! `records/modify` with the change tag the asset was last read with. One
//! changed since (on the iPhone, say) is refused with CONFLICT, looked up
//! for its tag now and tried once more. The change then comes down the
//! change feed like any other, and agrees with what was set.

use anyhow::{anyhow, Result};
use serde_json::{json, Value};

use crate::icloud::cloudkit::PhotosClient;

/// Assets to one request: CloudKit takes 200 operations at most.
pub const MODIFY_ASSETS: usize = 100;

/// Make these assets (record name, change tag as the index has it)
/// favourites, or not: their new change tags, or an error naming the first
/// iCloud would not change.
pub fn set(client: &mut PhotosClient, assets: &[(String, Option<String>)], favorite: bool) -> Result<Vec<(String, String)>> {
    let fields = json!({"isFavorite": {"value": i64::from(favorite)}});
    let mut changed = Vec::with_capacity(assets.len());
    for chunk in assets.chunks(MODIFY_ASSETS) {
        let records: Vec<(&str, Option<&str>)> = chunk.iter().map(|(n, t)| (n.as_str(), t.as_deref())).collect();
        let (done, refused) = outcome(&client.modify(&records, &fields)?);
        changed.extend(done);
        if refused.is_empty() {
            continue;
        }
        // Their tags now, and once more.
        let names: Vec<&str> = refused.iter().map(|(n, _)| n.as_str()).collect();
        let found = client.lookup(&names)?;
        let mut again: Vec<(&str, Option<&str>)> = Vec::new();
        for (name, why) in &refused {
            let record = found["records"].as_array().into_iter().flatten().find(|r| r["recordName"] == name.as_str());
            match record.and_then(|r| r["recordChangeTag"].as_str()) {
                Some(tag) => again.push((name, Some(tag))),
                None => return Err(anyhow!("iCloud would not change {name}: {why}")),
            }
        }
        let (done, refused) = outcome(&client.modify(&again, &fields)?);
        changed.extend(done);
        if let Some((name, why)) = refused.first() {
            return Err(anyhow!("iCloud would not change {name}: {why}"));
        }
    }
    Ok(changed)
}

/// A modify's answer: the records changed with their new tags, and the
/// ones refused with why.
fn outcome(response: &Value) -> (Vec<(String, String)>, Vec<(String, String)>) {
    let (mut done, mut refused) = (Vec::new(), Vec::new());
    for r in response["records"].as_array().into_iter().flatten() {
        let name = r["recordName"].as_str().unwrap_or_default().to_owned();
        match (r["serverErrorCode"].as_str(), r["recordChangeTag"].as_str()) {
            (None, Some(tag)) => done.push((name, tag.to_owned())),
            (code, _) => {
                let reason = r["reason"].as_str().unwrap_or_default();
                refused.push((name, format!("{} {reason}", code.unwrap_or("no change tag")).trim().to_owned()));
            }
        }
    }
    (done, refused)
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;
    use std::io::{BufRead, BufReader, Read, Write};
    use std::net::TcpListener;
    use std::sync::{Arc, Mutex};

    use super::*;
    use crate::icloud::auth::FixedSession;
    use crate::icloud::session::Session;

    /// CloudKit as far as favourites go: records with a change tag and
    /// `isFavorite`, which `records/modify` changes only with the tag they
    /// have now, and `records/lookup` tells. Every request is noted.
    struct FakeCloudKit {
        records: HashMap<String, (u32, i64)>,
        requests: Vec<(String, Value)>,
    }

    impl FakeCloudKit {
        fn answer(&mut self, path: &str, body: Value) -> Value {
            self.requests.push((path.to_owned(), body.clone()));
            let record = |name: &str, (tag, favorite): (u32, i64)| {
                json!({"recordName": name, "recordType": "CPLAsset", "recordChangeTag": format!("t{tag}"),
                       "fields": {"isFavorite": {"value": favorite}}})
            };
            let mut records = Vec::new();
            if path.ends_with("records/modify") {
                for op in body["operations"].as_array().unwrap() {
                    assert_eq!(op["operationType"], "update");
                    let r = &op["record"];
                    let name = r["recordName"].as_str().unwrap();
                    match self.records.get_mut(name) {
                        None => records.push(json!({"recordName": name, "serverErrorCode": "NOT_FOUND"})),
                        Some((tag, _)) if r["recordChangeTag"].as_str() != Some(&format!("t{tag}")) => records.push(json!({
                            "recordName": name, "serverErrorCode": "CONFLICT", "reason": "record changed"
                        })),
                        Some(state) => {
                            *state = (state.0 + 1, r["fields"]["isFavorite"]["value"].as_i64().unwrap());
                            records.push(record(name, *state));
                        }
                    }
                }
            } else if path.ends_with("records/lookup") {
                for r in body["records"].as_array().unwrap() {
                    let name = r["recordName"].as_str().unwrap();
                    records.push(match self.records.get(name) {
                        Some(state) => record(name, *state),
                        None => json!({"recordName": name, "serverErrorCode": "NOT_FOUND"}),
                    });
                }
            }
            json!({"records": records})
        }
    }

    /// A client of CloudKit served from 127.0.0.1 by `fake`.
    fn client(fake: Arc<Mutex<FakeCloudKit>>) -> PhotosClient {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { return };
                let mut reader = BufReader::new(stream.try_clone().unwrap());
                let (mut line, mut length) = (String::new(), 0);
                reader.read_line(&mut line).unwrap();
                let path = line.split(' ').nth(1).unwrap_or_default().split('?').next().unwrap_or_default().to_owned();
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
                let answer = fake.lock().unwrap().answer(&path, serde_json::from_slice(&body).unwrap()).to_string();
                let _ = write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{answer}", answer.len());
            }
        });
        let session: Session = serde_json::from_value(json!({
            "apple_id": "a@example.com", "dsid": "1", "services": {"ckdatabasews": format!("http://127.0.0.1:{port}")},
            "params": {}, "headers": {}, "cookies": [], "validated_at": ""
        }))
        .unwrap();
        PhotosClient::new(Box::new(FixedSession(session))).unwrap()
    }

    #[test]
    fn favourites_are_set_with_each_change_tag_and_one_changed_since_is_tried_again() {
        let fake = Arc::new(Mutex::new(FakeCloudKit {
            records: HashMap::from([("A1".to_owned(), (1, 0)), ("A2".to_owned(), (5, 0))]),
            requests: Vec::new(),
        }));
        let mut client = client(fake.clone());
        // A2 was changed on the iPhone since the index read it at t4.
        let assets = [("A1".to_owned(), Some("t1".to_owned())), ("A2".to_owned(), Some("t4".to_owned()))];
        let mut tags = set(&mut client, &assets, true).unwrap();
        tags.sort();
        assert_eq!(tags, [("A1".to_owned(), "t2".to_owned()), ("A2".to_owned(), "t6".to_owned())]);
        {
            let fake = fake.lock().unwrap();
            assert_eq!(fake.records["A1"], (2, 1));
            assert_eq!(fake.records["A2"], (6, 1));
            let paths: Vec<&str> = fake.requests.iter().map(|(p, _)| p.rsplit_once("/private/").unwrap().1).collect();
            assert_eq!(paths, ["records/modify", "records/lookup", "records/modify"]);
            // Only isFavorite, on the asset record, in the library's zone.
            let first = &fake.requests[0].1;
            assert_eq!(first["zoneID"]["zoneName"], "PrimarySync");
            assert_eq!(first["operations"][0]["record"]["recordType"], "CPLAsset");
            assert_eq!(first["operations"][0]["record"]["fields"], json!({"isFavorite": {"value": 1}}));
            // The second try only for the one refused, with its tag now.
            let again = &fake.requests[2].1["operations"];
            assert_eq!(again.as_array().unwrap().len(), 1);
            assert_eq!(again[0]["record"]["recordChangeTag"], "t5");
        }
        // And back, with the new tags.
        let assets: Vec<(String, Option<String>)> = tags.into_iter().map(|(n, t)| (n, Some(t))).collect();
        set(&mut client, &assets, false).unwrap();
        assert_eq!(fake.lock().unwrap().records["A1"], (3, 0));
    }

    #[test]
    fn an_asset_icloud_does_not_have_is_an_error() {
        let fake = Arc::new(Mutex::new(FakeCloudKit { records: HashMap::new(), requests: Vec::new() }));
        let e = set(&mut client(fake), &[("GONE".to_owned(), Some("t1".to_owned()))], true).unwrap_err();
        assert!(format!("{e}").contains("GONE"), "{e}");
    }
}
