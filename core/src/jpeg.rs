//! A photo's `full` as a JPEG, for whatever reads no HEIC (a chat app, a
//! browser, a phone): ImageMagick at quality 92, which keeps the EXIF
//! (date, place, camera), the colour profile, and turns the pixels upright
//! with the orientation set to match. The window's exports and every
//! plugin ask for it as `jpeg` (service.rs), made once into the cache.
//!
//! A HEIC's HDR goes with it: its gain map, Apple's or ISO 21496-1's, is
//! added as an Ultra HDR JPEG's (gainmap.rs), which Android, Chrome and Apple
//! show above SDR white; anything else sees the same JPEG as before.
//!
//! Made while someone waits, so at normal priority; but a few at a time,
//! as one ImageMagick of a 48-megapixel HEIC takes every core and a few
//! hundred megabytes, and a window may ask for fifty at once.

use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::Command;
use std::sync::{Condvar, Mutex};

use anyhow::{anyhow, Context, Result};

/// In the cached file's name (service.rs): "2" since JPEGs keep a HEIC's
/// gain map.
pub const VERSION: &str = "2";

/// ImageMagicks running at once.
const AT_ONCE: usize = 2;

static RUNNING: Mutex<usize> = Mutex::new(0);
static FREED: Condvar = Condvar::new();

/// `full` as a JPEG at `out`. Read from stdin, so no name of a folder's
/// file is taken for ImageMagick's own syntax (`[0]`, `*`, `format:`);
/// only its first image (an animated GIF's first frame); turned upright
/// as its orientation says (a TIFF's, a PNG's or a WebP's; a HEIC comes
/// upright from its decoder, which says so); see-through parts on white,
/// not black.
pub fn make(full: &Path, out: &Path) -> Result<()> {
    let input = std::fs::File::open(full).with_context(|| format!("opening {}", full.display()))?;
    let _turn = Turn::wait();
    let mut magick = Command::new("magick");
    magick
        .args(["-[0]", "-auto-orient", "-background", "white", "-alpha", "remove", "-alpha", "off", "-quality", "92"])
        .arg(format!("jpg:{}", out.display()))
        .stdin(input)
        // A group of its own, as background work's: stopped with whatever
        // it starts.
        .process_group(0);
    // Stopped if it hangs (a folder's file on a mount that has gone) or
    // spins, as background work is, so it never holds a turn for good.
    let output = crate::background::output(&mut magick).context("running magick")?;
    if !output.status.success() {
        let message = String::from_utf8_lossy(&output.stderr);
        return Err(anyhow!("magick failed: {}", message.lines().next().unwrap_or("")));
    }
    // A HEIC's gain map, kept: the JPEG as it is, with the map beside it.
    // Without one, or should it fail, the JPEG is as it always was.
    if let Some(map) = crate::gainmap::read(full) {
        let ultra = std::fs::read(out).map_err(anyhow::Error::from).and_then(|base| crate::gainmap::attach(&base, &map));
        match ultra {
            Ok(ultra) => std::fs::write(out, ultra).with_context(|| format!("writing {}", out.display()))?,
            Err(e) => eprintln!("pinholed: {}'s gain map is not kept in its JPEG: {e:#}", full.display()),
        }
    }
    Ok(())
}

/// One of the `AT_ONCE` places, given back when dropped.
struct Turn;

impl Turn {
    fn wait() -> Turn {
        let mut running = RUNNING.lock().unwrap_or_else(|e| e.into_inner());
        while *running >= AT_ONCE {
            running = FREED.wait(running).unwrap_or_else(|e| e.into_inner());
        }
        *running += 1;
        Turn
    }
}

