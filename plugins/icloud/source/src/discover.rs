//! What could be added as iCloud sources, for a window's first run and its
//! Add button: the Apple IDs icloudpd is signed in to. See docs/service.md,
//! `discover`.

use std::collections::HashSet;
use std::path::Path;

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

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("pinhole-icloudpd-{name}-{}", std::process::id()));
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
        let _ = std::fs::remove_dir_all(&dir);
    }
}
