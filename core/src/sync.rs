//! Keeping the local library in step with iCloud: the index (every asset's
//! metadata) and the thumbnails the grid shows.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Result};
use rusqlite::{params, Connection};
use serde_json::Value;

use crate::auth::FixedSession;
use crate::cloudkit::{Downloader, PhotosClient, Traffic, LIBRARY_COUNT};
use crate::db;
use crate::session::Session;
use crate::source::{pair_records_with_leftovers, Asset, ICloudSource, Page, PhotoSource};

/// Download URLs stop working somewhere under 35 minutes; older than this
/// they are looked up again before use.
const URL_FRESH_SECS: i64 = 20 * 60;

pub fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

#[derive(Debug, Default)]
pub struct WalkReport {
    pub reported: u64,
    pub assets: u64,
    pub repaired: usize,
    pub unpaired: usize,
    pub removed: usize,
    pub short_pages: u64,
    pub seconds: f64,
    pub traffic: Traffic,
}

/// Read the whole library list into the index, `parallel` pages at a time.
///
/// The list is addressed by position, so pages can be fetched side by side.
/// A page is 200 records, which can split an asset from its master; those
/// halves are paired across pages at the end, and assets whose master never
/// showed up are completed by looking the master up. Assets the index has
/// but iCloud no longer lists are removed.
pub fn walk(
    client: &mut PhotosClient,
    conn: &mut Connection,
    per_page: u32,
    parallel: usize,
    keep_raw: bool,
    progress: &(dyn Fn(u64, u64) + Sync),
) -> Result<WalkReport> {
    let total = client.count(LIBRARY_COUNT)?;
    db::set_meta(conn, "walk_started_at", &now_secs().to_string())?;

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
                let mut source = ICloudSource { client, keep_raw };
                loop {
                    let rank = next.fetch_add(per_page as u64, Ordering::SeqCst);
                    if rank >= total {
                        break;
                    }
                    let before = source.client.traffic.clone();
                    let result = source.page(rank, per_page).map(|p| (rank, p, source.client.traffic.since(&before)));
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
    let mut seen: HashSet<String> = HashSet::new();
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
        // The first page's token marks where the change feed picks up:
        // everything after it is "changed since the walk".
        if rank == 0 {
            if let Some(token) = &page.sync_token {
                db::set_meta(conn, "walk_sync_token", token)?;
            }
        }
        if (page.assets.len() as u32) < per_page && rank + (per_page as u64) < total {
            report.short_pages += 1;
        }
        db::upsert_assets(conn, &page.assets, Some(rank), fetched_at)?;
        seen.extend(page.assets.iter().map(|a| a.id.clone()));
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
    let repaired: Vec<Asset> = repaired.into_iter().filter(|a| !seen.contains(&a.id)).collect();
    db::upsert_assets(conn, &repaired, None, fetched_at)?;
    seen.extend(repaired.iter().map(|a| a.id.clone()));
    report.repaired = repaired.len();
    report.unpaired = unpaired.len();

    // Gone from iCloud since the last walk.
    let stale: Vec<String> = conn
        .prepare("SELECT id FROM assets")?
        .query_map([], |r| r.get::<_, String>(0))?
        .filter_map(|r| r.ok())
        .filter(|id| !seen.contains(id))
        .collect();
    db::remove_assets(conn, &stale)?;
    report.removed = stale.len();

    report.seconds = started.elapsed().as_secs_f64();
    db::set_meta(conn, "walk_done_at", &now_secs().to_string())?;
    Ok(report)
}

#[derive(Debug, Default)]
pub struct ThumbReport {
    pub wanted: usize,
    pub downloaded: usize,
    pub refreshed: usize,
    pub failed: usize,
    pub bytes: u64,
    pub seconds: f64,
}

/// Where a thumbnail lives, relative to the library directory, named by
/// iCloud's checksum of the file: duplicates on one master share it, and an
/// edit, which brings a new thumbnail, gets a new name. Checksums use the
/// base64 alphabet, '/' included, so they are made filesystem-safe, and the
/// files are spread over 256 folders.
pub fn thumb_path(key: &str) -> String {
    let safe: String = key.chars().map(|c| match c { '/' => '_', '+' => '-', c => c }).collect();
    let hash = key.bytes().fold(0u32, |h, b| h.wrapping_mul(31).wrapping_add(b as u32));
    format!("thumbs/{:02x}/{safe}.jpg", hash & 0xff)
}

struct ThumbJob {
    id: String,
    master_id: String,
    url: String,
    checksum: Option<String>,
}

enum ThumbResult {
    Done { id: String, path: String, width: u32, height: u32, colour: u32, bytes: u64 },
    Expired(ThumbJob),
    Failed(String),
}

/// Download every thumbnail the index does not have yet, newest first, so
/// the grid's starting screen fills first. Links older than 20 minutes are
/// looked up again; links that turn out expired anyway are looked up and
/// tried once more.
pub fn download_thumbs(
    client: &mut PhotosClient,
    conn: &mut Connection,
    library: &Path,
    parallel: usize,
    progress: &(dyn Fn(usize, usize) + Sync),
) -> Result<ThumbReport> {
    let started = Instant::now();
    let now = now_secs();
    let mut stale: Vec<(String, String)> = Vec::new();
    let mut jobs: Vec<ThumbJob> = Vec::new();
    {
        let mut stmt = conn.prepare(
            "SELECT a.id, a.master_id, r.url, r.url_fetched_at, r.checksum FROM assets a
             JOIN resources r ON r.asset_id = a.id AND r.kind = 'JPEGThumb'
             WHERE a.thumb_path IS NULL ORDER BY a.asset_date DESC",
        )?;
        let rows = stmt.query_map([], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?, r.get::<_, Option<String>>(2)?,
                r.get::<_, Option<i64>>(3)?, r.get::<_, Option<String>>(4)?))
        })?;
        for row in rows {
            let (id, master_id, url, fetched, checksum) = row?;
            let local = checksum.as_deref().is_some_and(|c| library.join(thumb_path(c)).exists());
            match url {
                // Already on disk under its checksum (a shared master, or a
                // library rebuilt): no link needed, fresh or not.
                Some(url) if local || now - fetched.unwrap_or(0) < URL_FRESH_SECS => {
                    jobs.push(ThumbJob { id, master_id, url, checksum })
                }
                _ => stale.push((id, master_id)),
            }
        }
    }
    let mut report = ThumbReport { wanted: jobs.len() + stale.len(), ..Default::default() };
    if report.wanted == 0 {
        return Ok(report);
    }

    let refreshed = refresh_thumb_urls(client.session(), &stale, 4)?;
    report.refreshed += refreshed.len();
    jobs.extend(refreshed);

    let downloader = client.downloader();
    let mut retry: Vec<ThumbJob> = Vec::new();
    run_thumb_jobs(&downloader, library, jobs, parallel, conn, &mut report, &mut retry, progress)?;
    if !retry.is_empty() {
        let ids: Vec<(String, String)> = retry.into_iter().map(|j| (j.id, j.master_id)).collect();
        let again = refresh_thumb_urls(client.session(), &ids, 4)?;
        report.refreshed += again.len();
        let mut expired_twice = Vec::new();
        run_thumb_jobs(&downloader, library, again, parallel, conn, &mut report, &mut expired_twice, progress)?;
        report.failed += expired_twice.len();
    }
    report.seconds = started.elapsed().as_secs_f64();
    Ok(report)
}

