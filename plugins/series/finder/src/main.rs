//! pinhole-series: finds series in a Pinhole library, for the Series
//! plugin. A series is a run of photos of one subject taken close together,
//! the way a moment worth a photo rarely gets just one.
//!
//! Two neighbouring shots belong to one series when they are close in time
//! and look alike; the closer in time, the less alike they need to look, so
//! a burst of a running child stays together while two scenes a minute
//! apart do not. What a shot looks like is a small grey picture and a
//! colour histogram of its thumbnail, made once and kept in the plugin's
//! cache. Only camera shots count (HEIC and JPEG under a camera's name): no
//! videos, screenshots, or images saved from chats and the web.
//!
//!     pinhole-series find <library> <cache>
//!         every series, a line each: its shots' ids, oldest first
//!     pinhole-series steady <library> <id>...
//!         how far each shot is moved against the first, a line each: dx dy,
//!         as fractions of its width and height (a hand-held series wobbles)
//!
//! The library (the index and its thumbnails) is only read. The program
//! runs at the lowest priority there is, so it never slows anything down.

use std::collections::HashMap;
use std::io::Write;
use std::path::Path;
use std::sync::{mpsc, Mutex};

use anyhow::{bail, Context, Result};
use rusqlite::{params, Connection, OpenFlags};

/// Bumped when finding changes, so every library is looked through again.
const VERSION: u32 = 2;
/// Derived pixels/features made with the corrected JPEG SIMD decoder.
const FEATURE_VERSION: i64 = 1;
const MIN_SHOTS: usize = 3;
/// Milliseconds between neighbours, and how alike they must look then.
// A burst: a little likeness is enough. Not none, as photos scanned or
// imported together often share one time.
const BURST_GAP: i64 = 4_000;
const NEAR_GAP: i64 = 30_000; // within this, a modest likeness is enough
const FAR_GAP: i64 = 120_000; // beyond this, never
const BURST_LIKENESS: f32 = 0.5;
const NEAR_LIKENESS: f32 = 0.6;
const FAR_LIKENESS: f32 = 0.8;

/// The small picture's size, on its long side and short side.
const LONG: u32 = 48;
const SHORT: u32 = 36;
/// Hue by saturation bins of the histogram.
const HUES: usize = 12;
const SATURATIONS: usize = 4;

/// What a shot looks like.
#[derive(Clone, Debug, PartialEq)]
struct Feature {
    portrait: bool,
    /// LONG × SHORT grey levels, row by row.
    grey: Vec<u8>,
    /// HUES × SATURATIONS, summing to 1.
    hist: Vec<f32>,
}

impl Feature {
    fn of(image: &image::RgbImage) -> Feature {
        let portrait = image.height() > image.width();
        let (w, h) = if portrait { (SHORT, LONG) } else { (LONG, SHORT) };
        let small = image::imageops::resize(image, w, h, image::imageops::FilterType::Triangle);
        let mut grey = Vec::with_capacity((w * h) as usize);
        let mut hist = vec![0f32; HUES * SATURATIONS];
        for p in small.pixels() {
            let [r, g, b] = p.0.map(|c| c as f32 / 255.0);
            grey.push((0.299 * r + 0.587 * g + 0.114 * b).mul_add(255.0, 0.5) as u8);
            let (hue, saturation) = hue_saturation(r, g, b);
            let hb = ((hue * HUES as f32) as usize).min(HUES - 1);
            let sb = ((saturation * SATURATIONS as f32) as usize).min(SATURATIONS - 1);
            hist[hb * SATURATIONS + sb] += 1.0;
        }
        let total: f32 = hist.iter().sum::<f32>().max(1.0);
        hist.iter_mut().for_each(|v| *v /= total);
        Feature { portrait, grey, hist }
    }

    fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(1 + self.grey.len() + 2 * self.hist.len());
        out.push(self.portrait as u8);
        out.extend_from_slice(&self.grey);
        for v in &self.hist {
            out.extend_from_slice(&((v * 65535.0).round() as u16).to_le_bytes());
        }
        out
    }

    fn decode(bytes: &[u8]) -> Option<Feature> {
        let cells = (LONG * SHORT) as usize;
        if bytes.len() != 1 + cells + 2 * HUES * SATURATIONS {
            return None;
        }
        let hist = bytes[1 + cells..].chunks(2).map(|c| u16::from_le_bytes([c[0], c[1]]) as f32 / 65535.0).collect();
        Some(Feature { portrait: bytes[0] != 0, grey: bytes[1..1 + cells].to_vec(), hist })
    }
}

