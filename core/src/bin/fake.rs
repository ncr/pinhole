//! pinhole-fake: a synthetic library for working on the grid without an
//! Apple account. It writes the same index the real sync writes, and
//! thumbnails shaped like iCloud's: JPEG, 480 px on the long edge.

use std::path::{Path, PathBuf};
use std::time::Instant;

use anyhow::Result;
use clap::Parser;
use image::codecs::jpeg::JpegEncoder;
use image::{ExtendedColorType, RgbImage};
use rand::rngs::SmallRng;
use rand::{Rng, SeedableRng};
use rayon::prelude::*;
use rusqlite::params;

use pinhole_core::db;

#[derive(Parser)]
struct Cli {
    /// How many assets.
    #[arg(long, default_value_t = 100_000)]
    count: usize,
    /// Where the library goes: library.sqlite and thumbs/.
    #[arg(long)]
    out: Option<PathBuf>,
    #[arg(long, default_value_t = 7)]
    seed: u64,
}

/// One fake asset, before its thumbnail is drawn.
struct Spec {
    id: String,
    date_ms: i64,
    width: u32,
    height: u32,
    video: Option<f64>,
    live: bool,
    favorite: bool,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let out = cli.out.unwrap_or_else(|| {
        PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/share/pinhole/fake")
    });
    std::fs::create_dir_all(out.join("thumbs"))?;
    let specs = plan(cli.count, cli.seed);

    let started = Instant::now();
    let done = std::sync::atomic::AtomicUsize::new(0);
    let colors: Vec<(String, u32, u64)> = specs
        .par_iter()
        .map(|s| {
            let rel = thumb_path(&s.id);
            let (color, bytes) = draw(s, &out.join(&rel), cli.seed).expect("writing thumbnail");
            let n = done.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
            if n % 10_000 == 0 {
                eprintln!("{n} thumbnails, {:.0}/s", n as f64 / started.elapsed().as_secs_f64());
            }
            (rel, color, bytes)
        })
        .collect();
    let bytes: u64 = colors.iter().map(|c| c.2).sum();
    eprintln!("{} thumbnails in {:.0}s, {:.2} GB, {:.0} KB each",
        specs.len(), started.elapsed().as_secs_f64(), bytes as f64 / 1e9, bytes as f64 / specs.len() as f64 / 1e3);

    let db_path = out.join("library.sqlite");
    let _ = std::fs::remove_file(&db_path);
    let mut conn = db::open(&db_path)?;
    let tx = conn.transaction()?;
    {
        let mut stmt = tx.prepare(
            "INSERT INTO assets (id, rank, asset_record, item_type, filename, asset_date, added_date, favorite,
                duration, is_video, is_live, thumb_path, thumb_w, thumb_h, avg_color)
             VALUES (?1, ?2, ?1, ?3, ?4, ?5, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)",
        )?;
        for (i, (s, (rel, color, _))) in specs.iter().zip(&colors).enumerate() {
            let item_type = if s.video.is_some() { "com.apple.quicktime-movie" } else { "public.heic" };
            let filename = format!("IMG_{:04}.{}", i % 10_000, if s.video.is_some() { "MOV" } else { "HEIC" });
            stmt.execute(params![
                s.id, i as i64, item_type, filename, s.date_ms, s.favorite, s.video, s.video.is_some(), s.live,
                rel, s.width, s.height, *color as i64,
            ])?;
        }
    }
    tx.commit()?;
    db::set_meta(&conn, "fake_seed", &cli.seed.to_string())?;
    println!("{} assets in {}", specs.len(), out.display());
    Ok(())
}

