//! Reading iCloud's library into the index: the whole list, walked in
//! parallel pages, and after that its change feed.

use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc};
use std::time::Instant;

use anyhow::{anyhow, Result};
use rusqlite::Connection;
use serde_json::Value;

use pinhole_core::db::Index;
use crate::auth::FixedSession;
use crate::cloudkit::{PhotosClient, Traffic, LIBRARY_COUNT};
use crate::index::store;
use crate::records::{pair_records_with_leftovers, CloudAsset, Page, Pager};
use pinhole_core::source::TokenExpired;
use pinhole_core::sync::now_secs;

#[derive(Debug, Default)]
pub struct WalkReport {
    pub reported: u64,
    pub assets: u64,
    pub repaired: usize,
    pub unpaired: usize,
    pub short_pages: u64,
    /// The first page's: where the change feed picks up, everything after
    /// it being "changed since the walk".
    pub token: Option<String>,
    pub seconds: f64,
    pub traffic: Traffic,
}

/// Read the whole library list into the index, `parallel` pages at a time.
///
/// The list is addressed by position, so pages can be fetched side by side.
/// A page is 200 records, which can split an asset from its master; those
/// halves are paired across pages at the end, and assets whose master never
/// showed up are completed by looking the master up. Removing what iCloud
/// no longer lists is the caller's.
pub fn walk(
    client: &mut PhotosClient,
    index: &mut Index,
    per_page: u32,
    parallel: usize,
    keep_raw: bool,
    progress: &(dyn Fn(u64, u64) + Sync),
) -> Result<WalkReport> {
    let total = client.count(LIBRARY_COUNT)?;

    let next = Arc::new(AtomicU64::new(0));
    let (tx, rx) = mpsc::channel::<Result<(u64, Page, Traffic)>>();
    let session = client.session().clone();
    let started = Instant::now();
    let workers: Vec<_> = (0..parallel.max(1))
        .map(|_| {
            let (next, tx, session) = (next.clone(), tx.clone(), session.clone());
            std::thread::spawn(move || {
                let client = match PhotosClient::new(Box::new(FixedSession(session))) {
                    Ok(c) => c,
                    Err(e) => return drop(tx.send(Err(e))),
                };
                let mut pager = Pager { client, keep_raw };
                loop {
                    let rank = next.fetch_add(per_page as u64, Ordering::SeqCst);
                    if rank >= total {
                        break;
                    }
                    let before = pager.client.traffic.clone();
                    let result = pager.page(rank, per_page).map(|p| (rank, p, pager.client.traffic.since(&before)));
                    let failed = result.is_err();
                    if tx.send(result).is_err() || failed {
                        break;
                    }
                }
            })
        })
        .collect();
    drop(tx);

    let mut report = WalkReport { reported: total, ..Default::default() };
    let mut leftovers: Vec<Value> = Vec::new();
    let fetched_at = now_secs();
    let mut failure = None;
    let mut pages = 0u64;
    for message in rx {
        let (rank, page, t) = match message {
            Ok(m) => m,
            Err(e) => {
                failure.get_or_insert(e);
                continue;
            }
        };
        pages += 1;
        if rank == 0 {
            report.token = page.sync_token.clone();
        }
        // Nothing where the library goes on: not a library that ends here.
        if page.records == 0 && rank + (per_page as u64) < total {
            failure.get_or_insert(anyhow!("iCloud's list from {rank} came empty, of {total}"));
        }
        if (page.assets.len() as u32) < per_page && rank + (per_page as u64) < total {
            report.short_pages += 1;
        }
        store(index, &page.assets, Some(rank), fetched_at)?;
        leftovers.extend(page.leftovers);
        report.assets += page.assets.len() as u64;
        report.traffic.add(&t);
        progress(report.assets, total);
    }
    for w in workers {
        if w.join().is_err() {
            failure.get_or_insert(anyhow!("a thread reading iCloud's list panicked"));
        }
    }
    if let Some(e) = failure {
        return Err(e);
    }
    // Every page, or the caller would take what is missing for gone.
    let wanted = total.div_ceil(per_page.max(1) as u64);
    if pages != wanted {
        return Err(anyhow!("iCloud's list came in {pages} of its {wanted} pages"));
    }

    // Halves split across page boundaries.
    let (mut repaired, mut unpaired) = pair_records_with_leftovers(&leftovers, keep_raw);
    let missing_masters: Vec<String> = unpaired
        .iter()
        .filter(|r| r["recordType"] == "CPLAsset")
        .filter_map(|r| r["fields"]["masterRef"]["value"]["recordName"].as_str().map(str::to_owned))
        .collect();
    for chunk in missing_masters.chunks(200) {
        let names: Vec<&str> = chunk.iter().map(String::as_str).collect();
        let r = client.lookup(&names)?;
        unpaired.extend(r["records"].as_array().cloned().unwrap_or_default());
        let (more, rest) = pair_records_with_leftovers(&unpaired, keep_raw);
        repaired.extend(more);
        unpaired = rest;
    }
    let repaired: Vec<CloudAsset> = repaired.into_iter().filter(|a| !index.seen.contains(&a.id)).collect();
    store(index, &repaired, None, fetched_at)?;
    report.repaired = repaired.len();
    report.unpaired = unpaired.len();
    report.seconds = started.elapsed().as_secs_f64();
    Ok(report)
}

