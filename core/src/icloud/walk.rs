//! Reading iCloud's library into the index: the whole list, walked in
//! parallel pages, and after that its change feed.

use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc};
use std::time::Instant;

use anyhow::Result;
use rusqlite::Connection;
use serde_json::Value;

use crate::db::Index;
use crate::icloud::auth::FixedSession;
use crate::icloud::cloudkit::{PhotosClient, Traffic, LIBRARY_COUNT};
use crate::icloud::index::store;
use crate::icloud::records::{pair_records_with_leftovers, CloudAsset, Page, Pager};
use crate::source::TokenExpired;
use crate::sync::now_secs;

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
    for message in rx {
        let (rank, page, t) = match message {
            Ok(m) => m,
            Err(e) => {
                failure.get_or_insert(e);
                continue;
            }
        };
        if rank == 0 {
            report.token = page.sync_token.clone();
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
        let _ = w.join();
    }
    if let Some(e) = failure {
        return Err(e);
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
