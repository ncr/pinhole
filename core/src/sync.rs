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
/// the last listing or round saved. `TokenExpired` when the source no
/// longer accepts it.
pub fn follow(source: &dyn Source, conn: &mut Connection) -> Result<SyncReport> {
    let started = Instant::now();
    let token = db::meta(conn, "change_token").ok_or_else(|| TokenExpired("there is none yet".into()))?;
    let mut index = Index::new(conn);
    let listing = source.changes(&mut index, &token)?;
    let changes = std::mem::take(&mut index.changes);
    if let Some(token) = &listing.token {
        db::set_meta(conn, "change_token", token)?;
    }
    finish(conn, &changes)?;
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
    match follow(source, conn) {
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
    let colour = average_colour(&image);
    if fetched {
        if let Err(e) = write_beside(&path, &bytes) {
            return ThumbResult::Failed(e.to_string());
        }
    }
    let bytes = if fetched { bytes.len() as u64 } else { 0 };
    ThumbResult::Done { id: job.stored.asset, path: rel, width: w, height: h, colour, bytes }
}

/// What the grid draws until it has decoded the thumbnail.
fn average_colour(image: &image::RgbImage) -> u32 {
    let mut sum = [0u64; 3];
    let mut n = 0u64;
    for p in image.pixels().step_by(7) {
        for k in 0..3 {
            sum[k] += p.0[k] as u64;
        }
        n += 1;
    }
    let n = n.max(1);
    ((sum[0] / n) << 16 | (sum[1] / n) << 8 | (sum[2] / n)) as u32
}

/// Written beside the target and renamed, so the window never reads half
/// a file.
fn write_beside(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    std::fs::create_dir_all(path.parent().expect("thumb folder"))?;
    let tmp = path.with_extension("jpg.part");
    std::fs::write(&tmp, bytes)?;
    std::fs::rename(&tmp, path)
}

/// Where a video's own cover lives (cover.rs), beside the thumbnail it
/// stands in for, named by that thumbnail's key.
pub fn cover_path(key: &str) -> String {
    let thumb = thumb_path(key);
    format!("{}-cover.jpg", thumb.strip_suffix(".jpg").unwrap_or(&thumb))
}

/// The same frame for bigger cells and the viewer, in place of the
/// source's `medium` (iCloud's is of the first frame too), beside the
/// cover: `cover_path` of a cover's thumbnail key.
pub fn medium_cover_path(cover: &str) -> String {
    format!("{}-medium.jpg", cover.strip_suffix(".jpg").unwrap_or(cover))
}

/// The long edge of a medium cover, as a folder's medium.
const MEDIUM_COVER_EDGE: u32 = 2048;

/// A video bigger than this keeps a dull thumbnail rather than be fetched
/// whole for a better one.
const COVER_MAX_BYTES: i64 = 1 << 30;

/// A cover of its own (cover.rs) for each video whose thumbnail shows next
/// to nothing: iCloud makes a video's of its first frame unless an iPhone
/// chose another, and a screen recording or a clip that fades in starts
/// black. The video is fetched as a window would play it, into the library
/// beside its cover, and gone once the cover is made. A cover made stays,
/// even one as dull as the video is, so no video is fetched twice; one
/// already on disk (a library rebuilt) is used as it is. Returns how many
/// thumbnails were replaced.
pub fn make_covers(source: &dyn Source, conn: &mut Connection, library: &Path, stop: &(dyn Fn() -> bool + Sync)) -> Result<usize> {
    struct Dull {
        asset: String,
        cover: String,
        edge: u32,
        video: Stored,
        size: Option<i64>,
    }
    let mut dull = Vec::new();
    {
        let mut stmt = conn.prepare(
            "SELECT a.id, a.thumb_path, t.key, v.name, v.key, coalesce(v.file_type, ''), v.size FROM assets a
             JOIN renditions t ON t.asset_id = a.id AND t.kind = 'thumb'
             JOIN renditions v ON v.asset_id = a.id AND v.kind = 'video'
             WHERE a.is_video AND a.thumb_path IS NOT NULL AND a.thumb_path NOT LIKE '%-cover.jpg'
             ORDER BY a.asset_date DESC",
        )?;
        let rows = stmt.query_map([], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?, r.get::<_, String>(2)?, Stored {
                asset: r.get(0)?,
                want: Want::Video,
                name: r.get(3)?,
                key: r.get(4)?,
                file_type: r.get(5)?,
            }, r.get::<_, Option<i64>>(6)?))
        })?;
        for row in rows {
            let (asset, thumb, key, video, size) = row?;
            let Ok(image) = image::open(library.join(&thumb)) else { continue };
            let image = image.to_rgb8();
            if crate::cover::dull(&image) {
                let edge = image.width().max(image.height());
                dull.push(Dull { asset, cover: cover_path(&key), edge, video, size });
            }
        }
    }
    let mut made = 0;
    for d in dull {
        if stop() {
            return Err(Stopped.into());
        }
        let path = library.join(&d.cover);
        if !path.exists() {
            if d.size.is_some_and(|s| s > COVER_MAX_BYTES) {
                continue;
            }
            if let Err(e) = make_cover(source, conn, &d.video, &path, d.edge) {
                eprintln!("cover for {}: {e:#}", d.asset);
                continue;
            }
        }
        let image = image::open(&path)?.to_rgb8();
        conn.execute(
            "UPDATE assets SET thumb_path = ?2, thumb_w = ?3, thumb_h = ?4, avg_color = ?5 WHERE id = ?1",
            params![d.asset, d.cover, image.width(), image.height(), average_colour(&image)],
        )?;
        made += 1;
    }
    Ok(made)
}

