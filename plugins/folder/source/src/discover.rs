//! What could be added as a folder source, for a window's first run and its
//! Add button: the folders of photos a person is likely to have. See
//! docs/service.md, `discover`.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde_json::{json, Value};


/// How long counting the folders' files may take, all of them together.
pub const BUDGET: Duration = Duration::from_secs(2);

/// The Pictures folder as the desktop names it (`XDG_PICTURES_DIR` in
/// user-dirs.dirs), else `~/Pictures`.
pub fn pictures_dir(home: &Path, config: &Path) -> PathBuf {
    let named = std::fs::read_to_string(config.join("user-dirs.dirs")).ok().and_then(|text| {
        text.lines().find_map(|line| {
            let value = line.trim().strip_prefix("XDG_PICTURES_DIR=")?.trim_matches('"');
            let path = value.replace("$HOME", &home.to_string_lossy());
            Some(PathBuf::from(path)).filter(|p| p.is_absolute() && p != home)
        })
    });
    named.unwrap_or_else(|| home.join("Pictures"))
}

/// Folders to offer, in the order to offer them: every DCIM folder on a
/// disk under `media` (a camera's card, a phone), the folders in Pictures,
/// then Pictures itself.
pub fn candidates(pictures: &Path, media: &Path) -> Vec<PathBuf> {
    let dirs = |parent: &Path| -> Vec<PathBuf> {
        let Ok(entries) = std::fs::read_dir(parent) else { return Vec::new() };
        let mut dirs: Vec<PathBuf> = entries
            .flatten()
            .filter(|e| !e.file_name().to_string_lossy().starts_with('.'))
            .filter(|e| e.path().is_dir())
            .map(|e| e.path())
            .collect();
        dirs.sort();
        dirs
    };
    let mut list: Vec<PathBuf> = dirs(media).into_iter().map(|disk| disk.join("DCIM")).filter(|d| d.is_dir()).collect();
    list.extend(dirs(pictures));
    if pictures.is_dir() {
        list.push(pictures.to_owned());
    }
    list
}

/// The folders `discover` offers (docs/service.md), those in `added` left
/// out, and those without photos.
pub fn folders(candidates: Vec<PathBuf>, added: &HashSet<PathBuf>, home: &Path, budget: Duration) -> Vec<Value> {
    let deadline = Instant::now() + budget;
    let mut folders = Vec::new();
    for path in candidates {
        if added.contains(&path) {
            continue;
        }
        let count = crate::count(&path, Some(deadline));
        if count.assets == 0 {
            continue;
        }
        folders.push(json!({
            "path": path,
            "name": crate::display_name(&path, home),
            "assets": count.assets,
            "live": count.live,
            "bytes": count.bytes,
            "partial": count.partial,
        }));
    }
    folders
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("pinhole-discover-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn file(path: &Path, bytes: usize) {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, vec![0u8; bytes]).unwrap();
    }

    #[test]
    fn pictures_is_where_the_desktop_says() {
        let dir = temp("pictures");
        let (home, config) = (dir.join("home"), dir.join("config"));
        assert_eq!(pictures_dir(&home, &config), home.join("Pictures"));
        file(&config.join("user-dirs.dirs"), 0);
        std::fs::write(config.join("user-dirs.dirs"), "# x\nXDG_PICTURES_DIR=\"$HOME/Obrazy\"\n").unwrap();
        assert_eq!(pictures_dir(&home, &config), home.join("Obrazy"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn folders_with_photos_are_offered_cards_first_and_counted_by_name() {
        let dir = temp("folders");
        let home = dir.join("home");
        let pictures = home.join("Pictures");
        let media = dir.join("media");
        file(&pictures.join("loose.png"), 10);
        // A Live Photo is one asset, its two files' bytes counted.
        file(&pictures.join("iCloud/2026/IMG_1.HEIC"), 100);
        file(&pictures.join("iCloud/2026/IMG_1_HEVC.MOV"), 50);
        file(&pictures.join("iCloud/2026/IMG_2.JPG"), 30);
        file(&pictures.join("Empty/readme.txt"), 5);
        file(&pictures.join(".hidden/x.jpg"), 5);
        file(&media.join("T7/DCIM/100CANON/IMG_9.CR3.jpg"), 7);
        file(&media.join("Stick/music.mp3"), 7);

        let list = candidates(&pictures, &media);
        assert_eq!(list, vec![media.join("T7/DCIM"), pictures.join("Empty"), pictures.join("iCloud"), pictures.clone()]);

        let added: HashSet<PathBuf> = [pictures.clone()].into();
        let found = folders(list, &added, &home, BUDGET);
        assert_eq!(json!(found), json!([
            {"path": media.join("T7/DCIM"), "name": "T7/DCIM", "assets": 1, "live": 0, "bytes": 7, "partial": false},
            {"path": pictures.join("iCloud"), "name": "Pictures/iCloud", "assets": 2, "live": 1, "bytes": 180, "partial": false},
        ]));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_folder_is_named_with_the_one_it_is_in_unless_that_is_home() {
        let home = Path::new("/home/me");
        assert_eq!(crate::display_name(Path::new("/home/me/Pictures/iCloud"), home), "Pictures/iCloud");
        assert_eq!(crate::display_name(Path::new("/home/me/Pictures"), home), "Pictures");
        assert_eq!(crate::display_name(Path::new("/photos"), home), "photos");
        assert_eq!(crate::display_name(Path::new("/run/media/me/T7/DCIM"), home), "T7/DCIM");
        assert_eq!(crate::display_name(Path::new("/"), home), "/");
    }
}
