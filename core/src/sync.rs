//! Keeping the local library in step with its source, whichever it is: the
//! index (every asset's metadata) and the thumbnails the grid shows.

use std::collections::HashMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Result};
use rusqlite::{params, Connection};

use crate::db::{self, Changes, Index};
use crate::source::{FetchError, Source, Stopped, Stored, Ticket, TokenExpired, Want};

pub fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

#[derive(Debug, Default)]
pub struct SyncReport {
    pub changes: Changes,
    /// The whole library was listed, rather than its changes followed.
    pub listed: bool,
    /// The source's own line about how it went.
    pub summary: String,
    pub seconds: f64,
}

/// Make sure the index is this source's before anything is written to it:
/// a new one becomes it, and one from another source is never touched.
/// iCloud's 50,000 photos must not be listed away because a folder was
/// pointed at their library.
pub fn claim(source: &dyn Source, conn: &mut Connection, library: &Path) -> Result<()> {
    let id = source.id();
    match db::meta(conn, "source") {
        Some(owner) if owner == id => Ok(()),
        None if conn.query_row("SELECT count(*) FROM assets", [], |r| r.get::<_, i64>(0))? == 0 => {
            db::set_meta(conn, "source", &id)
        }
        owner => Err(anyhow!(
            "{} holds photos from {}, not from {}; leaving it alone",
            library.display(),
            owner.as_deref().unwrap_or("something else"),
            source.name()
        )),
    }
}

/// List the whole library into the index: what the source has is put,
/// what it no longer has is removed, and the change token saved. `stop`
/// true gives up with `Stopped`, before anything is removed.
pub fn list(
    source: &dyn Source,
    conn: &mut Connection,
    progress: &(dyn Fn(u64, u64) + Sync),
    stop: &(dyn Fn() -> bool + Sync),
) -> Result<SyncReport> {
    let started = Instant::now();
    // Under way, before the source has counted anything.
    progress(0, 0);
    let mut index = Index::new(conn);
    index.stop = Some(stop);
    let listing = source.list(&mut index, progress)?;
    index.remove_unseen()?;
    let changes = std::mem::take(&mut index.changes);
    match &listing.token {
        Some(token) => db::set_meta(conn, "change_token", token)?,
        None => drop(conn.execute("DELETE FROM meta WHERE key = 'change_token'", [])?),
    }
    db::set_meta(conn, "listed_at", &now_secs().to_string())?;
    finish(conn, &changes)?;
    Ok(SyncReport { changes, listed: true, summary: listing.summary, seconds: started.elapsed().as_secs_f64() })
}

/// Bring the index up to date from the source's changes since the token
/// the last listing or round saved. With `dry_run` nothing is written, the
/// token included. `TokenExpired` when the source no longer accepts it.
pub fn follow(source: &dyn Source, conn: &mut Connection, dry_run: bool) -> Result<SyncReport> {
    let started = Instant::now();
    let token = db::meta(conn, "change_token").ok_or_else(|| TokenExpired("there is none yet".into()))?;
    let mut index = Index::new(conn);
    index.dry_run = dry_run;
    let listing = source.changes(&mut index, &token)?;
    let changes = std::mem::take(&mut index.changes);
    if !dry_run {
        if let Some(token) = &listing.token {
            db::set_meta(conn, "change_token", token)?;
        }
        finish(conn, &changes)?;
    }
    Ok(SyncReport { changes, listed: false, summary: listing.summary, seconds: started.elapsed().as_secs_f64() })
}

/// Follow the changes, or list everything when there is no token yet or
/// the source no longer accepts it, telling `progress` how the listing
/// goes (assets put, and the source's count of them); `stop` as for
/// `list`.
pub fn catch_up(
    source: &dyn Source,
    conn: &mut Connection,
    progress: &(dyn Fn(u64, u64) + Sync),
    stop: &(dyn Fn() -> bool + Sync),
) -> Result<SyncReport> {
    match follow(source, conn, false) {
        Err(e) if e.downcast_ref::<TokenExpired>().is_some() => {
            eprintln!("pinholed: sync: {e}; listing the whole library");
            list(source, conn, progress, stop)
        }
        other => other,
    }
}