fn hue_saturation(r: f32, g: f32, b: f32) -> (f32, f32) {
    let max = r.max(g).max(b);
    let min = r.min(g).min(b);
    let delta = max - min;
    if max <= 0.0 || delta <= 0.0 {
        return (0.0, 0.0);
    }
    let hue = if max == r {
        ((g - b) / delta).rem_euclid(6.0)
    } else if max == g {
        (b - r) / delta + 2.0
    } else {
        (r - g) / delta + 4.0
    };
    (hue / 6.0, delta / max)
}

/// 0 for unrelated, 1 for the same picture: how alike the shapes are
/// (the grey pictures' correlation) and the colours (the histograms'
/// overlap).
fn likeness(a: &Feature, b: &Feature) -> f32 {
    if a.portrait != b.portrait {
        return 0.0;
    }
    let normal = |g: &[u8]| -> Vec<f32> {
        let n = g.len() as f32;
        let mean = g.iter().map(|&v| v as f32).sum::<f32>() / n;
        let sd = (g.iter().map(|&v| (v as f32 - mean).powi(2)).sum::<f32>() / n).sqrt() + 1e-6;
        g.iter().map(|&v| (v as f32 - mean) / sd).collect()
    };
    let (ga, gb) = (normal(&a.grey), normal(&b.grey));
    let structure = (ga.iter().zip(&gb).map(|(x, y)| x * y).sum::<f32>() / ga.len() as f32).clamp(0.0, 1.0);
    let colour: f32 = a.hist.iter().zip(&b.hist).map(|(x, y)| x.min(*y)).sum();
    0.4 * structure + 0.6 * colour
}

/// How alike two neighbours `gap` ms apart must look to be one series.
fn needed(gap: i64) -> f32 {
    if gap <= BURST_GAP {
        return BURST_LIKENESS;
    }
    if gap <= NEAR_GAP {
        return NEAR_LIKENESS;
    }
    if gap >= FAR_GAP {
        return f32::INFINITY;
    }
    let t = (gap - NEAR_GAP) as f32 / (FAR_GAP - NEAR_GAP) as f32;
    NEAR_LIKENESS + t * (FAR_LIKENESS - NEAR_LIKENESS)
}

/// A camera's own file name (IMG_1234.HEIC), not one a chat or a browser
/// gave an image it saved (a UUID).
fn camera_shot(item_type: &str, filename: &str) -> bool {
    if !matches!(item_type, "public.heic" | "public.heif" | "public.jpeg") {
        return false;
    }
    let stem = filename.split('.').next().unwrap_or("");
    let uuid = stem.len() >= 36
        && stem.split('-').map(str::len).collect::<Vec<_>>() == [8, 4, 4, 4, 12]
        && stem.chars().all(|c| c == '-' || c.is_ascii_hexdigit());
    !uuid
}

struct Shot {
    id: String,
    date: i64,
    thumb: String,
}

/// Group shots in date order into series: each a run of at least
/// MIN_SHOTS, given as positions into `shots`.
fn runs(shots: &[Shot], features: &HashMap<String, Feature>) -> Vec<Vec<usize>> {
    let mut found = Vec::new();
    let mut run: Vec<usize> = Vec::new();
    for (i, shot) in shots.iter().enumerate() {
        let joined = run.last().is_some_and(|&p| {
            let gap = shot.date - shots[p].date;
            match (features.get(&shots[p].id), features.get(&shot.id)) {
                (Some(a), Some(b)) => gap <= FAR_GAP && a.portrait == b.portrait && likeness(a, b) >= needed(gap),
                _ => false,
            }
        });
        if !joined {
            if run.len() >= MIN_SHOTS {
                found.push(std::mem::take(&mut run));
            }
            run.clear();
        }
        run.push(i);
    }
    if run.len() >= MIN_SHOTS {
        found.push(run);
    }
    found
}


