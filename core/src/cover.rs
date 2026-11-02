//! A video's cover in the grid: the most telling of a few frames from
//! across it, not whatever its first frame happens to be. A screen
//! recording or a clip that fades in starts black, and iCloud makes the
//! thumbnail of a video not added from an iPhone of its first frame.
//!
//! How telling a frame is: the spread of its brightness (a black, white or
//! one-colour frame has none) and how much detail it has (a blurred one
//! has little). A frame with almost no spread is dull, whatever its colour;
//! a dark night scene or a blue one under water still has some.
//!
//! An HDR video's frame (HLG, as an iPhone records, or PQ) is tone mapped
//! to SDR as it is taken (docs/hdr.md, H2), as the window plays it
//! (app/src/shaders/hdrvideo.frag): its codes read as SDR's are flat and
//! grey, and a cover in another curve would jump in brightness as the
//! video starts.

use std::fs::File;
use std::path::Path;
use std::process::{Command, Stdio};

use anyhow::{anyhow, Result};
use image::RgbImage;

/// Below this spread of brightness (its standard deviation, of 255) a
/// frame shows next to nothing. Of 2,800 iPhone videos' thumbnails, the
/// 25 below it were black, white or nearly so; the dimmest night scenes
/// and the bluest ones under water were above.
const DULL: f64 = 6.0;

/// Frames looked at, spread evenly across the video.
const CANDIDATES: u32 = 7;

/// How covers are made: part of a folder's video's key, so that its cover
/// is made again once when this changes. 2: HDR tone mapped. 3: by
/// BT.2390, as the window plays it.
pub const VERSION: &str = "3";

/// SDR white, in nits (BT.2408).
const SDR_WHITE: f64 = 203.0;
/// HLG's display peak, in nits.
const HLG_PEAK: f64 = 1000.0;
const PQ_M1: f64 = 2610.0 / 16384.0;
const PQ_M2: f64 = 2523.0 / 4096.0 * 128.0;
const PQ_C1: f64 = 3424.0 / 4096.0;
const PQ_C2: f64 = 2413.0 / 4096.0 * 32.0;
const PQ_C3: f64 = 2392.0 / 4096.0 * 32.0;

/// A video's HDR curve.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Curve {
    Hlg,
    /// PQ, with its peak in nits.
    Pq(f64),
}

fn pq_to_nits(e: f64) -> f64 {
    let p = e.clamp(0.0, 1.0).powf(1.0 / PQ_M2);
    10000.0 * ((p - PQ_C1).max(0.0) / (PQ_C2 - PQ_C3 * p)).powf(1.0 / PQ_M1)
}

fn nits_to_pq(nits: f64) -> f64 {
    let y = (nits / 10000.0).clamp(0.0, 1.0).powf(PQ_M1);
    ((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y)).powf(PQ_M2)
}

/// HLG's inverse OETF: a code to scene light from 0 to 1.
fn hlg_to_scene(e: f64) -> f64 {
    let e = e.clamp(0.0, 1.0);
    if e <= 0.5 { e * e / 3.0 } else { (((e - 0.55991073) / 0.17883277).exp() + 0.28466892) / 12.0 }
}

/// BT.2390's EETF from a source peaking at `peak` nits to SDR white: nits
/// to linear SDR, 1 its white (colour.cpp's eetfToSdr).
fn eetf_to_sdr(nits: f64, peak: f64) -> f64 {
    if peak <= SDR_WHITE {
        return nits.min(SDR_WHITE) / SDR_WHITE;
    }
    let top = nits_to_pq(peak);
    let max_lum = nits_to_pq(SDR_WHITE) / top;
    let knee = 1.5 * max_lum - 0.5;
    let e1 = (nits_to_pq(nits) / top).min(1.0);
    let mut e2 = e1;
    if e1 >= knee {
        let t = (e1 - knee) / (1.0 - knee);
        let (t2, t3) = (t * t, t * t * t);
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * knee + (t3 - 2.0 * t2 + t) * (1.0 - knee) + (-2.0 * t3 + 3.0 * t2) * max_lum;
    }
    pq_to_nits(e2 * top).min(SDR_WHITE) / SDR_WHITE
}

