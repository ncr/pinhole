//! Keeping the local library in step with its source, whichever it is: the
//! index (every asset's metadata) and the thumbnails the grid shows.

use std::collections::HashMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{mpsc, Mutex};
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
    // None yet (a library new, or a source that gives none and is listed
    // every round): listed, with nothing to say of it.
    if db::meta(conn, "change_token").is_none() {
        return list(source, conn, progress, stop);
    }
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
/// stale is prepared again, once; a file turned down is not tried again
/// until it changes (source::unreadable). `stop` true gives up with `Stopped`
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
        // Not one whose file was turned down lately and has not changed
        // since (source::unreadable): a damaged file is not made every
        // round.
        let id = source.id();
        for row in rows {
            let stored = row?;
            if crate::source::unreadable(&id, &stored.key).is_none() {
                wanted.push(stored);
            }
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
            // The file's own: not tried again until it changes.
            Err(FetchError::Unreadable(why)) => {
                crate::source::note_unreadable(&source.id(), &job.stored.key, &why);
                return ThumbResult::Failed(why);
            }
            Err(FetchError::Failed(_)) if attempt < 3 => {
                attempt += 1;
                std::thread::sleep(Duration::from_secs(1 << attempt));
            }
            Err(e) => return ThumbResult::Failed(e.to_string()),
        }
    } };

    // The grid draws the average colour until it has decoded the thumbnail,
    // and crops by the real dimensions. In sRGB, as the window shows the
    // thumbnail (colour.rs).
    let image = match image::load_from_memory_with_format(&bytes, image::ImageFormat::Jpeg) {
        Ok(i) => i.to_rgb8(),
        Err(e) => return ThumbResult::Failed(format!("not a JPEG: {e}")),
    };
    let (w, h) = image.dimensions();
    let colour = crate::colour::average_in_srgb(average_colour(&image), crate::colour::jpeg_icc(&bytes).as_deref());
    if fetched {
        if let Err(e) = write_beside(&path, &bytes) {
            return ThumbResult::Failed(e.to_string());
        }
    }
    let bytes = if fetched { bytes.len() as u64 } else { 0 };
    ThumbResult::Done { id: job.stored.asset, path: rel, width: w, height: h, colour, bytes }
}