fn main() -> Result<()> {
    lower_priority();
    let args: Vec<String> = std::env::args().skip(1).collect();
    let out = std::io::stdout();
    let mut out = std::io::BufWriter::new(out.lock());
    match args.first().map(String::as_str) {
        Some("find") if args.len() == 3 => {
            for series in find(Path::new(&args[1]), Path::new(&args[2]))? {
                writeln!(out, "{}", series.join(" "))?;
            }
        }
        Some("steady") if args.len() >= 3 => {
            for (dx, dy) in steady(Path::new(&args[1]), &args[2..])? {
                writeln!(out, "{dx:.5} {dy:.5}")?;
            }
        }
        _ => bail!("usage: pinhole-series find <library> <cache> | steady <library> <id>..."),
    }
    out.flush()?;
    Ok(())
}

/// This process at the lowest priority, its threads too: run only when
/// nothing else wants the CPU. Failing is harmless.
fn lower_priority() {
    #[repr(C)]
    struct SchedParam {
        priority: i32,
    }
    extern "C" {
        fn setpriority(which: i32, who: u32, prio: i32) -> i32;
        fn sched_setscheduler(pid: i32, policy: i32, param: *const SchedParam) -> i32;
    }
    // SAFETY: plain system calls on this process.
    unsafe {
        setpriority(0, 0, 19);
        sched_setscheduler(0, 5, &SchedParam { priority: 0 }); // SCHED_IDLE
    }
}

fn library_db(library: &Path) -> Result<Connection> {
    let path = library.join("library.sqlite");
    let db = Connection::open_with_flags(&path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX)
        .with_context(|| format!("opening {}", path.display()))?;
    db.busy_timeout(std::time::Duration::from_secs(10))?;
    Ok(db)
}

/// The likenesses made so far, and the series last found.
fn cache_db(cache: &Path) -> Result<Connection> {
    std::fs::create_dir_all(cache)?;
    let mut db = Connection::open(cache.join("series.sqlite"))?;
    let version: i64 = db.query_row("PRAGMA user_version", [], |r| r.get(0))?;
    if version > FEATURE_VERSION {
        bail!("series cache was made by a newer feature decoder ({version})");
    }
    db.pragma_update(None, "journal_mode", "WAL")?;
    db.execute_batch(
        "CREATE TABLE IF NOT EXISTS likeness (
            asset_id TEXT PRIMARY KEY,
            -- The thumbnail it was made from: a new one (an edit) makes it again.
            thumb TEXT NOT NULL,
            feature BLOB NOT NULL
        ) WITHOUT ROWID;
        CREATE TABLE IF NOT EXISTS found (
            -- What the library was when these were found.
            state TEXT NOT NULL,
            series TEXT NOT NULL
        );",
    )?;
    if version < FEATURE_VERSION {
        // Both tables contain replaceable analysis only. Manual grouping
        // belongs to the plugin's separate state and is never changed here.
        let tx = db.transaction()?;
        tx.execute_batch("DELETE FROM likeness; DELETE FROM found;")?;
        tx.pragma_update(None, "user_version", FEATURE_VERSION)?;
        tx.commit()?;
    }
    Ok(db)
}