/// An HDR pixel's R'G'B' (BT.2020, 0 to 1) in sRGB, 8 bits, as the window
/// plays it: its light (HLG's OOTF at 1000 nits; PQ's EOTF), BT.2390's EETF
/// on the largest channel, BT.2020 to sRGB, clipped.
fn tone_map(rgb: [f64; 3], curve: Curve) -> [u8; 3] {
    let mut c = rgb;
    let peak = match curve {
        Curve::Hlg => {
            for v in &mut c {
                *v = hlg_to_scene(*v);
            }
            let ys = (0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2]).clamp(0.0, 1.0);
            let gain = HLG_PEAK * ys.powf(0.2);
            for v in &mut c {
                *v *= gain;
            }
            HLG_PEAK
        }
        Curve::Pq(peak) => {
            for v in &mut c {
                *v = pq_to_nits(*v);
            }
            peak
        }
    };
    let most = c[0].max(c[1]).max(c[2]);
    let f = if most > 1e-6 { eetf_to_sdr(most.min(peak), peak) / most } else { 1.0 / SDR_WHITE };
    const TO_SRGB: [[f64; 3]; 3] = [[1.6605, -0.5876, -0.0728], [-0.1246, 1.1329, -0.0083], [-0.0182, -0.1006, 1.1187]];
    let mut out = [0u8; 3];
    for (k, row) in TO_SRGB.iter().enumerate() {
        let v = ((row[0] * c[0] + row[1] * c[1] + row[2] * c[2]) * f).clamp(0.0, 1.0);
        let v = if v <= 0.0031308 { 12.92 * v } else { 1.055 * v.powf(1.0 / 2.4) - 0.055 };
        out[k] = (v * 255.0).round() as u8;
    }
    out
}

/// A PAM of 16-bit RGB (ffmpeg's `pam` of `rgb48be`) as its R'G'B', 0 to 1.
fn read_pam(bytes: &[u8]) -> Option<(u32, u32, Vec<[f64; 3]>)> {
    let end = bytes.windows(7).position(|w| w == b"ENDHDR\n")? + 7;
    let header = std::str::from_utf8(&bytes[..end]).ok()?;
    let (mut width, mut height, mut depth, mut max) = (0u32, 0u32, 0u32, 0u32);
    for line in header.lines() {
        let mut words = line.split_whitespace();
        match (words.next(), words.next().and_then(|v| v.parse::<u32>().ok())) {
            (Some("WIDTH"), Some(v)) => width = v,
            (Some("HEIGHT"), Some(v)) => height = v,
            (Some("DEPTH"), Some(v)) => depth = v,
            (Some("MAXVAL"), Some(v)) => max = v,
            _ => {}
        }
    }
    if depth != 3 || max != 65535 || width == 0 || height == 0 {
        return None;
    }
    let data = &bytes[end..];
    let n = width as usize * height as usize;
    if data.len() < n * 6 {
        return None;
    }
    let pixels = data[..n * 6]
        .chunks_exact(6)
        .map(|p| {
            let v = |i: usize| u16::from_be_bytes([p[i], p[i + 1]]) as f64 / 65535.0;
            [v(0), v(2), v(4)]
        })
        .collect();
    Some((width, height, pixels))
}

/// Whether a cover shows next to nothing.
pub fn dull(image: &RgbImage) -> bool {
    let (spread, _) = measure(image);
    spread < DULL
}

/// How telling a frame is: its spread of brightness and its detail (the
/// mean step between neighbouring pixels, weighed double as it is the
/// smaller number).
fn interest(image: &RgbImage) -> f64 {
    let (spread, detail) = measure(image);
    spread + 2.0 * detail
}

