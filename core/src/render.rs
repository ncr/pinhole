//! Thumbnails and mediums made from a file on this computer: a folder's
//! (pinhole.folder), and a provider's whose renditions say `"make": true`
//! (process.rs, docs/source-providers.md). A photo is made smaller with
//! ImageMagick, a video's from its most telling frame (cover.rs).

use std::fs::File;
use std::io::{BufReader, Seek, SeekFrom};
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

use anyhow::{anyhow, Result};

use crate::cover::Input;

/// Long edges of the thumbnail (kept in the library, as iCloud's are) and
/// of the medium size (made into the cache when first asked for).
pub const THUMB_EDGE: u32 = 480;
pub const MEDIUM_EDGE: u32 = 2048;

/// Whether a file is a video, by its extension (add::VIDEOS).
pub fn is_video(path: &Path) -> bool {
    crate::add::VIDEOS.contains(&crate::add::extension(path).as_str())
}

/// `O_NONBLOCK` (Linux, as on x86 and ARM): a FIFO put where a file was is
/// opened without waiting for a writer.
pub(crate) const O_NONBLOCK: i32 = 0o4000;

/// The same as `render`, of the file at `path`: only a file (a FIFO or a
/// device would hold whoever reads it), opened without waiting and found
/// to be one once open, then read only through what was opened.
pub fn render_path(path: &Path, edge: u32, video: bool) -> Result<Vec<u8>> {
    let not_a_file = || anyhow!("{} is not a file", path.display());
    if !std::fs::metadata(path)?.is_file() {
        return Err(not_a_file());
    }
    let file = std::fs::OpenOptions::new().read(true).custom_flags(O_NONBLOCK).open(path)?;
    if !file.metadata()?.is_file() {
        return Err(not_a_file());
    }
    let size = if video { None } else { size(&file) };
    render(&file, size, edge, video)
}

/// A photo's size in pixels, from its start, if it is a kind `image` reads
/// (a JPEG). It reads the file: on a mount that may not answer, done where
/// that holds no one up (process.rs).
pub fn size(mut file: &File) -> Option<(u32, u32)> {
    file.seek(SeekFrom::Start(0)).ok()?;
    image::ImageReader::new(BufReader::new(file)).with_guessed_format().ok()?.into_dimensions().ok()
}

/// A JPEG of a photo or of a video's frame, at most `edge` on the long
/// side, the right way up, from a file already opened and found to be a
/// file: whatever is at its path by now (a FIFO put there) is never what
/// is read. `size` is the photo's (`size`), if known. Each program that
/// makes it is stopped if it hangs or spins (`background::LIMITS`).
pub fn render(file: &File, size: Option<(u32, u32)>, edge: u32, video: bool) -> Result<Vec<u8>> {
    // A video's from its most telling frame (cover.rs).
    if video {
        // Never more at once than background work may take (background.rs).
        let _slot = crate::background::slot();
        return crate::cover::pick(Input::File(file), edge);
    }
    // On stdin, as jpeg.rs reads it: a name is never taken for
    // ImageMagick's own syntax ("50%off.jpg" is "500ff.jpg" to it). From
    // its start, wherever reading its size left it.
    let mut stdin = file.try_clone()?;
    stdin.seek(SeekFrom::Start(0))?;
    let fit = format!("{edge}x{edge}>");
    let mut magick = crate::background::command("magick");
    // Decoding a big JPEG at a fraction of its size is much quicker. Only
    // a big one: the hint lets the decoder enlarge a small one too.
    if size.is_some_and(|(w, h)| w.max(h) > edge * 2) {
        magick.args(["-define", &format!("jpeg:size={}x{}", edge * 2, edge * 2)]);
    }
    magick.args(["-[0]", "-auto-orient", "-thumbnail", &fit, "-quality", "82", "jpg:-"]).stdin(stdin);
    let _slot = crate::background::slot();
    let output = crate::background::output(&mut magick).map_err(|e| failed(e, "magick", true))?;
    if output.stdout.is_empty() {
        return Err(Rejected(said("magick made no image of it", &output.stderr)).into());
    }
    Ok(output.stdout)
}

