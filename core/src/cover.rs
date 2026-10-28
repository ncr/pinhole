//! A video's cover in the grid: the most telling of a few frames from
//! across it, not whatever its first frame happens to be. A screen
//! recording or a clip that fades in starts black, and iCloud makes the
//! thumbnail of a video not added from an iPhone of its first frame.
//!
//! How telling a frame is: the spread of its brightness (a black, white or
//! one-colour frame has none) and how much detail it has (a blurred one
//! has little). A frame with almost no spread is dull, whatever its colour;
//! a dark night scene or a blue one under water still has some.

use std::path::Path;
use std::process::Stdio;

use anyhow::{anyhow, Context, Result};
use image::RgbImage;

/// Below this spread of brightness (its standard deviation, of 255) a
/// frame shows next to nothing. Of 2,800 iPhone videos' thumbnails, the
/// 25 below it were black, white or nearly so; the dimmest night scenes
/// and the bluest ones under water were above.
const DULL: f64 = 6.0;

/// Frames looked at, spread evenly across the video.
const CANDIDATES: u32 = 7;

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

/// A JPEG of the video's most telling frame, at most `edge` on the long
/// side. Background work (background.rs): the caller holds a slot.
pub fn pick(video: &Path, edge: u32) -> Result<Vec<u8>> {
    best(video, edge).map(|(_, jpeg)| jpeg)
}

/// When the video's most telling frame is, in seconds, and a JPEG of it at
/// most `edge` on the long side.
pub fn best(video: &Path, edge: u32) -> Result<(f64, Vec<u8>)> {
    // A video whose length ffprobe cannot tell, or a very short one, from
    // its start.
    let times: Vec<f64> = match duration(video) {
        Some(d) if d >= 1.0 => (1..=CANDIDATES).map(|i| d * i as f64 / (CANDIDATES + 1) as f64).collect(),
        _ => vec![0.0],
    };
    let mut best: Option<(f64, f64, Vec<u8>)> = None;
    let mut failed = None;
    for at in times {
        let jpeg = match frame(video, at, edge) {
            Ok(jpeg) => jpeg,
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
        None => frame(video, 0.0, edge).map(|jpeg| (0.0, jpeg)).map_err(|e| failed.unwrap_or(e)),
    }
}

/// A JPEG of the frame `at` seconds in, at most `edge` on the long side.
pub fn frame(video: &Path, at: f64, edge: u32) -> Result<Vec<u8>> {
    let scale = format!("scale={edge}:{edge}:force_original_aspect_ratio=decrease");
    let output = crate::background::command("ffmpeg")
        .args(["-v", "error", "-threads", "1", "-ss", &format!("{at:.3}"), "-i"])
        .arg(video)
        .args(["-frames:v", "1", "-vf", &scale, "-q:v", "4", "-f", "image2pipe", "-vcodec", "mjpeg", "-"])
        .stdin(Stdio::null())
        .output()
        .context("running ffmpeg")?;
    if output.stdout.is_empty() {
        return Err(anyhow!("no frame at {at:.1} s: {}", String::from_utf8_lossy(&output.stderr).trim()));
    }
    Ok(output.stdout)
}

/// The video's length in seconds, from ffprobe.
fn duration(video: &Path) -> Option<f64> {
    let output = crate::background::command("ffprobe")
        .args(["-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1"])
        .arg(video)
        .stdin(Stdio::null())
        .output()
        .ok()?;
    String::from_utf8_lossy(&output.stdout).trim().parse().ok().filter(|d: &f64| d.is_finite())
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
        let jpeg = pick(&video, 80).unwrap();
        let image = image::load_from_memory(&jpeg).unwrap().to_rgb8();
        assert_eq!(image.dimensions(), (80, 45));
        assert!(!dull(&image));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