/// Fresh JPEGThumb links for (asset, master) record pairs, 100 per lookup,
/// `parallel` lookups at a time.
fn refresh_thumb_urls(session: &Session, ids: &[(String, String)], parallel: usize) -> Result<Vec<ThumbJob>> {
    if ids.is_empty() {
        return Ok(Vec::new());
    }
    let chunks: Arc<Mutex<Vec<Vec<(String, String)>>>> =
        Arc::new(Mutex::new(ids.chunks(100).map(<[_]>::to_vec).collect()));
    let out: Arc<Mutex<Vec<ThumbJob>>> = Arc::new(Mutex::new(Vec::new()));
    let errors: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
    let workers: Vec<_> = (0..parallel)
        .map(|_| {
            let (chunks, out, errors, session) = (chunks.clone(), out.clone(), errors.clone(), session.clone());
            std::thread::spawn(move || {
                let mut client = match PhotosClient::new(Box::new(FixedSession(session))) {
                    Ok(c) => c,
                    Err(e) => return errors.lock().unwrap().push(e.to_string()),
                };
                loop {
                    let chunk = chunks.lock().unwrap().pop();
                    let Some(chunk) = chunk else { break };
                    let names: Vec<&str> = chunk.iter().flat_map(|(a, m)| [a.as_str(), m.as_str()]).collect();
                    match client.lookup(&names) {
                        Ok(r) => {
                            let records = r["records"].as_array().cloned().unwrap_or_default();
                            let (assets, _) = pair_records_with_leftovers(&records, false);
                            let mut out = out.lock().unwrap();
                            for a in assets {
                                if let Some(r) = a.resource("JPEGThumb") {
                                    if let Some(url) = r.url.clone() {
                                        out.push(ThumbJob {
                                            id: a.id.clone(),
                                            master_id: a.master_id.clone(),
                                            url,
                                            checksum: r.checksum.clone(),
                                        });
                                    }
                                }
                            }
                        }
                        Err(e) => errors.lock().unwrap().push(e.to_string()),
                    }
                }
            })
        })
        .collect();
    for w in workers {
        let _ = w.join();
    }
    if let Some(e) = errors.lock().unwrap().first() {
        return Err(anyhow!("looking up thumbnail links: {e}"));
    }
    let jobs = std::mem::take(&mut *out.lock().unwrap());
    Ok(jobs)
}

