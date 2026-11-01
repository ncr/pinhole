//! A Live Photo's effects made from its motion with ffmpeg, for the photos
//! whose source has no render of them: on iCloud the effects not set on the
//! iPhone, and every effect of a folder's Live Photos. A loop needs nothing
//! made: it is the motion played round and round.
//!
//! These are made while someone waits for them, so they run at normal
//! priority, not as background work (background.rs).

use std::path::Path;
use std::process::{Command, Stdio};

use anyhow::{anyhow, Context, Result};

/// The motion forward, then backward, as an MP4 that plays anywhere.
pub fn bounce(motion: &Path, out: &Path) -> Result<()> {
    let mut command = ffmpeg();
    command
        .arg("-i")
        .arg(motion)
        .args(["-filter_complex", "[0:v]split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0,format=yuv420p[v]"])
        .args(["-map", "[v]", "-c:v", "libx264", "-preset", "veryfast", "-crf", "23", "-movflags", "+faststart", "-f", "mp4"])
        .arg(out);
    run(command)
}

/// Every frame of the motion averaged into one JPEG, as the iPhone's long
/// exposure: what moves blurs, what stands still stays sharp. The frames
/// are first held to the first one's framing (vidstab's tripod mode), as a
/// hand never holds still for three seconds.
pub fn long_exposure(motion: &Path, out: &Path) -> Result<()> {
    let frames = frame_count(motion)?;
    let (motion, out) = (std::path::absolute(motion)?, std::path::absolute(out)?);
    // The transforms beside the file made, named in the filter graph by
    // their name alone, ffmpeg running in their folder: a folder's path
    // (the one everything is kept in is the person's to choose) is never
    // read as the graph's syntax (`,`, `;`, `[`).
    let transforms = out.with_extension("trf");
    let folder = transforms.parent().unwrap_or(Path::new("/"));
    let name = filter_path(Path::new(transforms.file_name().unwrap_or_default()));
    let mut detect = ffmpeg();
    detect
        .current_dir(folder)
        .arg("-i")
        .arg(&motion)
        .arg("-vf")
        .arg(format!("vidstabdetect=result={name}:shakiness=8:accuracy=15:tripod=1"))
        .args(["-f", "null", "-"]);
    let mut average = ffmpeg();
    average
        .current_dir(folder)
        .arg("-i")
        .arg(&motion)
        .arg("-vf")
        .arg(format!(
            "vidstabtransform=input={name}:tripod=1:optzoom=0:crop=keep:interpol=bicubic,tmix=frames={frames},select=eq(n\\,{})",
            frames - 1,
        ))
        // One file, by its name as it is: without `-update` a `%d` in it
        // would be the number of the frame.
        .args(["-frames:v", "1", "-q:v", "2", "-update", "1", "-f", "image2", "-c:v", "mjpeg"])
        .arg(&out);
    let result = run(detect).and_then(|()| run(average));
    let _ = std::fs::remove_file(&transforms);
    result
}

/// How many frames the motion has: tmix averages that many (it takes at
/// most 1024; a Live Photo has about 90).
fn frame_count(motion: &Path) -> Result<u32> {
    let output = Command::new("ffprobe")
        .args(["-v", "error", "-count_packets", "-select_streams", "v:0", "-show_entries", "stream=nb_read_packets", "-of", "csv=p=0"])
        .arg(motion)
        .stdin(Stdio::null())
        .output()
        .context("running ffprobe")?;
    let text = String::from_utf8_lossy(&output.stdout);
    let digits: String = text.chars().take_while(char::is_ascii_digit).collect();
    match digits.parse::<u32>() {
        Ok(n) if n > 0 => Ok(n.min(1024)),
        _ => Err(anyhow!("{} has no frames ffprobe can count", motion.display())),
    }
}

/// A file name inside a filter's options, where `:`, `'` and `\` mean
/// something.
fn filter_path(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "\\\\").replace(':', "\\:").replace('\'', "\\'")
}

fn ffmpeg() -> Command {
    let mut command = Command::new("ffmpeg");
    command.args(["-v", "error", "-nostdin", "-y"]).stdin(Stdio::null());
    command
}

fn run(mut command: Command) -> Result<()> {
    let output = command.output().context("running ffmpeg")?;
    if !output.status.success() {
        let message = String::from_utf8_lossy(&output.stderr);
        return Err(anyhow!("ffmpeg failed: {}", message.lines().last().unwrap_or("")));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A few seconds of a square moving across a still grey frame.
    fn motion(dir: &Path) -> std::path::PathBuf {
        let path = dir.join("IMG 50%off [1].MOV");
        let status = Command::new("ffmpeg")
            .args(["-v", "error", "-y", "-f", "lavfi", "-i", "color=c=gray:s=160x120:d=1:r=30", "-vf"])
            .arg("drawbox=x=t*120:y=40:w=30:h=30:c=white:t=fill")
            .args(["-c:v", "libx264", "-pix_fmt", "yuv420p"])
            .arg(&path)
            .status()
            .unwrap();
        assert!(status.success());
        path
    }

    #[test]
    fn effects_are_made_from_the_motion() {
        if Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_err() {
            return;
        }
        let top = std::env::temp_dir().join(format!("pinhole-effects-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&top);
        // Names ffmpeg would read as its own syntax, were they given to it
        // as they are: a filter graph's, an image sequence's.
        let dir = top.join("Photos, 50%d [old]; 'x'");
        std::fs::create_dir_all(&dir).unwrap();
        let motion = motion(&dir);
        assert_eq!(frame_count(&motion).unwrap(), 30);

        let bounced = dir.join("bounce.mp4");
        bounce(&motion, &bounced).unwrap();
        assert_eq!(frame_count(&bounced).unwrap(), 60);

        let long = dir.join("long 50%d.jpg");
        long_exposure(&motion, &long).unwrap();
        assert!(std::fs::read(&long).unwrap().starts_with(&[0xFF, 0xD8]));
        assert_eq!(std::fs::read_dir(&dir).unwrap().count(), 3, "only the motion, the bounce and the long exposure");
        let _ = std::fs::remove_dir_all(&top);
    }
}