/// Every series in the library, each its shots' ids oldest first: as last
/// found, when the library has not changed since.
fn find(library: &Path, cache: &Path) -> Result<Vec<Vec<String>>> {
    let index = library_db(library)?;
    let mut kept = cache_db(cache)?;
    let state: String = {
        let version: Option<String> =
            index.query_row("SELECT value FROM meta WHERE key = 'library_version'", [], |r| r.get(0)).ok();
        let (count, thumbs): (i64, i64) =
            index.query_row("SELECT count(*), count(thumb_path) FROM assets", [], |r| Ok((r.get(0)?, r.get(1)?)))?;
        format!("{VERSION}:{}:{count}:{thumbs}", version.unwrap_or_default())
    };
    let last: Option<(String, String)> = kept.query_row("SELECT state, series FROM found", [], |r| Ok((r.get(0)?, r.get(1)?))).ok();
    if let Some((was, series)) = last {
        if was == state {
            return Ok(series.lines().map(|l| l.split(' ').map(str::to_owned).collect()).collect());
        }
    }

    let shots: Vec<Shot> = {
        let mut stmt = index.prepare(
            "SELECT id, asset_date, item_type, filename, thumb_path FROM assets
             WHERE is_video = 0 AND thumb_path IS NOT NULL AND asset_date IS NOT NULL ORDER BY asset_date, id",
        )?;
        let rows = stmt.query_map([], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?, r.get::<_, Option<String>>(2)?, r.get::<_, Option<String>>(3)?,
                r.get::<_, String>(4)?))
        })?;
        let mut shots = Vec::new();
        for row in rows {
            let (id, date, item_type, filename, thumb) = row?;
            if camera_shot(item_type.as_deref().unwrap_or(""), filename.as_deref().unwrap_or("")) {
                shots.push(Shot { id, date, thumb });
            }
        }
        shots
    };
    // Only a shot with a neighbour near enough can be in a series.
    let near = |i: usize| {
        (i > 0 && shots[i].date - shots[i - 1].date <= FAR_GAP)
            || (i + 1 < shots.len() && shots[i + 1].date - shots[i].date <= FAR_GAP)
    };
    let wanted: Vec<usize> = (0..shots.len()).filter(|&i| near(i)).collect();

    let mut features: HashMap<String, Feature> = HashMap::new();
    {
        let mut stmt = kept.prepare("SELECT asset_id, thumb, feature FROM likeness")?;
        let made: HashMap<String, (String, Vec<u8>)> =
            stmt.query_map([], |r| Ok((r.get(0)?, (r.get(1)?, r.get(2)?))))?.collect::<rusqlite::Result<_>>()?;
        for &i in &wanted {
            if let Some((thumb, bytes)) = made.get(&shots[i].id) {
                if *thumb == shots[i].thumb {
                    if let Some(f) = Feature::decode(bytes) {
                        features.insert(shots[i].id.clone(), f);
                    }
                }
            }
        }
    }
    let missing: Vec<usize> = wanted.iter().copied().filter(|&i| !features.contains_key(&shots[i].id)).collect();
    if !missing.is_empty() {
        let started = std::time::Instant::now();
        let made = make(&shots, &missing, library, &mut kept)?;
        eprintln!("pinhole-series: {} likenesses made in {:.1} s", made.len(), started.elapsed().as_secs_f64());
        features.extend(made);
    }

    let found: Vec<Vec<String>> =
        runs(&shots, &features).into_iter().map(|run| run.into_iter().map(|i| shots[i].id.clone()).collect()).collect();
    let text: Vec<String> = found.iter().map(|s| s.join(" ")).collect();
    let tx = kept.transaction()?;
    tx.execute("DELETE FROM found", [])?;
    tx.execute("INSERT INTO found (state, series) VALUES (?1, ?2)", params![state, text.join("\n")])?;
    tx.commit()?;
    eprintln!("pinhole-series: {} series in {} shots", found.len(), shots.len());
    Ok(found)
}

/// The likeness of each shot at `which`, from its thumbnail, on every core,
/// kept a few hundred at a time, so what is made stays made if the program
/// is stopped.
fn make(shots: &[Shot], which: &[usize], library: &Path, db: &mut Connection) -> Result<HashMap<String, Feature>> {
    let queue = Mutex::new(which.to_vec());
    let (tx, rx) = mpsc::channel::<(usize, Feature)>();
    let mut all = HashMap::new();
    let mut batch: Vec<(usize, Feature)> = Vec::new();
    let keep = |db: &mut Connection, batch: &mut Vec<(usize, Feature)>, all: &mut HashMap<String, Feature>| -> Result<()> {
        let t = db.transaction()?;
        {
            let mut put = t.prepare_cached("INSERT OR REPLACE INTO likeness (asset_id, thumb, feature) VALUES (?1, ?2, ?3)")?;
            for (i, f) in batch.iter() {
                put.execute(params![shots[*i].id, shots[*i].thumb, f.encode()])?;
            }
        }
        t.commit()?;
        all.extend(batch.drain(..).map(|(i, f)| (shots[i].id.clone(), f)));
        Ok(())
    };
    let threads = std::thread::available_parallelism().map_or(1, |n| n.get());
    std::thread::scope(|scope| -> Result<()> {
        for _ in 0..threads {
            let (queue, tx) = (&queue, tx.clone());
            scope.spawn(move || loop {
                let Some(i) = queue.lock().unwrap().pop() else { break };
                let Ok(image) = image::open(library.join(&shots[i].thumb)) else { continue };
                if tx.send((i, Feature::of(&image.to_rgb8()))).is_err() {
                    break;
                }
            });
        }
        drop(tx);
        for made in rx {
            batch.push(made);
            if batch.len() >= 500 {
                keep(db, &mut batch, &mut all)?;
            }
        }
        Ok(())
    })?;
    keep(db, &mut batch, &mut all)?;
    Ok(all)
}