fn measure(image: &RgbImage) -> (f64, f64) {
    let (w, h) = image.dimensions();
    if w == 0 || h == 0 {
        return (0.0, 0.0);
    }
    let luma: Vec<f64> =
        image.pixels().map(|p| 0.299 * p.0[0] as f64 + 0.587 * p.0[1] as f64 + 0.114 * p.0[2] as f64).collect();
    let n = luma.len() as f64;
    let mean = luma.iter().sum::<f64>() / n;
    let spread = (luma.iter().map(|y| (y - mean).powi(2)).sum::<f64>() / n).sqrt();
    let (w, h) = (w as usize, h as usize);
    let (mut steps, mut count) = (0.0, 0usize);
    for y in 0..h {
        for x in 0..w {
            let here = luma[y * w + x];
            if x + 1 < w {
                steps += (luma[y * w + x + 1] - here).abs();
                count += 1;
            }
            if y + 1 < h {
                steps += (luma[(y + 1) * w + x] - here).abs();
                count += 1;
            }
        }
    }
    (spread, if count == 0 { 0.0 } else { steps / count as f64 })
}

/// Where ffmpeg reads a video from.
#[derive(Clone, Copy)]
pub enum Input<'a> {
    /// A file it opens by its path: a folder's, or one pinholed made.
    Path(&'a Path),
    /// A file already opened, and found to be one (a provider's path
    /// ticket): its stdin, which it opens again as /proc/self/fd/0 (not
    /// /dev/stdin, a link to it a system may lack), the very file whatever
    /// is at its path by then, and seeks in.
    File(&'a File),
    /// An HTTP URL on the loopback (process.rs serves a provider's there):
    /// by that protocol only, as a playlist it is given could name a file,
    /// and never through a proxy (`http_proxy`), which would be given the
    /// URL and could not reach it.
    Url(&'a str),
}

impl Input<'_> {
    /// ffmpeg or ffprobe reading it, `before` its options for it.
    fn command(&self, program: &str, before: &[&str]) -> Result<Command> {
        let mut command = crate::background::command(program);
        command.args(["-v", "error"]);
        if program == "ffmpeg" {
            // Its stdin is the file, not keys to quit with.
            command.arg("-nostdin");
        }
        command.args(before);
        match self {
            Input::Path(path) => {
                command.args(["-protocol_whitelist", "file", "-i"]).arg(path).stdin(Stdio::null());
            }
            Input::File(file) => {
                command.args(["-protocol_whitelist", "file", "-i", "/proc/self/fd/0"]).stdin(file.try_clone()?);
            }
            Input::Url(url) => {
                // Its reads time out as pinholed's of the provider do.
                command.args(["-protocol_whitelist", "http,tcp", "-rw_timeout", "60000000", "-i", url]).stdin(Stdio::null());
                for proxy in ["http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY"] {
                    command.env_remove(proxy);
                }
            }
        }
        Ok(command)
    }

    /// A file's, not a URL's: what goes wrong reading one says something
    /// of the file (render::Rejected).
    fn of_file(&self) -> bool {
        !matches!(self, Input::Url(_))
    }

    /// `why` it made no image: the file's (render::Rejected), or a URL's.
    fn turned_down(&self, why: String) -> anyhow::Error {
        if self.of_file() {
            crate::render::Rejected(why).into()
        } else {
            anyhow!(why)
        }
    }
}

/// A JPEG of the video's most telling frame, at most `edge` on the long
/// side. Background work (background.rs): the caller holds a slot.
pub fn pick(input: Input, edge: u32) -> Result<Vec<u8>> {
    best_from(input, edge).map(|(_, jpeg)| jpeg)
}

/// A JPEG of a still (a HEIF or AVIF) whose picture is PQ or HLG, tone
/// mapped as a video's frame is, at most `edge` on the long side; None if
/// ffprobe finds it is not HDR after all. Background work: the caller holds
/// a slot.
pub fn still(input: Input, edge: u32) -> Result<Option<Vec<u8>>> {
    let Some(curve) = probe(input)?.curve else { return Ok(None) };
    frame_from(input, 0.0, edge, Some(curve)).map(Some)
}

/// HLG's grey of code `code` as tone_map makes it, 8 bits (for a test).
#[cfg(test)]
pub fn tone_map_hlg_grey(code: f64) -> u8 {
    tone_map([code; 3], Curve::Hlg)[1]
}

/// When the video's most telling frame is, in seconds, and a JPEG of it at
/// most `edge` on the long side.
pub fn best(video: &Path, edge: u32) -> Result<(f64, Vec<u8>)> {
    best_from(Input::Path(video), edge)
}

fn best_from(input: Input, edge: u32) -> Result<(f64, Vec<u8>)> {
    // A video whose length ffprobe cannot tell, or a very short one, from
    // its start.
    let probe = probe(input)?;
    let times: Vec<f64> = match probe.duration {
        Some(d) if d >= 1.0 => (1..=CANDIDATES).map(|i| d * i as f64 / (CANDIDATES + 1) as f64).collect(),
        _ => vec![0.0],
    };
    let hdr = probe.curve;
    let from_start = times == [0.0];
    let mut best: Option<(f64, f64, Vec<u8>)> = None;
    let mut failed = None;
    for at in times {
        let jpeg = match frame_from(input, at, edge, hdr) {
            Ok(jpeg) => jpeg,
            // One stopped, hung or spinning: so would the rest be.
            Err(e) if timed_out(&e) => return Err(e),
            Err(e) => {
                failed = Some(e);
                continue;
            }
        };
        let Ok(image) = image::load_from_memory_with_format(&jpeg, image::ImageFormat::Jpeg) else { continue };
        let score = interest(&image.to_rgb8());
        if best.as_ref().is_none_or(|(b, _, _)| score > *b) {
            best = Some((score, at, jpeg));
        }
    }
    match best {
        Some((_, at, jpeg)) => Ok((at, jpeg)),
        // Not the start again, when that is what failed.
        None => match failed {
            Some(e) if from_start => Err(e),
            failed => frame_from(input, 0.0, edge, hdr).map(|jpeg| (0.0, jpeg)).map_err(|e| failed.unwrap_or(e)),
        },
    }
}

/// A JPEG of the frame `at` seconds in, at most `edge` on the long side.
pub fn frame(video: &Path, at: f64, edge: u32) -> Result<Vec<u8>> {
    let input = Input::Path(video);
    frame_from(input, at, edge, probe(input)?.curve)
}

/// An HDR frame, scaled, comes as 16-bit R'G'B' (its own matrix, full
/// range) and is tone mapped here (tone_map), then made a JPEG.
fn frame_from(input: Input, at: f64, edge: u32, hdr: Option<Curve>) -> Result<Vec<u8>> {
    let fit = format!("scale={edge}:{edge}:force_original_aspect_ratio=decrease");
    let mut ffmpeg = input.command("ffmpeg", &["-threads", "1", "-ss", &format!("{at:.3}")])?;
    ffmpeg.args(["-frames:v", "1", "-vf"]);
    if hdr.is_some() {
        ffmpeg.args([&format!("{fit}:out_range=pc,format=rgb48be"), "-f", "image2pipe", "-vcodec", "pam", "-"]);
    } else {
        ffmpeg.args([&fit, "-q:v", "4", "-f", "image2pipe", "-vcodec", "mjpeg", "-"]);
    }
    let output = crate::background::output(&mut ffmpeg).map_err(|e| crate::render::failed(e, "ffmpeg", input.of_file()))?;
    if output.stdout.is_empty() {
        return Err(input.turned_down(crate::render::said(&format!("no frame at {at:.1} s"), &output.stderr)));
    }
    let Some(curve) = hdr else { return Ok(output.stdout) };
    let (width, height, pixels) =
        read_pam(&output.stdout).ok_or_else(|| input.turned_down(format!("no frame read at {at:.1} s")))?;
    let mut image = RgbImage::new(width, height);
    for (pixel, rgb) in image.pixels_mut().zip(pixels) {
        *pixel = image::Rgb(tone_map(rgb, curve));
    }
    let mut jpeg = Vec::new();
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut jpeg, 85).encode_image(&image)?;
    Ok(jpeg)
}

