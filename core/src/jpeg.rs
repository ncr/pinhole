//! A photo's `full` as a JPEG, for whatever reads no HEIC (a chat app, a
//! browser, a phone): ImageMagick at quality 92, which keeps the EXIF
//! (date, place, camera), the colour profile, and turns the pixels upright
//! with the orientation set to match. The window's exports and every
//! plugin ask for it as `jpeg` (service.rs), made once into the cache.
//!
//! Made while someone waits, so at normal priority; but a few at a time,
//! as one ImageMagick of a 48-megapixel HEIC takes every core and a few
//! hundred megabytes, and a window may ask for fifty at once.

use std::path::Path;
use std::process::{Command, Stdio};
use std::sync::{Condvar, Mutex};

use anyhow::{anyhow, Context, Result};

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
    let output = Command::new("magick")
        .args(["-[0]", "-auto-orient", "-background", "white", "-alpha", "remove", "-alpha", "off", "-quality", "92"])
        .arg(format!("jpg:{}", out.display()))
        .stdin(input)
        .stdout(Stdio::null())
        .output()
        .context("running magick")?;
    if !output.status.success() {
        let message = String::from_utf8_lossy(&output.stderr);
        return Err(anyhow!("magick failed: {}", message.lines().next().unwrap_or("")));
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

        // A file that is no picture, or none at all: an error.
        let junk = dir.join("junk.heic");
        std::fs::write(&junk, b"not a photo").unwrap();
        assert!(make(&junk, &dir.join("junk.jpg")).is_err());
        assert!(make(&dir.join("gone.heic"), &dir.join("gone.jpg")).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