fn make_cover(source: &dyn Source, conn: &Connection, video: &Stored, cover: &Path, edge: u32) -> Result<()> {
    let fetched = cover.with_extension("video.part");
    let file = match source.local(video) {
        Some(file) => file,
        None => {
            let ticket = source.prepare(conn, std::slice::from_ref(video))?.pop().ok_or_else(|| anyhow!("no ticket"))?;
            let ticket = ticket.map_err(|e| anyhow!("{e}"))?;
            std::fs::create_dir_all(cover.parent().expect("thumb folder"))?;
            let mut body = source.open(&ticket, None).map_err(|e| anyhow!("{e}"))?;
            let copied = std::fs::File::create(&fetched).and_then(|mut f| std::io::copy(&mut body.reader, &mut f));
            if let Err(e) = copied {
                let _ = std::fs::remove_file(&fetched);
                return Err(e.into());
            }
            fetched.clone()
        }
    };
    let picked = {
        let _slot = crate::background::slot();
        crate::cover::best(&file, edge)
            .and_then(|(at, thumb)| Ok((thumb, crate::cover::frame(&file, at, MEDIUM_COVER_EDGE)?)))
    };
    let _ = std::fs::remove_file(&fetched);
    let (thumb, medium) = picked?;
    // The medium first: a cover on disk is one done.
    write_beside(Path::new(&medium_cover_path(&cover.to_string_lossy())), &medium)?;
    write_beside(cover, &thumb)?;
    Ok(())
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

    #[test]
    fn a_video_with_a_dull_thumbnail_gets_a_cover_once() {
        use crate::folder::FolderSource;
        use std::process::{Command, Stdio};
        if Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_err() {
            return;
        }
        let dir = std::env::temp_dir().join(format!("pinhole-covers-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let (videos, library) = (dir.join("videos"), dir.join("library"));
        std::fs::create_dir_all(&videos).unwrap();
        std::fs::create_dir_all(&library).unwrap();
        // Black all through: its thumbnail is dull, and so is any cover.
        let status = Command::new("ffmpeg")
            .args(["-v", "error", "-y", "-f", "lavfi", "-i", "color=black:s=64x36:d=3:r=10", "-pix_fmt", "yuv420p"])
            .arg(videos.join("dark.mp4"))
            .status()
            .unwrap();
        assert!(status.success());
        let source = FolderSource::new(videos);
        let mut conn = db::open(&library.join("library.sqlite")).unwrap();
        list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        make_thumbs(&source, &mut conn, &library, 1, &|_, _| {}, &|| false).unwrap();
        assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 1);
        let thumb: String = conn.query_row("SELECT thumb_path FROM assets", [], |r| r.get(0)).unwrap();
        assert!(thumb.ends_with("-cover.jpg") && library.join(&thumb).exists());
        assert!(library.join(medium_cover_path(&thumb)).exists());
        // Made once, never again, however dull.
        assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 0);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