#[allow(clippy::too_many_arguments)]
fn run_thumb_jobs(
    downloader: &Downloader,
    library: &Path,
    jobs: Vec<ThumbJob>,
    parallel: usize,
    conn: &mut Connection,
    report: &mut ThumbReport,
    expired: &mut Vec<ThumbJob>,
    progress: &(dyn Fn(usize, usize) + Sync),
) -> Result<()> {
    let total = report.wanted;
    let queue = Arc::new(Mutex::new(jobs));
    let (tx, rx) = mpsc::channel::<ThumbResult>();
    let workers: Vec<_> = (0..parallel)
        .map(|_| {
            let (queue, tx, downloader, library) = (queue.clone(), tx.clone(), downloader.clone(), library.to_path_buf());
            std::thread::spawn(move || loop {
                let job = queue.lock().unwrap().pop();
                let Some(job) = job else { break };
                if tx.send(fetch_thumb(&downloader, &library, job)).is_err() {
                    break;
                }
            })
        })
        .collect();
    drop(tx);

    // Results land in the index in batches: one transaction per few
    // hundred keeps SQLite fast without holding everything back.
    let mut batch: Vec<(String, String, u32, u32, u32)> = Vec::new();
    let flush = |conn: &mut Connection, batch: &mut Vec<(String, String, u32, u32, u32)>| -> Result<()> {
        let tx = conn.transaction()?;
        {
            let mut stmt = tx.prepare_cached(
                "UPDATE assets SET thumb_path = ?2, thumb_w = ?3, thumb_h = ?4, avg_color = ?5 WHERE id = ?1",
            )?;
            for (id, path, w, h, colour) in batch.drain(..) {
                stmt.execute(params![id, path, w, h, colour])?;
            }
        }
        tx.commit()?;
        Ok(())
    };
    let mut failures: HashMap<String, usize> = HashMap::new();
    for result in rx {
        match result {
            ThumbResult::Done { id, path, width, height, colour, bytes } => {
                batch.push((id, path, width, height, colour));
                report.downloaded += 1;
                report.bytes += bytes;
                if batch.len() >= 500 {
                    flush(conn, &mut batch)?;
                }
            }
            ThumbResult::Expired(job) => expired.push(job),
            ThumbResult::Failed(reason) => {
                report.failed += 1;
                *failures.entry(reason).or_default() += 1;
            }
        }
        progress(report.downloaded, total);
    }
    flush(conn, &mut batch)?;
    for w in workers {
        let _ = w.join();
    }
    for (reason, n) in failures {
        eprintln!("{n} thumbnails failed: {reason}");
    }
    Ok(())
}