impl Drop for Turn {
    fn drop(&mut self) {
        *RUNNING.lock().unwrap_or_else(|e| e.into_inner()) -= 1;
        FREED.notify_one();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Stdio;

    fn has_magick() -> bool {
        Command::new("magick").arg("-version").stdout(Stdio::null()).status().is_ok_and(|s| s.success())
    }

    fn identify(path: &Path, format: &str) -> String {
        let out = Command::new("magick").args(["identify", "-format", format]).arg(path).output().unwrap();
        String::from_utf8_lossy(&out.stdout).into_owned()
    }

    #[test]
    fn any_photo_becomes_a_jpeg() {
        if !has_magick() {
            return;
        }
        let dir = std::env::temp_dir().join(format!("pinhole-jpeg-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let magick = |args: &[&str], to: &Path| {
            assert!(Command::new("magick").args(args).arg(to).status().unwrap().success());
        };
        let out = dir.join("out.jpg");

        // A name ImageMagick would read as its own syntax, were it given.
        let named = dir.join("IMG [1]*.png");
        magick(&["-size", "64x48", "xc:#c83232"], &named);
        make(&named, &out).unwrap();
        assert!(std::fs::read(&out).unwrap().starts_with(&[0xFF, 0xD8]));
        assert_eq!(identify(&out, "%m %wx%h"), "JPEG 64x48");

        // An animated GIF's first frame, one file.
        let gif = dir.join("moving.gif");
        magick(&["-size", "8x8", "xc:red", "xc:blue", "-loop", "0"], &gif);
        make(&gif, &out).unwrap();
        assert_eq!(identify(&out, "%n %[fx:round(p{4,4}.r*9)]%[fx:round(p{4,4}.b*9)]"), "1 90");
        assert!(!dir.join("out-1.jpg").exists());

        // See-through on white, not black.
        let clear = dir.join("clear.png");
        magick(&["-size", "8x8", "xc:none"], &clear);
        make(&clear, &out).unwrap();
        assert_eq!(identify(&out, "%[fx:round(minima*9)]"), "9");

        // Turned upright: a TIFF that says its top is on the right
        // (orientation 6), red on the left as stored, comes out portrait
        // with red at the top, and says it is upright.
        let tiff = dir.join("sideways.tif");
        magick(&["-size", "32x48", "xc:red", "-size", "32x48", "xc:blue", "+append", "-orient", "RightTop"], &tiff);
        assert_eq!(identify(&tiff, "%wx%h %[orientation]"), "64x48 RightTop");
        make(&tiff, &out).unwrap();
        assert_eq!(identify(&out, "%wx%h %[fx:round(p{24,4}.r*9)]%[fx:round(p{24,60}.b*9)]"), "48x64 99");
        assert!(matches!(identify(&out, "%[orientation]").as_str(), "TopLeft" | "Undefined"));
        // A HEIC turned by its own transform, as an iPhone's portrait is:
        // its decoder turns it, and it is not turned again.
        let heic = dir.join("portrait.heic");
        let png = dir.join("landscape.png");
        magick(&["-size", "32x48", "xc:red", "-size", "32x48", "xc:blue", "+append"], &png);
        let encoded = Command::new("heif-enc").args(["-q", "90", "--rotate-cw", "90"]).arg(&png).arg("-o").arg(&heic)
            .stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success());
        if encoded {
            assert_eq!(identify(&heic, "%wx%h"), "48x64");
            make(&heic, &out).unwrap();
            assert_eq!(identify(&out, "%wx%h %[fx:round(p{24,4}.r*9)]%[fx:round(p{24,60}.b*9)]"), "48x64 99");
        }

        // A HEIC without a gain map: no second image, as before.
        if encoded {
            assert!(crate::gainmap::probe(&std::fs::read(&out).unwrap()).is_err());
        }

        // A file that is no picture, or none at all: an error.
        let junk = dir.join("junk.heic");
        std::fs::write(&junk, b"not a photo").unwrap();
        assert!(make(&junk, &dir.join("junk.jpg")).is_err());
        assert!(make(&dir.join("gone.heic"), &dir.join("gone.jpg")).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// The APP segments of a JPEG up to its first scan, and that scan on.
    fn segments(jpeg: &[u8]) -> (Vec<(u8, Vec<u8>)>, Vec<u8>) {
        let mut at = 2;
        let mut out = Vec::new();
        while at + 4 <= jpeg.len() && jpeg[at] == 0xFF && jpeg[at + 1] != 0xDA {
            let length = u16::from_be_bytes([jpeg[at + 2], jpeg[at + 3]]) as usize;
            out.push((jpeg[at + 1], jpeg[at + 4..at + 2 + length].to_vec()));
            at += 2 + length;
        }
        (out, jpeg[at..].to_vec())
    }

    #[test]
    fn a_heics_gain_map_goes_with_its_jpeg() {
        // PINHOLE_HDR_HEIC: an iPhone's HEIC with a gain map, read only.
        // Skipped without.
        let Ok(heic) = std::env::var("PINHOLE_HDR_HEIC") else { return };
        if !has_magick() {
            return;
        }
        let dir = std::env::temp_dir().join(format!("pinhole-jpeg-hdr-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let heic = Path::new(&heic);
        let out = dir.join("out.jpg");
        make(heic, &out).unwrap();
        let ultra = std::fs::read(&out).unwrap();
        // Today's JPEG, as ImageMagick makes it.
        let plain = dir.join("plain.jpg");
        let status = Command::new("magick")
            .args(["-[0]", "-auto-orient", "-background", "white", "-alpha", "remove", "-alpha", "off", "-quality", "92"])
            .arg(format!("jpg:{}", plain.display()))
            .stdin(std::fs::File::open(heic).unwrap())
            .status()
            .unwrap();
        assert!(status.success());
        let plain = std::fs::read(&plain).unwrap();

        // libultrahdr reads its map back: the HEIC's, as JPEG at 90 keeps it.
        let map = crate::gainmap::read(heic).expect("the HEIC has a gain map");
        let (back, meta, primary) = crate::gainmap::probe(&ultra).unwrap();
        // Its metadata as written, to the fractions it is kept in.
        let close = |a: &[f32], b: &[f32]| a.iter().zip(b).all(|(x, y)| (x - y).abs() <= 1e-4 * x.abs().max(1.0));
        assert!(
            close(&meta.max_content_boost, &map.meta.max_content_boost)
                && close(&meta.min_content_boost, &map.meta.min_content_boost)
                && close(&meta.gamma, &map.meta.gamma)
                && close(&meta.offset_sdr, &map.meta.offset_sdr)
                && close(&meta.offset_hdr, &map.meta.offset_hdr)
                && close(&[meta.hdr_capacity_min, meta.hdr_capacity_max], &[map.meta.hdr_capacity_min, map.meta.hdr_capacity_max])
                && meta.use_base_cg == map.meta.use_base_cg,
            "{meta:?} against {:?}",
            map.meta
        );
        let back = if map.channels == 3 { back.to_rgb8().into_raw() } else { back.to_luma8().into_raw() };
        assert_eq!(back.len(), map.pixels.len());
        let worst = back.iter().zip(&map.pixels).map(|(a, b)| (*a as i32 - *b as i32).abs()).max().unwrap();
        let mean = back.iter().zip(&map.pixels).map(|(a, b)| (*a as i32 - *b as i32).abs() as f64).sum::<f64>() / back.len() as f64;
        let within = back.iter().zip(&map.pixels).filter(|(a, b)| (**a as i32 - **b as i32).abs() <= 4).count() as f64 / back.len() as f64;
        eprintln!(
            "{}: {}'s map {}x{}, {} channel(s), up to {:.2} stops; read back mean {mean:.3}, {:.3} % within 4, worst {worst} of 255; {} bytes, today's {}",
            heic.display(),
            map.kind,
            map.width,
            map.height,
            map.channels,
            meta.hdr_capacity_max.log2(),
            100.0 * within,
            ultra.len(),
            plain.len()
        );
        assert!(mean < 1.0 && within > 0.995 && worst < 32);
        // Its primary is today's JPEG: every segment of it there still (the
        // EXIF, the profile), libultrahdr's own added, and the very scan.
        let (theirs, scan) = segments(&primary);
        let (today, today_scan) = segments(&plain);
        assert_eq!(scan, today_scan);
        for segment in &today {
            assert!(theirs.contains(segment), "segment {:02x} kept", segment.0);
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}