/// What ffprobe tells of a video.
#[derive(Default)]
struct Probe {
    /// Its length in seconds, if it can tell.
    duration: Option<f64>,
    /// Its picture's curve, if HLG or PQ.
    curve: Option<Curve>,
}

/// The video's length and whether it is HDR, from ffprobe; an error only
/// when it was stopped, hung or spinning.
fn probe(input: Input) -> Result<Probe> {
    let mut ffprobe = input.command("ffprobe", &["-select_streams", "v:0"])?;
    ffprobe.args([
        "-show_entries",
        "format=duration:stream=color_transfer:stream_side_data=max_luminance",
        "-of",
        "default=noprint_wrappers=1",
    ]);
    let output = match crate::background::output(&mut ffprobe) {
        Ok(output) => output,
        Err(e) if e.kind() == std::io::ErrorKind::TimedOut => return Err(crate::render::failed(e, "ffprobe", input.of_file())),
        Err(_) => return Ok(Probe::default()),
    };
    let mut probe = Probe::default();
    let (mut transfer, mut peak) = (String::new(), None::<f64>);
    for line in String::from_utf8_lossy(&output.stdout).lines() {
        match line.trim().split_once('=') {
            Some(("duration", d)) => probe.duration = d.parse().ok().filter(|d: &f64| d.is_finite()),
            Some(("color_transfer", t)) => transfer = t.to_string(),
            // Its mastering display's, "10000000/10000" in nits.
            Some(("max_luminance", l)) => {
                peak = match l.split_once('/') {
                    Some((a, b)) => a.parse::<f64>().ok().zip(b.parse::<f64>().ok()).map(|(a, b)| a / b),
                    None => l.parse().ok(),
                }
            }
            _ => {}
        }
    }
    probe.curve = match transfer.as_str() {
        "arib-std-b67" => Some(Curve::Hlg),
        // As the window has it: 1000 nits unless it says a peak above SDR
        // white and below PQ's top.
        "smpte2084" => Some(Curve::Pq(peak.filter(|p| *p > SDR_WHITE && *p < 10000.0).unwrap_or(1000.0))),
        _ => None,
    };
    Ok(probe)
}