fn fetch_thumb(downloader: &Downloader, library: &Path, job: ThumbJob) -> ThumbResult {
    let rel = thumb_path(job.checksum.as_deref().unwrap_or(&job.id));
    let path: PathBuf = library.join(&rel);
    let local = std::fs::read(&path).ok();
    let downloaded = local.is_none();
    let mut attempt = 0;
    let bytes = if let Some(b) = local { b.into() } else { loop {
        match downloader.fetch(&job.url, None, false) {
            Ok(r) if r.status().is_success() => match r.bytes() {
                Ok(b) => break b,
                Err(e) => return ThumbResult::Failed(e.to_string()),
            },
            Ok(r) if matches!(r.status().as_u16(), 401 | 403 | 404 | 410) => return ThumbResult::Expired(job),
            Ok(r) if attempt < 3 => {
                attempt += 1;
                eprintln!("thumbnail answered {}, retrying", r.status());
                std::thread::sleep(Duration::from_secs(1 << attempt));
            }
            Ok(r) => return ThumbResult::Failed(format!("HTTP {}", r.status())),
            Err(_) if attempt < 3 => {
                attempt += 1;
                std::thread::sleep(Duration::from_secs(1 << attempt));
            }
            Err(e) => return ThumbResult::Failed(e.to_string()),
        }
    } };

    // The grid draws the average colour until it has decoded the thumbnail,
    // and crops by the real dimensions.
    let image = match image::load_from_memory_with_format(&bytes, image::ImageFormat::Jpeg) {
        Ok(i) => i.to_rgb8(),
        Err(e) => return ThumbResult::Failed(format!("not a JPEG: {e}")),
    };
    let (w, h) = image.dimensions();
    let mut sum = [0u64; 3];
    let mut n = 0u64;
    for p in image.pixels().step_by(7) {
        for k in 0..3 {
            sum[k] += p.0[k] as u64;
        }
        n += 1;
    }
    let n = n.max(1);
    let colour = ((sum[0] / n) << 16 | (sum[1] / n) << 8 | (sum[2] / n)) as u32;

    let write = || -> std::io::Result<()> {
        if !downloaded {
            return Ok(());
        }
        std::fs::create_dir_all(path.parent().expect("thumb folder"))?;
        // Written beside the target and renamed, so the window never reads
        // half a file.
        let tmp = path.with_extension("jpg.part");
        std::fs::write(&tmp, &bytes)?;
        std::fs::rename(&tmp, &path)
    };
    if let Err(e) = write() {
        return ThumbResult::Failed(e.to_string());
    }
    let bytes = if downloaded { bytes.len() as u64 } else { 0 };
    ThumbResult::Done { id: job.id, path: rel, width: w, height: h, colour, bytes }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn thumb_paths_are_safe_file_names() {
        let p = thumb_path("AQn+dI2mCHvNQz6vj/K4BvumTtZV");
        assert!(p.starts_with("thumbs/"));
        assert!(p.ends_with("/AQn-dI2mCHvNQz6vj_K4BvumTtZV.jpg"));
        assert_eq!(p.matches('/').count(), 2);
        assert_eq!(p, thumb_path("AQn+dI2mCHvNQz6vj/K4BvumTtZV"));
    }
}