/// When the round ended, and a new library version when a window would
/// draw something differently.
fn finish(conn: &Connection, changes: &Changes) -> Result<()> {
    db::set_meta(conn, "synced_at", &now_secs().to_string())?;
    if changes.visible() {
        let version: i64 = db::meta(conn, "library_version").and_then(|v| v.parse().ok()).unwrap_or(0) + 1;
        db::set_meta(conn, "library_version", &version.to_string())?;
    }
    Ok(())
}

#[derive(Debug, Default)]
pub struct ThumbReport {
    pub wanted: usize,
    pub made: usize,
    pub failed: usize,
    /// Of thumbnails fetched; ones already on disk count nothing.
    pub bytes: u64,
    pub seconds: f64,
}

/// Where a thumbnail lives, relative to the library directory, named by the
/// rendition's key (iCloud's checksum of the file): duplicates on one master
/// share it, and an edit, which brings a new thumbnail, gets a new name. Keys
/// may use the base64 alphabet, '/' included, so they are made filesystem-
/// safe, and the files are spread over 256 folders.
pub fn thumb_path(key: &str) -> String {
    let safe: String = key.chars().map(|c| match c { '/' => '_', '+' => '-', c => c }).collect();
    let hash = key.bytes().fold(0u32, |h, b| h.wrapping_mul(31).wrapping_add(b as u32));
    format!("thumbs/{:02x}/{safe}.jpg", hash & 0xff)
}

/// Thumbnails got ready for at a time: iCloud's links stay fresh while
/// they are used, and the newest photos arrive first.
const THUMB_BATCH: usize = 1000;

struct ThumbJob {
    stored: Stored,
    ticket: Ticket,
    retried: bool,
}

enum ThumbResult {
    Done { id: String, path: String, width: u32, height: u32, colour: u32, bytes: u64 },
    Expired(ThumbJob),
    Failed(String),
}

/// Make every thumbnail the index does not have yet, newest first, so the
/// grid's starting screen fills first: from the file already on disk under
/// its key when there is one, else read through the source (iCloud's are
/// downloaded, a folder's made from the photo). A ticket that turns out
/// stale is prepared again, once. `stop` true gives up with `Stopped`
/// once the thumbnails under way are in; the rest are made next time.
pub fn make_thumbs(
    source: &dyn Source,
    conn: &mut Connection,
    library: &Path,
    parallel: usize,
    progress: &(dyn Fn(usize, usize) + Sync),
    stop: &(dyn Fn() -> bool + Sync),
) -> Result<ThumbReport> {
    let started = Instant::now();
    let mut wanted: Vec<Stored> = Vec::new();
    {
        let mut stmt = conn.prepare(
            "SELECT a.id, r.name, r.key, coalesce(r.file_type, '') FROM assets a
             JOIN renditions r ON r.asset_id = a.id AND r.kind = 'thumb'
             WHERE a.thumb_path IS NULL ORDER BY a.asset_date DESC",
        )?;
        let rows = stmt.query_map([], |r| {
            Ok(Stored { asset: r.get(0)?, want: Want::Thumb, name: r.get(1)?, key: r.get(2)?, file_type: r.get(3)? })
        })?;
        for row in rows {
            wanted.push(row?);
        }
    }
    let mut report = ThumbReport { wanted: wanted.len(), ..Default::default() };
    if wanted.is_empty() {
        return Ok(report);
    }

    // Already on disk under its key (a shared master, or a library
    // rebuilt): nothing to fetch.
    let (local, remote): (Vec<Stored>, Vec<Stored>) =
        wanted.into_iter().partition(|s| library.join(thumb_path(&s.key)).exists());
    let jobs = local.into_iter().map(|stored| ThumbJob { stored, ticket: Ticket(String::new()), retried: false }).collect();
    run_thumb_jobs(source, library, jobs, parallel, conn, &mut report, &mut Vec::new(), progress, stop)?;

    for batch in remote.chunks(THUMB_BATCH) {
        if stop() {
            return Err(Stopped.into());
        }
        let mut jobs = Vec::new();
        for (stored, ticket) in batch.iter().zip(source.prepare(conn, batch)?) {
            match ticket {
                Ok(ticket) => jobs.push(ThumbJob { stored: stored.clone(), ticket, retried: false }),
                Err(e) => {
                    report.failed += 1;
                    eprintln!("thumbnail for {}: {e}", stored.asset);
                }
            }
        }
        let mut expired = Vec::new();
        run_thumb_jobs(source, library, jobs, parallel, conn, &mut report, &mut expired, progress, stop)?;
        if expired.is_empty() {
            continue;
        }
        let stored: Vec<Stored> = expired.into_iter().map(|j| j.stored).collect();
        let mut again = Vec::new();
        for (stored, ticket) in stored.iter().zip(source.prepare(conn, &stored)?) {
            match ticket {
                Ok(ticket) => again.push(ThumbJob { stored: stored.clone(), ticket, retried: true }),
                Err(_) => report.failed += 1,
            }
        }
        let mut expired_twice = Vec::new();
        run_thumb_jobs(source, library, again, parallel, conn, &mut report, &mut expired_twice, progress, stop)?;
        report.failed += expired_twice.len();
    }
    if stop() {
        return Err(Stopped.into());
    }
    report.seconds = started.elapsed().as_secs_f64();
    Ok(report)
}