#[derive(Debug, Default)]
pub struct ChangeReport {
    pub records: usize,
    /// Records that could not be made sense of (an asset whose master is gone).
    pub skipped: usize,
    pub token: String,
}

/// Bring the index up to date from CloudKit's change feed, starting at
/// `token`. New and changed assets are written (an asset that changed alone
/// gets its master looked up), and assets deleted, hidden or erased are
/// removed. Thumbnails are not fetched here: new assets have none yet.
pub fn changes(client: &mut PhotosClient, index: &mut Index, token: &str) -> Result<ChangeReport> {
    let mut token = token.to_owned();
    let mut report = ChangeReport::default();
    let mut assets: HashMap<String, Value> = HashMap::new();
    let mut masters: HashMap<String, Value> = HashMap::new();
    let mut deleted: HashSet<String> = HashSet::new();
    loop {
        let r = client.zone_changes(Some(&token), 400)?;
        let zone = &r["zones"][0];
        if let Some(code) = zone["serverErrorCode"].as_str() {
            return Err(TokenExpired(format!("{code}: {}", zone["reason"].as_str().unwrap_or(""))).into());
        }
        for rec in zone["records"].as_array().into_iter().flatten() {
            report.records += 1;
            let Some(name) = rec["recordName"].as_str() else { continue };
            if rec["deleted"] == true {
                deleted.insert(name.to_owned());
                continue;
            }
            match rec["recordType"].as_str() {
                Some("CPLAsset") => drop(assets.insert(name.to_owned(), rec.clone())),
                Some("CPLMaster") => drop(masters.insert(name.to_owned(), rec.clone())),
                _ => {}
            }
        }
        if let Some(next) = zone["syncToken"].as_str() {
            token = next.to_owned();
        }
        if zone["moreComing"] != true {
            break;
        }
    }

    let on_master = |conn: &Connection, master: &str| -> Result<Vec<String>> {
        let mut stmt = conn.prepare_cached("SELECT asset_id FROM icloud_assets WHERE master_id = ?1")?;
        let ids = stmt.query_map([master], |r| r.get(0))?.filter_map(|r| r.ok()).collect();
        Ok(ids)
    };

    // A master that changed alone: its assets need their own records again.
    let mut lonely_masters: Vec<String> = Vec::new();
    for master in masters.keys() {
        for id in on_master(index.conn, master)? {
            if !assets.contains_key(&id) {
                lonely_masters.push(id);
            }
        }
    }
    for chunk in lonely_masters.chunks(200) {
        let names: Vec<&str> = chunk.iter().map(String::as_str).collect();
        for rec in client.lookup(&names)?["records"].as_array().into_iter().flatten() {
            if let Some(name) = rec["recordName"].as_str() {
                assets.insert(name.to_owned(), rec.clone());
            }
        }
    }

    // An asset that changed alone (a favourite, an edit, a deletion): its
    // master, looked up.
    let missing: Vec<String> = assets
        .values()
        .filter_map(|a| a["fields"]["masterRef"]["value"]["recordName"].as_str())
        .filter(|m| !masters.contains_key(*m))
        .map(str::to_owned)
        .collect::<HashSet<_>>()
        .into_iter()
        .collect();
    for chunk in missing.chunks(200) {
        let names: Vec<&str> = chunk.iter().map(String::as_str).collect();
        for rec in client.lookup(&names)?["records"].as_array().into_iter().flatten() {
            if rec["recordType"] == "CPLMaster" {
                if let Some(name) = rec["recordName"].as_str() {
                    masters.insert(name.to_owned(), rec.clone());
                }
            }
        }
    }

    let records: Vec<Value> = assets.values().cloned().chain(masters.values().cloned()).collect();
    let (paired, leftovers) = pair_records_with_leftovers(&records, false);
    report.skipped = leftovers.iter().filter(|r| r["recordType"] == "CPLAsset").count();
    // Deleted records: assets, and assets whose master went; and assets
    // now deleted, hidden or erased.
    let mut removed: Vec<String> = paired.iter().filter(|a| a.gone).map(|a| a.id.clone()).collect();
    for name in &deleted {
        removed.push(name.clone());
        removed.extend(on_master(index.conn, name)?);
    }
    let upserts: Vec<CloudAsset> = paired.into_iter().filter(|a| !a.gone).collect();
    store(index, &upserts, None, now_secs())?;
    index.remove(&removed)?;
    report.token = token;
    Ok(report)
}