/// A file a program turned down: it ran on it and made no image (a file
/// damaged or cut short, or of a kind its decoder does not know), or went
/// round in circles on it (`background::spun`). That is the file's alone:
/// the source it is in is fine, and making it again is not worth trying
/// until it changes (`FetchError::Unreadable`, source.rs).
#[derive(Debug)]
pub struct Rejected(pub String);

impl std::fmt::Display for Rejected {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Rejected {}

/// Whether a program turned the file down (`Rejected`), whatever was said
/// of it since.
pub fn rejected(e: &anyhow::Error) -> bool {
    e.downcast_ref::<Rejected>().is_some()
}

/// A program of background work that failed (`background::output`): when
/// it spun on a file (`of_file`; not a URL, whose server may be what went
/// wrong), the file's (`Rejected`); else as it is: it could not start, or
/// hung on what does not answer (a mount that has gone), which is the
/// source's.
pub(crate) fn failed(e: std::io::Error, program: &str, of_file: bool) -> anyhow::Error {
    let spun = of_file && crate::background::spun(&e);
    let e = anyhow::Error::from(e);
    if spun {
        e.context(Rejected(format!("running {program}")))
    } else {
        e.context(format!("running {program}"))
    }
}

/// `what`, and what a program wrote to stderr, on one line, if anything.
pub(crate) fn said(what: &str, stderr: &[u8]) -> String {
    let stderr = String::from_utf8_lossy(stderr);
    let line = stderr.lines().map(str::trim).filter(|l| !l.is_empty()).collect::<Vec<_>>().join(" ");
    match line.char_indices().nth(400) {
        _ if line.is_empty() => what.to_owned(),
        Some((cut, _)) => format!("{what}: {}…", &line[..cut]),
        None => format!("{what}: {line}"),
    }
}

/// A JPEG of a video's most telling frame, read from an HTTP URL by
/// ffmpeg (process.rs serves a provider's on the loopback). It mostly
/// waits on the network, so it holds a place among those that do, never
/// one a folder's thumbnails wait for (background.rs).
pub fn render_video_url(url: &str, edge: u32) -> Result<Vec<u8>> {
    let _slot = crate::background::network_slot();
    crate::cover::pick(Input::Url(url), edge)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};