/// Dates, shapes and kinds in roughly the proportions of a real library:
/// more photos each year, mostly 4:3, some video, many Live Photos.
fn plan(count: usize, seed: u64) -> Vec<Spec> {
    let mut rng = SmallRng::seed_from_u64(seed);
    const START: i64 = 978_307_200_000; // 2001-01-01
    const SPAN: f64 = 25.8 * 365.25 * 86_400_000.0;
    let mut dates: Vec<i64> = (0..count)
        // Square root skews the dates late: a library grows faster over time.
        .map(|_| START + (rng.gen::<f64>().sqrt() * SPAN) as i64)
        .collect();
    dates.sort_unstable();
    dates
        .into_iter()
        .enumerate()
        .map(|(i, date_ms)| {
            let (width, height) = match rng.gen_range(0..100) {
                0..=49 => (480, 360),
                50..=74 => (360, 480),
                75..=84 => (480, 270),
                85..=92 => (270, 480),
                _ => (480, 480),
            };
            let video = (rng.gen_range(0..100) < 6).then(|| rng.gen_range(3.0..300.0));
            Spec {
                id: format!("fake-{i:06}"),
                date_ms,
                width,
                height,
                live: video.is_none() && rng.gen_range(0..100) < 28,
                favorite: rng.gen_range(0..1000) < 12,
                video,
            }
        })
        .collect()
}

/// Thumbnails spread over 256 folders so no directory gets huge.
fn thumb_path(id: &str) -> String {
    let hash = id.bytes().fold(0u32, |h, b| h.wrapping_mul(31).wrapping_add(b as u32));
    format!("thumbs/{:02x}/{id}.jpg", hash & 0xff)
}

/// Draw a soft gradient with a few blurred discs and some grain, so the
/// JPEG comes out about the size of a real photo's thumbnail. Returns the
/// average colour and the file size.
fn draw(spec: &Spec, path: &Path, seed: u64) -> Result<(u32, u64)> {
    let index: u64 = spec.id[5..].parse().unwrap_or(0);
    let mut rng = SmallRng::seed_from_u64(seed ^ index.wrapping_mul(0x9E37_79B9_7F4A_7C15));
    let (w, h) = (spec.width, spec.height);
    let top: [f32; 3] = [rng.gen(), rng.gen(), rng.gen()];
    let bottom: [f32; 3] = [rng.gen(), rng.gen(), rng.gen()];
    let discs: Vec<(f32, f32, f32, [f32; 3])> = (0..rng.gen_range(3..7))
        .map(|_| (rng.gen::<f32>() * w as f32, rng.gen::<f32>() * h as f32, rng.gen_range(20.0..160.0), [rng.gen(), rng.gen(), rng.gen()]))
        .collect();

    let mut img = RgbImage::new(w, h);
    let mut sum = [0u64; 3];
    for (x, y, px) in img.enumerate_pixels_mut() {
        let t = y as f32 / h as f32;
        let mut c = [0f32; 3];
        for k in 0..3 {
            c[k] = top[k] * (1.0 - t) + bottom[k] * t;
        }
        for (cx, cy, r, col) in &discs {
            let d = ((x as f32 - cx).powi(2) + (y as f32 - cy).powi(2)).sqrt();
            let a = (1.0 - (d - r) / 24.0).clamp(0.0, 1.0) * 0.85;
            for k in 0..3 {
                c[k] = c[k] * (1.0 - a) + col[k] * a;
            }
        }
        let grain: f32 = rng.gen_range(-0.1..0.1);
        for k in 0..3 {
            let v = ((c[k] + grain).clamp(0.0, 1.0) * 255.0) as u8;
            px.0[k] = v;
            sum[k] += v as u64;
        }
    }
    let n = (w * h) as u64;
    let avg = ((sum[0] / n) << 16 | (sum[1] / n) << 8 | (sum[2] / n)) as u32;

    std::fs::create_dir_all(path.parent().expect("thumb folder"))?;
    let mut buf = Vec::with_capacity(64 * 1024);
    JpegEncoder::new_with_quality(&mut buf, 82).encode(img.as_raw(), w, h, ExtendedColorType::Rgb8)?;
    std::fs::write(path, &buf)?;
    Ok((avg, buf.len() as u64))
}