/// Whether a video (a file) is HLG or PQ, by ffprobe; an error only when it
/// was stopped, hung or spinning.
pub fn is_hdr(video: &Path) -> Result<bool> {
    let _slot = crate::background::slot();
    Ok(probe(Input::Path(video))?.curve.is_some())
}

/// Whether a program was stopped, hung or spinning (background::output).
pub fn timed_out(e: &anyhow::Error) -> bool {
    e.chain().any(|e| e.downcast_ref::<std::io::Error>().is_some_and(|e| e.kind() == std::io::ErrorKind::TimedOut))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    #[test]
    fn a_frame_of_one_colour_is_dull_and_one_with_something_in_it_is_not() {
        let black = RgbImage::new(32, 18);
        let white = RgbImage::from_pixel(32, 18, image::Rgb([255, 255, 255]));
        let navy = RgbImage::from_pixel(32, 18, image::Rgb([11, 19, 34]));
        assert!(dull(&black) && dull(&white) && dull(&navy));
        // A dim scene with a light in it.
        let night = RgbImage::from_fn(32, 18, |x, y| if (12..20).contains(&x) && (6..12).contains(&y) {
            image::Rgb([120, 100, 60])
        } else {
            image::Rgb([8, 8, 12])
        });
        assert!(!dull(&night));
        assert!(interest(&night) > interest(&navy));
    }

    #[test]
    fn a_video_that_starts_black_gets_a_cover_from_what_follows() {
        if Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_err() {
            return;
        }
        let dir = std::env::temp_dir().join(format!("pinhole-cover-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let video = dir.join("fade.mp4");
        // Two seconds of black, then six of a test pattern.
        let status = Command::new("ffmpeg")
            .args(["-v", "error", "-y", "-f", "lavfi", "-i", "color=black:s=160x90:d=2:r=10"])
            .args(["-f", "lavfi", "-i", "testsrc=s=160x90:d=6:r=10"])
            .args(["-filter_complex", "[0:v][1:v]concat=n=2:v=1[v]", "-map", "[v]", "-pix_fmt", "yuv420p"])
            .arg(&video)
            .status()
            .unwrap();
        assert!(status.success());
        let jpeg = pick(Input::Path(&video), 80).unwrap();
        let image = image::load_from_memory(&jpeg).unwrap().to_rgb8();
        assert_eq!(image.dimensions(), (80, 45));
        assert!(!dull(&image));
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Mean saturation (HSV's, 0 to 1) of an image.
    fn saturation(image: &RgbImage) -> f64 {
        let s: f64 = image
            .pixels()
            .map(|p| {
                let (max, min) = (p.0.iter().max().copied().unwrap_or(0) as f64, p.0.iter().min().copied().unwrap_or(0) as f64);
                if max == 0.0 { 0.0 } else { (max - min) / max }
            })
            .sum();
        s / (image.width() * image.height()) as f64
    }

    /// The window's mapping (app/src/shaders/hdrvideo.frag): HLG's
    /// reference white (75 %, 203 nits) is BT.2390's 0.78 of SDR white,
    /// its peak SDR white, as colour.cpp's eetfToSdr has them.
    #[test]
    fn hdr_is_tone_mapped_as_the_window_plays_it() {
        assert_eq!(tone_map([0.75; 3], Curve::Hlg), [229; 3]);
        assert_eq!(tone_map([1.0; 3], Curve::Hlg), [255; 3]);
        assert_eq!(tone_map([0.0; 3], Curve::Hlg), [0; 3]);
        // PQ's 1000 nits at a 1000-nit peak: SDR white; 203 nits under it.
        assert_eq!(tone_map([nits_to_pq(1000.0); 3], Curve::Pq(1000.0)), [255; 3]);
        assert_eq!(tone_map([nits_to_pq(203.0); 3], Curve::Pq(1000.0)), [229; 3]);
        // A saturated BT.2020 red beyond sRGB, clipped, not wrapped.
        let red = tone_map([0.75, 0.0, 0.0], Curve::Hlg);
        assert!(red[0] > 200 && red[1] == 0 && red[2] == 0, "{red:?}");
    }

    #[test]
    fn an_hlg_video_gets_a_cover_more_colourful_than_its_codes() {
        if Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_err() {
            return;
        }
        let dir = std::env::temp_dir().join(format!("pinhole-cover-hlg-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        // testsrc2's bars as an SDR video, and the same made HLG as a
        // camera would record them: its SDR picture's light, put through
        // HLG's curve in BT.2020.
        let sdr = dir.join("sdr.mp4");
        let hlg = dir.join("hlg.mp4");
        let made = |args: &[&str], out: &Path| {
            let status = Command::new("ffmpeg")
                .args(["-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=320x180:d=2:r=10"])
                .args(args)
                .arg(out)
                .status()
                .unwrap();
            assert!(status.success());
        };
        made(&["-pix_fmt", "yuv420p", "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709"], &sdr);
        made(
            &[
                "-vf",
                "format=yuv420p,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv,zscale=t=linear:npl=203,format=gbrpf32le,zscale=p=bt2020:t=arib-std-b67:m=bt2020nc:r=tv,format=yuv420p10le",
                "-c:v",
                "libx264",
                "-x264-params",
                "colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc",
            ],
            &hlg,
        );
        let cover = |video: &Path| image::load_from_memory(&pick(Input::Path(video), 160).unwrap()).unwrap().to_rgb8();
        let (s_sdr, s_hlg) = (saturation(&cover(&sdr)), saturation(&cover(&hlg)));
        // Its codes read as SDR's, as before: far greyer.
        let flat = image::load_from_memory(&frame_from(Input::Path(&hlg), 1.0, 160, None).unwrap()).unwrap().to_rgb8();
        let s_flat = saturation(&flat);
        eprintln!("saturation: SDR {s_sdr:.3}, HLG tone mapped {s_hlg:.3}, HLG as SDR {s_flat:.3}");
        // zscale's HLG is not BT.2100's own (no OOTF as BT.2100 has it), so
        // not as colourful as the SDR it came from; far more than its codes.
        assert!(s_hlg > 1.15 * s_flat, "HLG {s_hlg:.3} against its codes {s_flat:.3}");
        assert!(s_flat < 0.85 * s_sdr, "untouched HLG {s_flat:.3} against SDR {s_sdr:.3}");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