    fn has(program: &str) -> bool {
        Command::new(program).arg("-version").stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success())
    }

    /// The file opened is what is made, whatever is put at its path after:
    /// a FIFO there is never opened (it would hold whoever reads it).
    #[test]
    fn what_is_made_is_the_file_opened_not_what_is_at_its_path_by_then() {
        let dir = std::env::temp_dir().join(format!("pinhole-render-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let fifo = dir.join("fifo");
        assert!(Command::new("mkfifo").arg(&fifo).status().unwrap().success());

        let photo = dir.join("photo.jpg");
        image::RgbImage::from_pixel(1200, 800, image::Rgb([200, 40, 40])).save(&photo).unwrap();
        let file = File::open(&photo).unwrap();
        std::fs::rename(&fifo, &photo).unwrap();
        let size = size(&file);
        assert_eq!(size, Some((1200, 800)));
        let started = Instant::now();
        let jpeg = render(&file, size, 480, false).unwrap();
        let made = image::load_from_memory(&jpeg).unwrap();
        assert_eq!((made.width(), made.height()), (480, 320));
        assert!(started.elapsed() < Duration::from_secs(30));

        if has("ffmpeg") {
            // An MP4 and a MOV as an iPhone writes it (HEVC, if this ffmpeg
            // can, turned for a phone held upright), each with its index
            // (moov) at its end, which ffmpeg seeks to: read through the
            // file it was given on stdin.
            let ffmpeg = |args: &[&str], to: &Path| {
                let made = Command::new("ffmpeg").args(["-v", "error", "-y"]).args(args).arg(to).status().unwrap();
                assert!(made.success(), "{args:?}");
            };
            let testsrc = ["-f", "lavfi", "-i", "testsrc2=duration=3:size=320x180:rate=10", "-pix_fmt", "yuv420p"];
            ffmpeg(&testsrc, &dir.join("clip.mp4"));
            let hevc = Command::new("ffmpeg").args(["-v", "error", "-h", "encoder=libx265"]).output().is_ok_and(|o| !o.stdout.is_empty());
            let codec: &[&str] = if hevc { &["-c:v", "libx265", "-tag:v", "hvc1"] } else { &[] };
            ffmpeg(&[&testsrc[..], codec].concat(), &dir.join("level.mov"));
            let level = dir.join("level.mov").display().to_string();
            ffmpeg(&["-display_rotation", "90", "-i", &level, "-c", "copy"], &dir.join("clip.mov"));
            for (name, size) in [("clip.mp4", (160, 90)), ("clip.mov", (90, 160))] {
                let video = dir.join(name);
                let bytes = std::fs::read(&video).unwrap();
                let at = |atom: &[u8]| bytes.windows(4).position(|w| w == atom).unwrap();
                assert!(at(b"moov") > at(b"mdat"), "{name}: its index first");
                let file = File::open(&video).unwrap();
                assert!(Command::new("mkfifo").arg(&fifo).status().unwrap().success());
                std::fs::rename(&fifo, &video).unwrap();
                let jpeg = render(&file, None, 160, true).unwrap();
                let made = image::load_from_memory(&jpeg).unwrap();
                assert_eq!((made.width(), made.height()), size, "{name}");
            }
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// A file a program makes nothing of is turned down, and says so
    /// whatever is said of it after; a program that spins on one too. One
    /// that hangs, or cannot start, is not the file's.
    #[test]
    fn a_damaged_file_is_turned_down_and_a_hung_program_is_not_its_fault() {
        let dir = std::env::temp_dir().join(format!("pinhole-rejected-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let mut damaged = vec![("empty.jpg", Vec::new()), ("noise.heic", b"ftypheic but nothing after".repeat(40))];
        if has("ffmpeg") {
            let clip = dir.join("whole.mp4");
            let made = Command::new("ffmpeg")
                .args(["-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=duration=2:size=320x180:rate=10", "-pix_fmt", "yuv420p"])
                .arg(&clip)
                .status()
                .unwrap();
            assert!(made.success());
            // Cut short before its index (moov, at its end): no frame in it.
            let bytes = std::fs::read(&clip).unwrap();
            damaged.push(("cut.mp4", bytes[..bytes.len() / 2].to_vec()));
        }
        for (name, bytes) in damaged {
            let path = dir.join(name);
            std::fs::write(&path, bytes).unwrap();
            let e = render_path(&path, 480, is_video(&path)).unwrap_err().context("could not make an image of it");
            assert!(rejected(&e), "{name}: {e:#}");
        }

        let quick = |cpu| crate::background::Limits { quiet: Duration::from_secs(1), cpu, every: Duration::from_millis(100) };
        let mut sleeping = crate::background::command("sleep");
        sleeping.arg("60");
        let hung = crate::background::output_within(&mut sleeping, &quick(Duration::from_secs(60))).unwrap_err();
        assert!(!rejected(&failed(hung, "sleep", true)));
        let mut spinning = crate::background::command("sh");
        spinning.args(["-c", "while :; do :; done"]);
        let spun = crate::background::output_within(&mut spinning, &quick(Duration::from_secs(1))).unwrap_err();
        assert!(rejected(&failed(spun, "sh", true).context("more said of it")));
        let missing = crate::background::command("/nonexistent/magick").output().unwrap_err();
        assert!(!rejected(&failed(missing, "magick", true)));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
