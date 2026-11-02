//! Thumbnails and mediums made from a file on this computer: a folder's
//! (pinhole.folder), and a provider's whose renditions say `"make": true`
//! (process.rs, docs/source-providers.md). A photo is made smaller with
//! ImageMagick, a video's from its most telling frame (cover.rs). A HEIF or
//! AVIF still whose picture is PQ or HLG (a camera's HDR still) is tone
//! mapped as a video's frame is (cover.rs, docs/hdr.md): ImageMagick shows
//! its codes as they are, flat and grey.

use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};
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
    // PQ or HLG, by ffmpeg and tone mapped; anything it cannot, as any.
    if hdr_still(file) {
        let _slot = crate::background::slot();
        if let Some(jpeg) = crate::cover::still(Input::File(file), edge)? {
            return Ok(jpeg);
        }
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

/// Whether a HEIF or AVIF file's picture (its primary item) says PQ or HLG
/// in its colour (nclx): from its first boxes, read through the file
/// opened. Anything else, or anything it cannot read, is not.
pub fn hdr_still(file: &File) -> bool {
    let mut head = Vec::new();
    let Ok(mut reader) = file.try_clone() else { return false };
    if reader.seek(SeekFrom::Start(0)).is_err() || reader.by_ref().take(1 << 20).read_to_end(&mut head).is_err() {
        return false;
    }
    matches!(primary_transfer(&head), Some(16 | 18))
}

/// ISOBMFF boxes in `data`: each one's type and body.
fn boxes(data: &[u8]) -> impl Iterator<Item = ([u8; 4], &[u8])> {
    let mut at = 0usize;
    std::iter::from_fn(move || {
        let header = data.get(at..at + 8)?;
        let mut size = u32::from_be_bytes(header[..4].try_into().ok()?) as usize;
        let kind: [u8; 4] = header[4..8].try_into().ok()?;
        let mut body = at + 8;
        if size == 1 {
            size = u64::from_be_bytes(data.get(at + 8..at + 16)?.try_into().ok()?) as usize;
            body += 8;
        } else if size == 0 {
            size = data.len() - at;
        }
        let end = at.checked_add(size)?;
        if size < body - at || end > data.len() {
            return None;
        }
        let found = (kind, &data[body..end]);
        at = end;
        Some(found)
    })
}

/// The transfer characteristics of the primary item's nclx colour, if a
/// HEIF or AVIF says one (ISO/IEC 23008-12: pitm, then its properties by
/// ipma in ipco).
fn primary_transfer(data: &[u8]) -> Option<u16> {
    let (_, ftyp) = boxes(data).find(|(kind, _)| kind == b"ftyp")?;
    if !ftyp.chunks(4).any(|b| matches!(b, b"heic" | b"heix" | b"heim" | b"heis" | b"mif1" | b"msf1" | b"avif" | b"avis")) {
        return None;
    }
    let (_, meta) = boxes(data).find(|(kind, _)| kind == b"meta")?;
    let meta = meta.get(4..)?; // version and flags
    let (_, pitm) = boxes(meta).find(|(kind, _)| kind == b"pitm")?;
    let primary = if pitm.first()? == &0 { u16::from_be_bytes(pitm.get(4..6)?.try_into().ok()?) as u32 } else {
        u32::from_be_bytes(pitm.get(4..8)?.try_into().ok()?)
    };
    let (_, iprp) = boxes(meta).find(|(kind, _)| kind == b"iprp")?;
    let properties: Vec<([u8; 4], &[u8])> = boxes(boxes(iprp).find(|(kind, _)| kind == b"ipco")?.1).collect();
    for (kind, ipma) in boxes(iprp) {
        if &kind != b"ipma" {
            continue;
        }
        let (version, flags) = (*ipma.first()?, *ipma.get(3)?);
        let count = u32::from_be_bytes(ipma.get(4..8)?.try_into().ok()?);
        let mut at = 8usize;
        for _ in 0..count {
            let item = if version < 1 {
                at += 2;
                u16::from_be_bytes(ipma.get(at - 2..at)?.try_into().ok()?) as u32
            } else {
                at += 4;
                u32::from_be_bytes(ipma.get(at - 4..at)?.try_into().ok()?)
            };
            let associations = *ipma.get(at)? as usize;
            at += 1;
            for _ in 0..associations {
                let index = if flags & 1 != 0 {
                    at += 2;
                    (u16::from_be_bytes(ipma.get(at - 2..at)?.try_into().ok()?) & 0x7fff) as usize
                } else {
                    at += 1;
                    (ipma.get(at - 1)? & 0x7f) as usize
                };
                if item != primary || index == 0 {
                    continue;
                }
                if let Some((b"colr", colr)) = properties.get(index - 1).map(|(k, b)| (k, *b)) {
                    if colr.get(..4) == Some(b"nclx") {
                        return Some(u16::from_be_bytes(colr.get(6..8)?.try_into().ok()?));
                    }
                }
            }
        }
    }
    None
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

    /// A camera's HDR still (HLG, in an AVIF here; an HEIC's boxes are the
    /// same) is tone mapped as a video's frame is: a grey of HLG's 0.75,
    /// SDR white as the window shows it, not its code read as sRGB's. SDR
    /// ones, and the same picture said to be sRGB's, as before.
    #[test]
    fn an_hdr_still_is_tone_mapped_as_a_video_frame_is() {
        if !has("magick") || !has("ffmpeg") || Command::new("heif-enc").arg("--version").output().is_err() {
            return;
        }
        let dir = std::env::temp_dir().join(format!("pinhole-hdr-still-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let png = dir.join("grey.png");
        let made = Command::new("magick").args(["-size", "640x480", "xc:rgb(75%,75%,75%)", "-depth", "16"]).arg(&png).status().unwrap();
        assert!(made.success());
        let encode = |name: &str, transfer: &str| {
            let to = dir.join(name);
            let made = Command::new("heif-enc")
                .args(["-A", "-b", "10", "--colour_primaries", "9", "--matrix_coefficients", "9", "--full_range_flag", "1"])
                .args(["--transfer_characteristic", transfer, "-o"])
                .arg(&to)
                .arg(&png)
                .output()
                .unwrap();
            assert!(made.status.success(), "{}", String::from_utf8_lossy(&made.stderr));
            to
        };
        let middle = |path: &Path| {
            let file = File::open(path).unwrap();
            let jpeg = render(&file, size(&file), 480, false).unwrap();
            let image = image::load_from_memory(&jpeg).unwrap().to_rgb8();
            assert_eq!((image.width(), image.height()), (480, 360));
            (hdr_still(&file), image.get_pixel(240, 180).0)
        };
        let (hdr, hlg) = middle(&encode("hlg.avif", "18"));
        // HLG's 0.75: a quarter of its scene light, at 1000 nits by the
        // OOTF, 203 nits SDR white.
        let want = crate::cover::tone_map_hlg_grey(0.75);
        assert!(hdr);
        for k in 0..3 {
            assert!((hlg[k] as i32 - want as i32).abs() <= 3, "{hlg:?} against {want}");
        }
        let (hdr, sdr) = middle(&encode("srgb.avif", "13"));
        assert!(!hdr);
        // Its codes as they are: 75 %, 191 or so.
        assert!((sdr[1] as i32 - 191).abs() <= 3, "{sdr:?}");
        assert!((hlg[1] as i32 - sdr[1] as i32).abs() > 10, "tone mapped {hlg:?}, as it is {sdr:?}");
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