#[cfg(test)]
mod tests {
    use std::io::{BufRead, BufReader, Read, Write};
    use std::net::TcpListener;

    use serde_json::json;

    use super::*;
    use crate::session::Session;

    /// CloudKit's list of a library of `total`, its page at each rank as
    /// `page` answers it.
    fn client(total: u64, page: fn(u64) -> Value) -> PhotosClient {
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
                let body: Value = serde_json::from_slice(&body).unwrap();
                let answer = if path.ends_with("query/batch") {
                    json!({"batch": [{"records": [{"fields": {"itemCount": {"value": total}}}]}]})
                } else {
                    page(body["query"]["filterBy"][0]["fieldValue"]["value"].as_u64().unwrap())
                }
                .to_string();
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

    fn walked(total: u64, page: fn(u64) -> Value) -> Result<WalkReport> {
        let path = std::env::temp_dir().join(format!("pinhole-walk-{}-{total}.sqlite", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let mut conn = pinhole_core::db::open(&path).unwrap();
        let mut index = Index::new(&mut conn);
        let result = walk(&mut client(total, page), &mut index, 2, 2, false, &|_, _| {});
        let _ = std::fs::remove_file(&path);
        result
    }

    #[test]
    fn a_listing_with_a_page_missing_is_no_listing() {
        fn master() -> Value {
            json!({"records": [{"recordName": "M", "recordType": "CPLMaster", "fields": {}}]})
        }
        // A page without its records.
        let e = walked(4, |rank| if rank == 2 { json!({}) } else { master() }).unwrap_err();
        assert!(format!("{e:#}").contains("without its records"), "{e:#}");
        // An empty page where the library goes on.
        let e = walked(6, |rank| if rank == 2 { json!({"records": []}) } else { master() }).unwrap_err();
        assert!(format!("{e:#}").contains("came empty"), "{e:#}");
        // The last one empty: the library got shorter meanwhile.
        assert!(walked(4, |rank| if rank == 2 { json!({"records": []}) } else { master() }).is_ok());
    }
}