/// Refresh derived colours after the JPEG SIMD decoder correction. Runs in
/// the sync worker, never on the UI/startup path. Only cached thumbnails are
/// read. Short transactions and a durable per-thumbnail checkpoint let a
/// cancelled repair resume; missing or damaged thumbnails remain pending.
pub fn srgb_averages(conn: &mut Connection, library: &Path, stop: &(dyn Fn() -> bool + Sync)) -> Result<usize> {
    use rayon::prelude::*;
    const DECODER: &str = "srgb-pack-lanes-1";
    if db::meta(conn, "average_colour_decoder").as_deref() == Some(DECODER) {
        return Ok(0);
    }
    conn.execute_batch("CREATE TABLE IF NOT EXISTS jpeg_average_repair (
        asset_id TEXT PRIMARY KEY, thumb_path TEXT NOT NULL, decoder TEXT NOT NULL
    ) WITHOUT ROWID")?;
    let pending = "SELECT a.id, a.thumb_path, a.avg_color FROM assets a
        WHERE a.thumb_path IS NOT NULL AND NOT EXISTS (
          SELECT 1 FROM jpeg_average_repair r
          WHERE r.asset_id = a.id AND r.thumb_path = a.thumb_path AND r.decoder = ?1)
        ORDER BY a.id";
    let rows: Vec<(String, String, Option<u32>)> = conn.prepare(pending)?
        .query_map([DECODER], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?
        .collect::<rusqlite::Result<_>>()?;
    let pool = rayon::ThreadPoolBuilder::new().num_threads(2)
        .thread_name(|i| format!("jpeg-average-{i}"))
        .start_handler(|_| {
            // Linux setpriority with pid 0 applies to the calling thread.
            unsafe extern "C" { fn setpriority(which: i32, who: u32, priority: i32) -> i32; }
            unsafe { setpriority(0, 0, 19); }
        }).build()?;
    let mut changed = 0;
    for batch in rows.chunks(256) {
        let made: Vec<_> = pool.install(|| batch.par_iter().filter_map(|(id, path, was)| {
            if stop() { return None; }
            let bytes = std::fs::read(library.join(path)).ok()?;
            let image = image::load_from_memory_with_format(&bytes, image::ImageFormat::Jpeg).ok()?.to_rgb8();
            let colour = crate::colour::average_in_srgb(average_colour(&image), crate::colour::jpeg_icc(&bytes).as_deref());
            Some((id, path, *was, colour))
        }).collect());
        if stop() { return Err(Stopped.into()); }
        let tx = conn.transaction()?;
        let mut batch_changed = 0;
        {
            let mut update = tx.prepare_cached("UPDATE assets SET avg_color = ?2 WHERE id = ?1")?;
            let mut checkpoint = tx.prepare_cached("INSERT OR REPLACE INTO jpeg_average_repair (asset_id, thumb_path, decoder) VALUES (?1, ?2, ?3)")?;
            for (id, path, was, colour) in made {
                if was != Some(colour) {
                    update.execute(params![id, colour])?;
                    batch_changed += 1;
                }
                checkpoint.execute(params![id, path, DECODER])?;
            }
        }
        if batch_changed > 0 {
            let version = db::meta(&tx, "library_version").and_then(|v| v.parse().ok()).unwrap_or(0i64) + 1;
            db::set_meta(&tx, "library_version", &version.to_string())?;
        }
        tx.commit()?;
        changed += batch_changed;
    }
    let remaining = conn.prepare(pending)?.query([DECODER])?.next()?.is_some();
    if !remaining {
        let tx = conn.transaction()?;
        db::set_meta(&tx, "average_colour", "srgb")?;
        db::set_meta(&tx, "average_colour_decoder", DECODER)?;
        tx.execute_batch("DROP TABLE jpeg_average_repair")?;
        tx.commit()?;
    }
    Ok(changed)
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
/// already on disk (a library rebuilt) is used as it is; and a video
/// ffmpeg turns down is not fetched again until it changes
/// (source::unreadable). Returns how many thumbnails were replaced.
///
/// Covers made before HDR video was tone mapped (cover::VERSION) are made
/// again once, where that is cheap: those of an HLG or PQ video still on
/// this computer, which need no fetching. The rest stay as they are.
/// The camera of each photo not yet read for it whose original (or full
/// size) is on this computer, a folder's file or one fetched into the cache
/// (`at_hand`), from its EXIF: make, model and lens. Read once (found or
/// not in its file); one with no file at hand is looked at again each
/// round, as its original may have been fetched since. At most `most`
/// files a round. How many were read.
pub fn read_cameras(
    conn: &mut Connection,
    at_hand: &dyn Fn(&Stored) -> Option<PathBuf>,
    most: usize,
    stop: &(dyn Fn() -> bool + Sync),
) -> Result<usize> {
    let waiting: Vec<Stored> = conn
        .prepare(
            "SELECT a.id, r.kind, r.name, r.key, coalesce(r.file_type, '') FROM assets a
             JOIN renditions r ON r.asset_id = a.id AND r.kind IN ('original', 'full')
             WHERE NOT a.is_video AND a.camera_read = 0
             ORDER BY a.id, r.kind DESC",
        )?
        .query_map([], |r| {
            let want = if r.get::<_, String>(1)? == "original" { Want::Original } else { Want::Full };
            Ok(Stored { asset: r.get(0)?, want, name: r.get(2)?, key: r.get(3)?, file_type: r.get(4)? })
        })?
        .collect::<rusqlite::Result<_>>()?;
    let mut read: Vec<(String, Camera)> = Vec::new();
    let mut done = std::collections::HashSet::new();
    for stored in waiting {
        if read.len() >= most || stop() {
            break;
        }
        // Its original first ("original" sorts after "full", DESC), then
        // its full size if the original is not here.
        if done.contains(&stored.asset) {
            continue;
        }
        let Some(file) = at_hand(&stored) else { continue };
        done.insert(stored.asset.clone());
        read.push((stored.asset, camera_of(&file)));
    }
    let tx = conn.transaction()?;
    {
        let mut set = tx.prepare(
            "UPDATE assets SET camera_make = ?2, camera_model = ?3, lens_model = ?4, camera_read = 1 WHERE id = ?1",
        )?;
        for (asset, camera) in &read {
            set.execute(params![asset, camera.make, camera.model, camera.lens])?;
        }
    }
    tx.commit()?;
    Ok(read.len())
}

/// What a photo's EXIF says of its camera.
#[derive(Debug, Default, PartialEq)]
pub struct Camera {
    pub make: Option<String>,
    pub model: Option<String>,
    pub lens: Option<String>,
}

pub fn camera_of(file: &Path) -> Camera {
    let Ok(f) = std::fs::File::open(file) else { return Camera::default() };
    let Ok(exif) = exif::Reader::new().read_from_container(&mut std::io::BufReader::new(f)) else {
        return Camera::default();
    };
    let text = |tag| {
        exif.get_field(tag, exif::In::PRIMARY)
            .and_then(|f| match &f.value {
                exif::Value::Ascii(parts) => parts.first().map(|b| String::from_utf8_lossy(b).trim_matches(char::from(0)).trim().to_owned()),
                _ => None,
            })
            .filter(|s| !s.is_empty())
    };
    Camera { make: text(exif::Tag::Make), model: text(exif::Tag::Model), lens: text(exif::Tag::LensModel) }
}

pub fn make_covers(source: &dyn Source, conn: &mut Connection, library: &Path, stop: &(dyn Fn() -> bool + Sync)) -> Result<usize> {
    if db::meta(conn, "covers").as_deref() != Some(crate::cover::VERSION) {
        let made: Vec<(String, String, String, Stored)> = conn
            .prepare(
                "SELECT a.id, a.thumb_path, t.key, v.name, v.key, coalesce(v.file_type, '') FROM assets a
                 JOIN renditions t ON t.asset_id = a.id AND t.kind = 'thumb'
                 JOIN renditions v ON v.asset_id = a.id AND v.kind = 'video'
                 WHERE a.is_video AND a.thumb_path LIKE '%-cover.jpg'",
            )?
            .query_map([], |r| {
                Ok((r.get(0)?, r.get(1)?, r.get(2)?, Stored { asset: r.get(0)?, want: Want::Video, name: r.get(3)?, key: r.get(4)?, file_type: r.get(5)? }))
            })?
            .collect::<rusqlite::Result<_>>()?;
        for (asset, cover, key, video) in made {
            if stop() {
                return Err(Stopped.into());
            }
            let Some(file) = source.local(&video).filter(|f| f.exists()) else { continue };
            if !crate::cover::is_hdr(&file)? {
                continue;
            }
            // Its own thumbnail again, dull: its cover is made anew below.
            let _ = std::fs::remove_file(library.join(medium_cover_path(&cover)));
            let _ = std::fs::remove_file(library.join(&cover));
            conn.execute("UPDATE assets SET thumb_path = ?2 WHERE id = ?1", params![asset, thumb_path(&key)])?;
        }
        db::set_meta(conn, "covers", crate::cover::VERSION)?;
    }
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
            // A video ffmpeg turned down lately (damaged, cut short) is
            // not fetched again for it until it changes.
            let id = source.id();
            if crate::source::unreadable(&id, &d.video.key).is_some() {
                continue;
            }
            if let Err(e) = make_cover(source, conn, &d.video, &path, d.edge) {
                eprintln!("cover for {}: {e:#}", d.asset);
                if crate::render::rejected(&e) {
                    crate::source::note_unreadable(&id, &d.video.key, &format!("{e:#}"));
                }
                continue;
            }
        }
        let image = image::open(&path)?.to_rgb8();
        let icc = crate::colour::jpeg_icc_of_file(&path).ok().flatten();
        conn.execute(
            "UPDATE assets SET thumb_path = ?2, thumb_w = ?3, thumb_h = ?4, avg_color = ?5 WHERE id = ?1",
            params![d.asset, d.cover, image.width(), image.height(), crate::colour::average_in_srgb(average_colour(&image), icc.as_deref())],
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
    fn a_photos_camera_is_read_from_its_file_at_hand_once() {
        let dir = std::env::temp_dir().join(format!("pinhole-cameras-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        // A JPEG with the EXIF of a phone's camera.
        let mut fields = Vec::new();
        for (tag, text) in [(exif::Tag::Make, "Apple"), (exif::Tag::Model, "iPhone 15 Pro")] {
            fields.push(exif::Field { tag, ifd_num: exif::In::PRIMARY, value: exif::Value::Ascii(vec![text.as_bytes().to_vec()]) });
        }
        let lens = exif::Field { tag: exif::Tag::LensModel, ifd_num: exif::In::PRIMARY,
                                 value: exif::Value::Ascii(vec![b"iPhone 15 Pro back camera".to_vec()]) };
        let mut writer = exif::experimental::Writer::new();
        for field in fields.iter().chain([&lens]) {
            writer.push_field(field);
        }
        let mut tiff = std::io::Cursor::new(Vec::new());
        writer.write(&mut tiff, false).unwrap();
        let tiff = tiff.into_inner();
        let mut plain = Vec::new();
        image::codecs::jpeg::JpegEncoder::new(&mut plain)
            .encode(&[200, 30, 40].repeat(64), 8, 8, image::ExtendedColorType::Rgb8).unwrap();
        let mut tagged = plain[..2].to_vec();
        tagged.extend([0xff, 0xe1]);
        tagged.extend(((2 + 6 + tiff.len()) as u16).to_be_bytes());
        tagged.extend(b"Exif\0\0");
        tagged.extend(&tiff);
        tagged.extend(&plain[2..]);
        std::fs::write(dir.join("phone.jpg"), &tagged).unwrap();
        std::fs::write(dir.join("plain.jpg"), &plain).unwrap();
        let mut conn = db::open(&dir.join("library.sqlite")).unwrap();
        conn.execute_batch(
            "INSERT INTO assets (id) VALUES ('phone'), ('plain'), ('away');
             INSERT INTO renditions (asset_id, kind, name, key) VALUES
               ('phone', 'original', 'phone.jpg', 'k1'), ('plain', 'full', 'plain.jpg', 'k2'), ('away', 'original', 'x.jpg', 'k3');",
        ).unwrap();
        let here = |stored: &Stored| Some(dir.join(&stored.name)).filter(|p| p.exists());
        assert_eq!(read_cameras(&mut conn, &here, 100, &|| false).unwrap(), 2);
        let camera = |conn: &Connection, id: &str| -> (Option<String>, Option<String>, Option<String>, bool) {
            conn.query_row("SELECT camera_make, camera_model, lens_model, camera_read FROM assets WHERE id = ?1", [id],
                           |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?))).unwrap()
        };
        assert_eq!(camera(&conn, "phone"), (Some("Apple".into()), Some("iPhone 15 Pro".into()), Some("iPhone 15 Pro back camera".into()), true));
        // Read, with nothing to say: not read again.
        assert_eq!(camera(&conn, "plain"), (None, None, None, true));
        // No file of it here: looked at again once there is one.
        assert!(!camera(&conn, "away").3);
        assert_eq!(read_cameras(&mut conn, &here, 100, &|| false).unwrap(), 0);
        std::fs::copy(dir.join("phone.jpg"), dir.join("x.jpg")).unwrap();
        assert_eq!(read_cameras(&mut conn, &here, 100, &|| false).unwrap(), 1);
        assert_eq!(camera(&conn, "away").1.as_deref(), Some("iPhone 15 Pro"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn jpeg_averages_are_repaired_once_and_missing_thumbnails_retry() {
        let dir = std::env::temp_dir().join(format!("pinhole-averages-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("thumbs")).unwrap();
        let p3 = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../app/tests/data/display-p3.icc")).unwrap();
        let mut plain = Vec::new();
        image::codecs::jpeg::JpegEncoder::new(&mut plain)
            .encode(&[200, 30, 40].repeat(64), 8, 8, image::ExtendedColorType::Rgb8).unwrap();
        let mut tagged = plain[..2].to_vec();
        tagged.extend([0xff, 0xe2]);
        tagged.extend(((2 + 14 + p3.len()) as u16).to_be_bytes());
        tagged.extend(b"ICC_PROFILE\0");
        tagged.extend([1, 1]);
        tagged.extend(&p3);
        tagged.extend(&plain[2..]);
        std::fs::write(dir.join("thumbs/p3.jpg"), &tagged).unwrap();
        std::fs::write(dir.join("thumbs/plain.jpg"), &plain).unwrap();
        std::fs::write(dir.join("thumbs/ramp.jpg"), include_bytes!("../../docs/reviews/jpeg-repro/gray-ramp.jpg")).unwrap();
        std::fs::write(dir.join("thumbs/bad.jpg"), b"broken JPEG").unwrap();
        let mut conn = db::open(&dir.join("library.sqlite")).unwrap();
        let wrong = 0x123456u32;
        for id in ["p3", "plain", "ramp", "gone", "bad"] {
            conn.execute("INSERT INTO assets (id, thumb_path, avg_color, asset_date, added_date, favorite, filename)
                VALUES (?1, ?2, ?3, 123, 456, 1, 'unchanged.jpg')", params![id, format!("thumbs/{id}.jpg"), wrong]).unwrap();
        }
        db::set_meta(&conn, "average_colour", "srgb").unwrap(); // Already migrated, but decoded by old Blaze.
        let colour = |conn: &Connection, id: &str| -> u32 {
            conn.query_row("SELECT avg_color FROM assets WHERE id = ?1", [id], |r| r.get(0)).unwrap()
        };
        assert!(srgb_averages(&mut conn, &dir, &|| true).is_err());
        assert_eq!(colour(&conn, "p3"), wrong);
        assert_eq!(srgb_averages(&mut conn, &dir, &|| false).unwrap(), 3);
        let decoded = image::load_from_memory(&plain).unwrap().to_rgb8();
        let red = average_colour(&decoded);
        assert_eq!(colour(&conn, "plain"), red);
        assert_eq!(colour(&conn, "p3"), crate::colour::average_in_srgb(red, crate::colour::jpeg_icc(&tagged).as_deref()));
        // Independent Pillow decode, sampling every seventh pixel, averages to 126.
        assert_eq!(colour(&conn, "ramp"), 0x7e7e7e);
        assert_eq!(colour(&conn, "gone"), wrong);
        assert_eq!(db::meta(&conn, "average_colour_decoder"), None);
        assert_eq!(db::meta(&conn, "library_version").as_deref(), Some("1"));
        // Successful rows are checkpointed even when other thumbnails are offline.
        std::fs::remove_file(dir.join("thumbs/plain.jpg")).unwrap();
        assert_eq!(srgb_averages(&mut conn, &dir, &|| false).unwrap(), 0);
        std::fs::write(dir.join("thumbs/gone.jpg"), &plain).unwrap();
        conn.execute("DELETE FROM assets WHERE id = 'bad'", []).unwrap();
        assert_eq!(srgb_averages(&mut conn, &dir, &|| false).unwrap(), 1);
        assert_eq!(colour(&conn, "gone"), red);
        assert_eq!(db::meta(&conn, "average_colour_decoder").as_deref(), Some("srgb-pack-lanes-1"));
        assert_eq!(srgb_averages(&mut conn, &dir, &|| false).unwrap(), 0);
        let unchanged: i64 = conn.query_row("SELECT count(*) FROM assets WHERE asset_date=123 AND added_date=456 AND favorite=1 AND filename='unchanged.jpg'", [], |r| r.get(0)).unwrap();
        assert_eq!(unchanged, 4);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn cancelled_jpeg_repair_resumes_after_its_committed_batch() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let dir = std::env::temp_dir().join(format!("pinhole-averages-batches-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("ramp.jpg"), include_bytes!("../../docs/reviews/jpeg-repro/gray-ramp.jpg")).unwrap();
        let mut conn = db::open(&dir.join("library.sqlite")).unwrap();
        for id in 0..270 {
            conn.execute("INSERT INTO assets (id, thumb_path, avg_color) VALUES (?1, 'ramp.jpg', 0)", [format!("{id:04}")]).unwrap();
        }
        let calls = AtomicUsize::new(0);
        assert!(srgb_averages(&mut conn, &dir, &|| calls.fetch_add(1, Ordering::SeqCst) >= 257).is_err());
        let repaired: i64 = conn.query_row("SELECT count(*) FROM assets WHERE avg_color = 8289918", [], |r| r.get(0)).unwrap();
        assert_eq!(repaired, 256);
        assert_eq!(db::meta(&conn, "average_colour_decoder"), None);
        assert_eq!(srgb_averages(&mut conn, &dir, &|| false).unwrap(), 14);
        assert_eq!(db::meta(&conn, "average_colour_decoder").as_deref(), Some("srgb-pack-lanes-1"));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
