//! Adding photos and videos to a library from files on this computer
//! (dropped on the window, or `pinhole-sync add`). The files are grouped
//! into the assets they make, a Live Photo's photo and motion together,
//! and handed to the source one asset at a time (`Source::add`): a folder
//! copies them in, iCloud uploads them. The files themselves stay as they
//! are. See docs/sources.md.

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};

/// Photos and videos, by extension, as a folder's look tells them apart.
pub const PHOTOS: &[&str] = &["jpg", "jpeg", "heic", "heif", "png", "gif", "tif", "tiff", "webp", "avif"];
pub const VIDEOS: &[&str] = &["mov", "mp4", "m4v", "mkv", "webm", "avi"];

/// One asset to add: a photo or a video, and a Live Photo's motion beside
/// its photo.
#[derive(Debug, Clone, PartialEq)]
pub struct Addition {
    pub file: PathBuf,
    pub motion: Option<PathBuf>,
    /// The same for everything added at once: iCloud's import group.
    pub group: String,
}

impl Addition {
    pub fn files(&self) -> impl Iterator<Item = &Path> {
        std::iter::once(self.file.as_path()).chain(self.motion.as_deref())
    }

    /// Of all its files, in bytes.
    pub fn size(&self) -> u64 {
        self.files().filter_map(|f| f.metadata().ok()).map(|m| m.len()).sum()
    }
}

/// An asset added.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Added {
    /// Its id in the library, when the source knows it already: a folder's
    /// path in it, iCloud's asset record name.
    pub id: Option<String>,
    /// The library had it already (iCloud knows its photos by content), so
    /// nothing was added.
    pub duplicate: bool,
    /// What was left out: a Live Photo's motion, which iCloud on the web
    /// does not take.
    pub note: Option<String>,
}

/// Why an asset could not be added, with a code the window knows:
/// `unsupported` (a kind of file the source does not take), `too_large`,
/// `over_quota` (the iCloud storage is full), `protected` (Advanced Data
/// Protection keeps the web out), `failed` (anything else, said in the
/// message).
#[derive(Debug)]
pub struct AddError {
    pub code: &'static str,
    pub message: String,
}

impl AddError {
    pub fn new(code: &'static str, message: impl Into<String>) -> Self {
        AddError { code, message: message.into() }
    }
}

impl std::fmt::Display for AddError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for AddError {}

/// A file's extension, in lower case.
pub fn extension(path: &Path) -> String {
    path.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase()
}

/// A random version 4 UUID, as browsers make them.
pub fn uuid() -> String {
    let mut b: [u8; 16] = rand::random();
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    let hex: String = b.iter().map(|x| format!("{x:02x}")).collect();
    format!("{}-{}-{}-{}-{}", &hex[..8], &hex[8..12], &hex[12..16], &hex[16..20], &hex[20..])
}

/// The assets the given files and folders make, in the order given (a
/// folder's files by name; hidden ones skipped), with a Live Photo's motion
/// paired to its photo as a folder pairs them: a video named as a photo
/// beside it (IMG_1234.HEIC with IMG_1234.MOV or IMG_1234_HEVC.MOV). Files
/// that are neither a photo nor a video come back on their own.
pub fn gather(paths: &[PathBuf]) -> (Vec<Addition>, Vec<PathBuf>) {
    let mut files = Vec::new();
    let mut other = Vec::new();
    let mut seen = HashSet::new();
    for path in paths {
        expand(path, &mut files, &mut other, &mut seen);
    }
    // Photos by folder and name without extension, for their motion.
    let stem = |p: &Path| -> String { p.with_extension("").to_string_lossy().to_lowercase() };
    let photos: HashSet<String> =
        files.iter().filter(|f| PHOTOS.contains(&extension(f).as_str())).map(|f| stem(f)).collect();
    let mut motion: HashMap<String, PathBuf> = HashMap::new();
    let mut assets = Vec::new();
    for f in files {
        if VIDEOS.contains(&extension(&f).as_str()) {
            let s = stem(&f);
            let photo = s.strip_suffix("_hevc").filter(|p| photos.contains(*p)).map(str::to_owned)
                .or_else(|| photos.contains(&s).then_some(s));
            if let Some(photo) = photo {
                motion.insert(photo, f);
                continue;
            }
        }
        assets.push(f);
    }
    let group = uuid();
    let additions = assets
        .into_iter()
        .map(|file| {
            let motion = if PHOTOS.contains(&extension(&file).as_str()) { motion.remove(&stem(&file)) } else { None };
            Addition { file, motion, group: group.clone() }
        })
        .collect();
    (additions, other)
}

fn expand(path: &Path, files: &mut Vec<PathBuf>, other: &mut Vec<PathBuf>, seen: &mut HashSet<PathBuf>) {
    if !seen.insert(path.to_owned()) {
        return;
    }
    if path.is_dir() {
        let Ok(entries) = std::fs::read_dir(path) else { return other.push(path.to_owned()) };
        let mut inside: Vec<PathBuf> = entries
            .flatten()
            .filter(|e| !e.file_name().to_string_lossy().starts_with('.'))
            .map(|e| e.path())
            .collect();
        inside.sort();
        for p in inside {
            expand(&p, files, other, seen);
        }
        return;
    }
    let ext = extension(path);
    if path.is_file() && (PHOTOS.contains(&ext.as_str()) || VIDEOS.contains(&ext.as_str())) {
        files.push(path.to_owned());
    } else {
        other.push(path.to_owned());
    }
}

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

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("pinhole-add-{name}-{}", std::process::id()));
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
    fn files_and_folders_make_assets_with_live_photos_paired() {
        let from = temp("gather");
        write(&from.join("IMG_1.HEIC"), b"photo");
        write(&from.join("IMG_1.MOV"), b"motion");
        write(&from.join("IMG_2.JPG"), b"photo");
        write(&from.join("IMG_2_HEVC.MOV"), b"motion");
        write(&from.join("clip.mp4"), b"video");
        write(&from.join("notes.txt"), b"text");
        std::fs::create_dir(from.join("more")).unwrap();
        write(&from.join("more/a.png"), b"photo");
        write(&from.join("more/.hidden.jpg"), b"photo");
        let (additions, other) = gather(&[from.join("IMG_1.HEIC"), from.join("IMG_1.MOV"), from.join("IMG_2.JPG"),
            from.join("IMG_2_HEVC.MOV"), from.join("clip.mp4"), from.join("notes.txt"), from.join("more"), from.join("clip.mp4")]);
        let got: Vec<(String, Option<String>)> = additions
            .iter()
            .map(|a| (a.file.file_name().unwrap().to_string_lossy().into(), a.motion.as_ref().map(|m| m.file_name().unwrap().to_string_lossy().into())))
            .collect();
        assert_eq!(got, vec![
            ("IMG_1.HEIC".into(), Some("IMG_1.MOV".into())),
            ("IMG_2.JPG".into(), Some("IMG_2_HEVC.MOV".into())),
            ("clip.mp4".into(), None),
            ("a.png".into(), None),
        ]);
        assert_eq!(other, vec![from.join("notes.txt")]);
        assert!(additions.iter().all(|a| a.group == additions[0].group));
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
    }

    #[test]
    fn uuids_look_like_a_browsers() {
        let u = uuid();
        assert_eq!(u.len(), 36);
        assert_eq!(&u[14..15], "4");
        assert_ne!(u, uuid());
    }
}
