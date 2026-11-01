//! Thumbnails and mediums made from a file on this computer: a folder's
//! (pinhole.folder), and a provider's whose renditions say `"make": true`
//! (process.rs, docs/source-providers.md). A photo is made smaller with
//! ImageMagick, a video's from its most telling frame (cover.rs).

use std::fs::File;
use std::path::Path;

use anyhow::{anyhow, Context, Result};

/// Long edges of the thumbnail (kept in the library, as iCloud's are) and
/// of the medium size (made into the cache when first asked for).
pub const THUMB_EDGE: u32 = 480;
pub const MEDIUM_EDGE: u32 = 2048;

/// Whether a file is a video, by its extension (add::VIDEOS).
pub fn is_video(path: &Path) -> bool {
    crate::add::VIDEOS.contains(&crate::add::extension(path).as_str())
}

/// A JPEG of a photo or of a video's frame, at most `edge` on the long
/// side, the right way up. `video` says which it is, when the file's name
/// does not.
pub fn render(path: &Path, edge: u32, video: bool) -> Result<Vec<u8>> {
    // Only a file: a FIFO or a device would hold whoever reads it, and a
    // place among background work with it.
    if !std::fs::metadata(path)?.is_file() {
        return Err(anyhow!("{} is not a file", path.display()));
    }
    // A video's from its most telling frame (cover.rs).
    if video {
        // Never more at once than background work may take (background.rs).
        let _slot = crate::background::slot();
        return crate::cover::pick(path, edge);
    }
    // On stdin, as jpeg.rs reads it: a name is never taken for
    // ImageMagick's own syntax ("50%off.jpg" is "500ff.jpg" to it).
    let file = File::open(path)?;
    let fit = format!("{edge}x{edge}>");
    let mut magick = crate::background::command("magick");
    // Decoding a big JPEG at a fraction of its size is much quicker. Only
    // a big one: the hint lets the decoder enlarge a small one too.
    let big = image::image_dimensions(path).is_ok_and(|(w, h)| w.max(h) > edge * 2);
    if big {
        magick.args(["-define", &format!("jpeg:size={}x{}", edge * 2, edge * 2)]);
    }
    // The file opened and its size read first: one that takes its time
    // holds no place among background work meanwhile.
    let _slot = crate::background::slot();
    let output = magick
        .args(["-[0]", "-auto-orient", "-thumbnail", &fit, "-quality", "82", "jpg:-"])
        .stdin(file)
        .output()
        .context("running magick")?;
    if output.stdout.is_empty() {
        return Err(anyhow!("{}", String::from_utf8_lossy(&output.stderr).trim()));
    }
    Ok(output.stdout)
}

/// A JPEG of a video's most telling frame, read from an HTTP URL by
/// ffmpeg (process.rs serves a provider's on the loopback): by that
/// protocol only, as a playlist it is given could name a file.
pub fn render_video_url(url: &str, edge: u32) -> Result<Vec<u8>> {
    let _slot = crate::background::slot();
    crate::cover::pick_from(std::ffi::OsStr::new(url), &["-protocol_whitelist", "http,tcp"], edge)
}