/// How far each shot's picture is moved against the first's, found on
/// their thumbnails: coarsely on small greys, then finely on larger ones
/// around that. A move that does not line the two up clearly better than
/// none (a subject moving rather than the camera) is no move.
fn steady(library: &Path, ids: &[String]) -> Result<Vec<(f32, f32)>> {
    let index = library_db(library)?;
    let mut greys = Vec::new();
    for id in ids {
        let thumb: String = index.query_row("SELECT thumb_path FROM assets WHERE id = ?1", [id], |r| r.get(0))?;
        greys.push(image::open(library.join(thumb))?.to_luma8());
    }
    let Some(first) = greys.first() else { return Ok(Vec::new()) };
    let (w, h) = first.dimensions();
    let at = |g: &image::GrayImage, width: u32| {
        let height = (h as f32 * width as f32 / w as f32).round() as u32;
        image::imageops::resize(g, width, height.max(1), image::imageops::FilterType::Triangle)
    };
    let (coarse, fine) = (64, 256);
    let (first_coarse, first_fine) = (at(first, coarse), at(first, fine));
    let mut shifts = Vec::new();
    for g in &greys {
        if g.dimensions() != (w, h) {
            shifts.push((0.0, 0.0));
            continue;
        }
        let (c, f) = (at(g, coarse), at(g, fine));
        let (cx, cy) = best_shift(&first_coarse, &c, (0, 0), 5);
        let scale = (fine / coarse) as i32;
        let (fx, fy) = best_shift(&first_fine, &f, (cx * scale, cy * scale), scale);
        let still = difference(&first_fine, &f, 0, 0);
        let moved = difference(&first_fine, &f, fx, fy);
        if moved < 0.85 * still {
            shifts.push((fx as f32 / f.width() as f32, fy as f32 / f.height() as f32));
        } else {
            shifts.push((0.0, 0.0));
        }
    }
    Ok(shifts)
}

/// The move of `b`, within `reach` of `around`, that lines it up best with `a`.
fn best_shift(a: &image::GrayImage, b: &image::GrayImage, around: (i32, i32), reach: i32) -> (i32, i32) {
    let mut best = (f32::INFINITY, around);
    for dy in around.1 - reach..=around.1 + reach {
        for dx in around.0 - reach..=around.0 + reach {
            let d = difference(a, b, dx, dy);
            // The smaller move wins a tie.
            if d < best.0 - 1e-6 || (d <= best.0 + 1e-6 && dx.abs() + dy.abs() < best.1 .0.abs() + best.1 .1.abs()) {
                best = (d, (dx, dy));
            }
        }
    }
    best.1
}

