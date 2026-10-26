//! What could be added as a source, for a window's first run and its Add
//! button: the Apple IDs icloudpd is signed in to, and the folders of
//! photos a person is likely to have. See docs/service.md, `discover`.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::folder;

/// How long counting the folders' files may take, all of them together.
pub const BUDGET: Duration = Duration::from_secs(2);

/// An Apple ID as pyicloud names its session files after it: everything
/// but letters, digits and `_` removed.
pub fn stem(apple_id: &str) -> String {
    apple_id.chars().filter(|c| c.is_alphanumeric() || *c == '_').collect()
}

/// The Apple IDs icloudpd keeps a session for in `dir`, by the names of
/// its files alone (`<stem>` and `<stem>.session`; what is in them is
/// never read), leaving out those signed in to here already.
pub fn icloudpd_stems(dir: &Path, signed_in: &[String]) -> Vec<String> {
    let ours: HashSet<String> = signed_in.iter().map(|a| stem(a)).collect();
    let Ok(entries) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut stems: Vec<String> = entries
        .flatten()
        .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
        .filter_map(|e| e.file_name().into_string().ok())
        .map(|name| name.strip_suffix(".session").map_or(name.clone(), str::to_owned))
        .filter(|s| !s.is_empty() && s.chars().all(|c| c.is_alphanumeric() || c == '_'))
        .filter(|s| !ours.contains(s))
        .collect();
    stems.sort();
    stems.dedup();
    stems
}

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

/// What `discover` answers: `icloudpd` and `folders` (docs/service.md),
/// the folders in `added` left out, and those without photos.
pub fn discover(stems: Vec<String>, candidates: Vec<PathBuf>, added: &HashSet<PathBuf>, home: &Path, budget: Duration) -> Value {
    let deadline = Instant::now() + budget;
    let mut folders = Vec::new();
    for path in candidates {
        if added.contains(&path) {
            continue;
        }
        let count = folder::count(&path, Some(deadline));
        if count.assets == 0 {
            continue;
        }
        folders.push(json!({
            "path": path,
            "name": folder::display_name(&path, home),
            "assets": count.assets,
            "live": count.live,
            "bytes": count.bytes,
            "partial": count.partial,
        }));
    }
    json!({"icloudpd": stems, "folders": folders})
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
    fn stems_are_file_names_without_the_apple_ids_signed_in_to() {
        assert_eq!(stem("me.you@example.com"), "meyouexamplecom");
        let dir = temp("stems");
        file(&dir.join("meexamplecom"), 1);
        file(&dir.join("meexamplecom.session"), 1);
        file(&dir.join("youexamplecom.session"), 1);
        file(&dir.join("notes.txt"), 1);
        assert_eq!(icloudpd_stems(&dir, &[]), vec!["meexamplecom", "youexamplecom"]);
        assert_eq!(icloudpd_stems(&dir, &["you@example.com".into()]), vec!["meexamplecom"]);
        assert!(icloudpd_stems(&dir.join("absent"), &[]).is_empty());
    }

    #[test]
    fn pictures_is_where_the_desktop_says() {
        let dir = temp("pictures");
        let (home, config) = (dir.join("home"), dir.join("config"));
        assert_eq!(pictures_dir(&home, &config), home.join("Pictures"));
        file(&config.join("user-dirs.dirs"), 0);
        std::fs::write(config.join("user-dirs.dirs"), "# x\nXDG_PICTURES_DIR=\"$HOME/Obrazy\"\n").unwrap();
        assert_eq!(pictures_dir(&home, &config), home.join("Obrazy"));
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
        let found = discover(vec!["meexamplecom".into()], list, &added, &home, BUDGET);
        assert_eq!(found["icloudpd"], json!(["meexamplecom"]));
        assert_eq!(found["folders"], json!([
            {"path": media.join("T7/DCIM"), "name": "T7/DCIM", "assets": 1, "live": 0, "bytes": 7, "partial": false},
            {"path": pictures.join("iCloud"), "name": "Pictures/iCloud", "assets": 2, "live": 1, "bytes": 180, "partial": false},
        ]));
    }

    #[test]
    fn a_folder_is_named_with_the_one_it_is_in_unless_that_is_home() {
        let home = Path::new("/home/me");
        assert_eq!(folder::display_name(Path::new("/home/me/Pictures/iCloud"), home), "Pictures/iCloud");
        assert_eq!(folder::display_name(Path::new("/home/me/Pictures"), home), "Pictures");
        assert_eq!(folder::display_name(Path::new("/photos"), home), "photos");
        assert_eq!(folder::display_name(Path::new("/run/media/me/T7/DCIM"), home), "T7/DCIM");
        assert_eq!(folder::display_name(Path::new("/"), home), "/");
    }
}
