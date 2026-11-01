//! Adding to a folder: an asset's files copied in, never over a file there.

use std::fs::File;
use std::io::{Read, Write};
use std::path::Path;

use anyhow::{Context, Result};

use pinhole_core::add::{uuid, Addition};

/// Copy an asset's files into `dir`, never over a file there: under their
/// own names when free, else as `IMG_1234 (2).HEIC`, a Live Photo's two
/// files under the same new name so they stay paired, and clear of any
/// file there that would pair with them. Each is written under a hidden
/// name first, so a look at the folder never sees half a file, and keeps
/// its modification time. The names, inside `dir`, in the order of
/// `Addition::files`.
pub fn copy_into(dir: &Path, addition: &Addition, sent: &(dyn Fn(u64, u64) + Sync)) -> Result<Vec<String>> {
    let total = addition.size();
    let mut done = 0u64;
    let photo_stem = file_stem(&addition.file);
    // A motion's own ending after the photo's name: "_HEVC", or nothing.
    let suffix = addition.motion.as_deref().map(|m| {
        let s = file_stem(m);
        s.get(photo_stem.len()..).filter(|_| s.to_lowercase().starts_with(&photo_stem.to_lowercase())).unwrap_or("").to_owned()
    });
    let mut temps = Vec::new();
    for file in addition.files() {
        let temp = dir.join(format!(".pinhole-adding-{}", uuid()));
        if let Err(e) = copy_file(file, &temp, &mut |n| {
            done += n;
            sent(done, total)
        }) {
            let _ = std::fs::remove_file(&temp);
            for t in &temps {
                let _ = std::fs::remove_file(t);
            }
            return Err(e);
        }
        temps.push(temp);
    }

    let result = (|| -> Result<Vec<String>> {
        for n in 1.. {
            let stem = if n == 1 { photo_stem.clone() } else { format!("{photo_stem} ({n})") };
            if taken(dir, &stem)? {
                continue;
            }
            let mut names = vec![format!("{stem}.{}", ext_of(&addition.file))];
            if let (Some(motion), Some(suffix)) = (&addition.motion, &suffix) {
                names.push(format!("{stem}{suffix}.{}", ext_of(motion)));
            }
            let mut placed = Vec::new();
            let mut clash = false;
            for (temp, name) in temps.iter().zip(&names) {
                match place(temp, &dir.join(name)) {
                    Ok(()) => placed.push(dir.join(name)),
                    Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                        clash = true;
                        break;
                    }
                    Err(e) => return Err(e).with_context(|| format!("writing {}", dir.join(name).display())),
                }
            }
            if clash {
                // Someone else took the name meanwhile: back out, and on.
                for (p, t) in placed.iter().zip(&temps) {
                    let _ = std::fs::rename(p, t);
                }
                continue;
            }
            return Ok(names);
        }
        unreachable!()
    })();
    for t in &temps {
        let _ = std::fs::remove_file(t);
    }
    result
}

fn file_stem(path: &Path) -> String {
    path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "photo".into())
}

fn ext_of(path: &Path) -> String {
    path.extension().map(|e| e.to_string_lossy().into_owned()).unwrap_or_default()
}

/// Whether a file in `dir` has this name without its extension (any case,
/// with or without a motion's `_HEVC`): a new file under it would clash or
/// pair with one there.
fn taken(dir: &Path, stem: &str) -> Result<bool> {
    let want = stem.to_lowercase();
    for entry in std::fs::read_dir(dir).with_context(|| format!("reading {}", dir.display()))?.flatten() {
        let name = entry.file_name().to_string_lossy().to_lowercase();
        let s = name.rsplit_once('.').map_or(name.as_str(), |(s, _)| s);
        if s == want || s.strip_suffix("_hevc") == Some(&want) {
            return Ok(true);
        }
    }
    Ok(false)
}

/// Move `temp` to `to` unless `to` exists: a hard link (which fails on an
/// existing name), or where a disk has none (exFAT), a claim on the name
/// by creating it, then the rename over the claim.
fn place(temp: &Path, to: &Path) -> std::io::Result<()> {
    match std::fs::hard_link(temp, to) {
        Ok(()) => std::fs::remove_file(temp),
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => Err(e),
        Err(_) => {
            File::options().write(true).create_new(true).open(to)?;
            std::fs::rename(temp, to)
        }
    }
}

fn copy_file(from: &Path, to: &Path, sent: &mut dyn FnMut(u64)) -> Result<()> {
    let mut input = File::open(from).with_context(|| format!("reading {}", from.display()))?;
    let mut output = File::options().write(true).create_new(true).open(to).with_context(|| format!("writing {}", to.display()))?;
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = input.read(&mut buf).with_context(|| format!("reading {}", from.display()))?;
        if n == 0 {
            break;
        }
        output.write_all(&buf[..n]).with_context(|| format!("writing {}", to.display()))?;
        sent(n as u64);
    }
    output.sync_all()?;
    if let Ok(modified) = input.metadata().and_then(|m| m.modified()) {
        let _ = output.set_modified(modified);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use pinhole_core::add::gather;
    use std::path::PathBuf;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("pinhole-copy-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write(path: &Path, bytes: &[u8]) {
        std::fs::write(path, bytes).unwrap();
    }

    fn names(dir: &Path) -> Vec<String> {
        let mut n: Vec<String> =
            std::fs::read_dir(dir).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        n.sort();
        n
    }

    #[test]
    fn copies_never_overwrite_and_keep_a_live_photo_paired() {
        let (from, to) = (temp("from"), temp("to"));
        write(&from.join("IMG_1.HEIC"), b"new photo");
        write(&from.join("IMG_1_HEVC.MOV"), b"new motion");
        write(&from.join("clip.mp4"), b"video");
        // A photo of that name there already, and a motion that would pair
        // with a second one.
        write(&to.join("IMG_1.heic"), b"old");
        write(&to.join("img_1 (2).mov"), b"old motion");
        let (additions, _) = gather(&[from.join("IMG_1.HEIC"), from.join("IMG_1_HEVC.MOV"), from.join("clip.mp4")]);
        let seen = std::sync::Mutex::new(Vec::new());
        let placed = copy_into(&to, &additions[0], &|done, total| seen.lock().unwrap().push((done, total))).unwrap();
        assert_eq!(placed, vec!["IMG_1 (3).HEIC".to_owned(), "IMG_1 (3)_HEVC.MOV".to_owned()]);
        assert_eq!(seen.lock().unwrap().last(), Some(&(19, 19)));
        assert_eq!(std::fs::read(to.join("IMG_1 (3).HEIC")).unwrap(), b"new photo");
        assert_eq!(std::fs::read(to.join("IMG_1.heic")).unwrap(), b"old");
        assert_eq!(copy_into(&to, &additions[1], &|_, _| {}).unwrap(), vec!["clip.mp4".to_owned()]);
        assert_eq!(copy_into(&to, &additions[1], &|_, _| {}).unwrap(), vec!["clip (2).mp4".to_owned()]);
        // No hidden half-files left behind.
        assert_eq!(names(&to), vec!["IMG_1 (3).HEIC", "IMG_1 (3)_HEVC.MOV", "IMG_1.heic", "clip (2).mp4", "clip.mp4", "img_1 (2).mov"]);
        let modified = |p: &Path| p.metadata().unwrap().modified().unwrap();
        assert_eq!(modified(&to.join("clip.mp4")), modified(&from.join("clip.mp4")));
        let _ = std::fs::remove_dir_all(&from);
        let _ = std::fs::remove_dir_all(&to);
    }
}