/// How unlike `a` and `b` moved by (dx, dy) are: the mean difference over
/// the middle both cover, edges left out so every move is judged alike.
fn difference(a: &image::GrayImage, b: &image::GrayImage, dx: i32, dy: i32) -> f32 {
    let (w, h) = (a.width() as i32, a.height() as i32);
    let margin = (w / 8).max(dx.abs()).max(dy.abs());
    let (mut sum, mut n) = (0u64, 0u64);
    for y in margin..h - margin {
        for x in margin..w - margin {
            let p = a.get_pixel(x as u32, y as u32).0[0] as i32;
            let q = b.get_pixel((x + dx) as u32, (y + dy) as u32).0[0] as i32;
            sum += (p - q).unsigned_abs() as u64;
            n += 1;
        }
    }
    sum as f32 / n.max(1) as f32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn old_jpeg_features_are_forgotten_once_without_touching_other_state() {
        let directory = std::env::temp_dir().join(format!("pinhole-series-decoder-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&directory);
        {
            let db = cache_db(&directory).unwrap();
            db.execute_batch("INSERT INTO likeness VALUES ('old', 'thumb.jpg', x'00');
                INSERT INTO found VALUES ('old-state', 'a b c');
                CREATE TABLE manual_example (value TEXT);
                INSERT INTO manual_example VALUES ('unchanged'); PRAGMA user_version = 0;").unwrap();
        }
        {
            let db = cache_db(&directory).unwrap();
            let count: i64 = db.query_row("SELECT count(*) FROM likeness", [], |r| r.get(0)).unwrap();
            assert_eq!(count, 0);
            let count: i64 = db.query_row("SELECT count(*) FROM found", [], |r| r.get(0)).unwrap();
            assert_eq!(count, 0);
            let manual: String = db.query_row("SELECT value FROM manual_example", [], |r| r.get(0)).unwrap();
            assert_eq!(manual, "unchanged");
            db.execute("INSERT INTO likeness VALUES ('corrected', 'thumb.jpg', x'01')", []).unwrap();
        }
        let db = cache_db(&directory).unwrap();
        let id: String = db.query_row("SELECT asset_id FROM likeness", [], |r| r.get(0)).unwrap();
        assert_eq!(id, "corrected");
        let _ = std::fs::remove_dir_all(directory);
    }

    fn plain(r: u8, g: u8, b: u8, w: u32, h: u32) -> Feature {
        Feature::of(&image::RgbImage::from_pixel(w, h, image::Rgb([r, g, b])))
    }

    fn gradient(w: u32, h: u32, shift: u32) -> Feature {
        Feature::of(&image::RgbImage::from_fn(w, h, |x, y| image::Rgb([((x + shift) * 255 / w) as u8, (y * 255 / h) as u8, 90])))
    }

    #[test]
    fn a_likeness_survives_being_kept() {
        let f = gradient(480, 360, 0);
        let back = Feature::decode(&f.encode()).unwrap();
        assert_eq!(back.grey, f.grey);
        assert!(f.hist.iter().zip(&back.hist).all(|(a, b)| (a - b).abs() < 1e-4));
        assert!(!back.portrait);
        assert!(plain(1, 2, 3, 360, 480).portrait);
    }

    #[test]
    fn the_same_scene_is_alike_and_another_is_not() {
        assert!(likeness(&gradient(480, 360, 0), &gradient(480, 360, 8)) > 0.8);
        assert!(likeness(&gradient(480, 360, 0), &plain(20, 160, 40, 480, 360)) < 0.5);
        // A turned phone is another series.
        assert_eq!(likeness(&gradient(480, 360, 0), &gradient(360, 480, 0)), 0.0);
    }

    #[test]
    fn neighbours_join_by_time_and_likeness() {
        let shot = |id: &str, seconds: i64| Shot { id: id.into(), date: seconds * 1000, thumb: String::new() };
        let shots = vec![
            shot("a", 0), shot("b", 2), shot("c", 3), // a burst
            shot("d", 200),                            // too far
            shot("e", 400), shot("f", 410), shot("g", 425), // alike, near
            shot("h", 430),                            // near, but another scene
        ];
        let mut features = HashMap::new();
        for id in ["a", "b", "c", "d", "e", "f", "g"] {
            features.insert(id.to_string(), gradient(480, 360, 0));
        }
        features.insert("h".into(), plain(20, 160, 40, 480, 360));
        assert_eq!(runs(&shots, &features), vec![vec![0, 1, 2], vec![4, 5, 6]]);
    }

    #[test]
    fn a_moved_picture_is_found_moved() {
        let picture = |shift: i32| {
            image::GrayImage::from_fn(256, 192, |x, y| {
                let (x, y) = (x as i32 - shift, y as i32);
                image::Luma([(((x / 16 + y / 16) % 2) * 160 + (x * 3 + y) % 60) as u8])
            })
        };
        assert_eq!(best_shift(&picture(0), &picture(6), (0, 0), 10), (6, 0));
        assert_eq!(best_shift(&picture(0), &picture(0), (0, 0), 10), (0, 0));
    }

    #[test]
    fn only_a_cameras_shots_count() {
        assert!(camera_shot("public.heic", "IMG_8018.HEIC"));
        assert!(camera_shot("public.jpeg", "DSC01234.JPG"));
        assert!(!camera_shot("public.png", "IMG_0001.PNG"));
        assert!(!camera_shot("public.jpeg", "3F2504E0-4F89-11D3-9A0C-0305E82C3301.jpeg"));
    }
}
