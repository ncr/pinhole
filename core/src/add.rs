//! Adding photos and videos to a library from files on this computer
//! (dropped on the window, or `pinhole add`). The files are grouped
//! into the assets they make, a Live Photo's photo and motion together,
//! and handed to the source one asset at a time (`Source::add`): a folder
//! copies them in, iCloud uploads them. The files themselves stay as they
//! are. See docs/sources.md.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

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
    fn uuids_look_like_a_browsers() {
        let u = uuid();
        assert_eq!(u.len(), 36);
        assert_eq!(&u[14..15], "4");
        assert_ne!(u, uuid());
    }
}