#[allow(clippy::too_many_arguments)]
fn run_thumb_jobs(
    source: &dyn Source,
    library: &Path,
    jobs: Vec<ThumbJob>,
    parallel: usize,
    conn: &mut Connection,
    report: &mut ThumbReport,
    expired: &mut Vec<ThumbJob>,
    progress: &(dyn Fn(usize, usize) + Sync),
    stop: &(dyn Fn() -> bool + Sync),
) -> Result<()> {
    if jobs.is_empty() {
        return Ok(());
    }
    let total = report.wanted;
    let queue = Mutex::new(jobs);
    let (tx, rx) = mpsc::channel::<ThumbResult>();
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
    std::thread::scope(|scope| -> Result<()> {
        for _ in 0..parallel.max(1) {
            let (queue, tx) = (&queue, tx.clone());
            scope.spawn(move || loop {
                if stop() {
                    break;
                }
                let job = queue.lock().unwrap().pop();
                let Some(job) = job else { break };
                if tx.send(fetch_thumb(source, library, job)).is_err() {
                    break;
                }
            });
        }
        drop(tx);
        for result in rx {
            match result {
                ThumbResult::Done { id, path, width, height, colour, bytes } => {
                    batch.push((id, path, width, height, colour));
                    report.made += 1;
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
            progress(report.made, total);
        }
        Ok(())
    })?;
    flush(conn, &mut batch)?;
    for (reason, n) in failures {
        eprintln!("{n} thumbnails failed: {reason}");
    }
    Ok(())
}

fn fetch_thumb(source: &dyn Source, library: &Path, job: ThumbJob) -> ThumbResult {
    let rel = thumb_path(&job.stored.key);
    let path: PathBuf = library.join(&rel);
    let local = std::fs::read(&path).ok();
    let fetched = local.is_none();
    let mut attempt = 0;
    let bytes = if let Some(b) = local { b } else { loop {
        let read = source.open(&job.ticket, None).and_then(|mut body| {
            let mut bytes = Vec::with_capacity(body.length.unwrap_or(64 << 10) as usize);
            body.reader.read_to_end(&mut bytes).map_err(|e| FetchError::Failed(e.into()))?;
            Ok(bytes)
        });
        match read {
            Ok(b) => break b,
            Err(FetchError::Expired) if !job.retried => return ThumbResult::Expired(job),
            Err(FetchError::Expired) => return ThumbResult::Failed("the link kept expiring".into()),
            Err(FetchError::NotFound(e)) => return ThumbResult::Failed(e),
            Err(FetchError::Failed(_)) if attempt < 3 => {
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
        if !fetched {
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
    let bytes = if fetched { bytes.len() as u64 } else { 0 };
    ThumbResult::Done { id: job.stored.asset, path: rel, width: w, height: h, colour, bytes }
}

/// A source from its settings entry, `{"kind": "icloud", "account": apple
/// id}` or `{"kind": "folder", "path": ...}` (`storage::read_settings` has
/// filled in an iCloud one's Apple ID); none is `NoSource`.
pub fn source_for(entry: Option<&serde_json::Value>, auth: PathBuf) -> Result<Arc<dyn Source>> {
    let Some(entry) = entry else { return Ok(Arc::new(crate::source::NoSource)) };
    match entry["kind"].as_str().unwrap_or("") {
        "icloud" => {
            let account = entry["account"].as_str().map(str::to_owned);
            Ok(Arc::new(crate::icloud::IcloudSource::new(auth, account)))
        }
        "folder" => {
            let path = entry["path"].as_str().ok_or_else(|| anyhow!("a folder source needs a path"))?;
            if !Path::new(path).is_absolute() {
                return Err(anyhow!("not an absolute path: {path}"));
            }
            Ok(Arc::new(crate::folder::FolderSource::new(PathBuf::from(path))))
        }
        other => Err(anyhow!("unknown source {other:?}")),
    }
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

    #[test]
    fn a_library_is_only_ever_written_by_its_own_source() {
        use crate::folder::FolderSource;
        let dir = std::env::temp_dir().join(format!("pinhole-claim-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("thumbs")).unwrap();
        let mut conn = db::open(&dir.join("library.sqlite")).unwrap();
        let photos = FolderSource::new(PathBuf::from("/photos"));
        let others = FolderSource::new(PathBuf::from("/other"));

        // A new library becomes the first source's, and no other's.
        claim(&photos, &mut conn, &dir).unwrap();
        conn.execute("INSERT INTO assets (id, thumb_path) VALUES ('a.jpg', 'thumbs/x.jpg')", []).unwrap();
        claim(&photos, &mut conn, &dir).unwrap();
        assert!(claim(&others, &mut conn, &dir).is_err());
        assert_eq!(conn.query_row("SELECT count(*) FROM assets", [], |r| r.get::<_, i64>(0)).unwrap(), 1);
        conn.execute("DELETE FROM assets", []).unwrap();
        // iCloud's is never touched by a folder, nor one that says nothing
        // of where it came from.
        db::set_meta(&conn, "source", "icloud:me@example.com").unwrap();
        conn.execute("INSERT INTO assets (id) VALUES ('B5379A3C')", []).unwrap();
        assert!(claim(&photos, &mut conn, &dir).is_err());
        conn.execute("DELETE FROM meta WHERE key = 'source'", []).unwrap();
        assert!(claim(&photos, &mut conn, &dir).is_err());
        assert_eq!(conn.query_row("SELECT count(*) FROM assets", [], |r| r.get::<_, i64>(0)).unwrap(), 1);
        assert!(dir.join("thumbs").exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_listing_or_thumbnails_stopped_part_way_lose_nothing() {
        use crate::folder::FolderSource;
        let dir = std::env::temp_dir().join(format!("pinhole-stop-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let (photos, library) = (dir.join("photos"), dir.join("library"));
        std::fs::create_dir_all(&photos).unwrap();
        std::fs::create_dir_all(&library).unwrap();
        for name in ["a.jpg", "b.jpg", "c.jpg"] {
            image::RgbImage::new(4, 4).save(photos.join(name)).unwrap();
        }
        let source = FolderSource::new(photos.clone());
        let mut conn = db::open(&library.join("library.sqlite")).unwrap();
        let count = |conn: &Connection, sql: &str| conn.query_row(sql, [], |r| r.get::<_, i64>(0)).unwrap();
        list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert_eq!(count(&conn, "SELECT count(*) FROM assets"), 3);

        // Stopped: nothing put, and above all nothing removed for not being
        // seen, though a file went meanwhile.
        std::fs::remove_file(photos.join("c.jpg")).unwrap();
        let e = list(&source, &mut conn, &|_, _| {}, &|| true).unwrap_err();
        assert!(e.downcast_ref::<Stopped>().is_some());
        assert_eq!(count(&conn, "SELECT count(*) FROM assets"), 3);
        let e = make_thumbs(&source, &mut conn, &library, 2, &|_, _| {}, &|| true).unwrap_err();
        assert!(e.downcast_ref::<Stopped>().is_some());
        assert_eq!(count(&conn, "SELECT count(thumb_path) FROM assets"), 0);

        // Shown again: listed again, and the thumbnails made.
        list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert_eq!(count(&conn, "SELECT count(*) FROM assets"), 2);
        let made = make_thumbs(&source, &mut conn, &library, 2, &|_, _| {}, &|| false).unwrap();
        assert_eq!(made.made, 2);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
