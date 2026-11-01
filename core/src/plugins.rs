//! The plugins' switches, as the window keeps them in `plugins.json`
//! (`enabled`, by plugin id). pinholed only reads the file: the window
//! writes it, then sends `plugins_changed` (docs/service.md), and each sync
//! round looks at it again in case it was edited by hand. A source kind
//! follows its plugin's switch (kinds.rs).

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde_json::Value;

/// `~/.config/pinhole/plugins.json`.
pub fn plugins_path() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".config"))
        .join("pinhole/plugins.json")
}

/// The switches turned one way or the other, by plugin id. A plugin
/// with no entry is not in it: whether it is on is the caller's to say
/// (a built-in is, a plugin of your own is not). No file, or one that is
/// not JSON, turns nothing either way.
pub fn switches() -> HashMap<String, bool> {
    switches_in(&plugins_path())
}

pub fn switches_in(path: &Path) -> HashMap<String, bool> {
    let Some(written) = std::fs::read_to_string(path).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) else {
        return HashMap::new();
    };
    written["enabled"]
        .as_object()
        .map(|enabled| enabled.iter().filter_map(|(id, on)| Some((id.clone(), on.as_bool()?))).collect())
        .unwrap_or_default()
}

/// When the file last changed, and its size: read again only when these
/// differ from the last time. None when there is no file.
pub fn stamp(path: &Path) -> Option<(SystemTime, u64)> {
    let meta = std::fs::metadata(path).ok()?;
    Some((meta.modified().ok()?, meta.len()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn switches_are_what_enabled_says_and_nothing_else() {
        let dir = std::env::temp_dir().join(format!("pinhole-plugins-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("plugins.json");
        // No file: nothing turned.
        assert!(switches_in(&path).is_empty());
        assert_eq!(stamp(&path), None);
        // Not JSON, or JSON of another shape: nothing turned.
        for text in ["{not json", "[1, 2]", r#"{"enabled": [true]}"#, r#"{"settings": {}, "toured": ["x"]}"#] {
            std::fs::write(&path, text).unwrap();
            assert!(switches_in(&path).is_empty(), "{text}");
        }
        // Off and on as written; anything but a boolean is no switch.
        std::fs::write(&path, r#"{"enabled": {"pinhole.icloud": false, "pinhole.folder": true, "odd": "yes"}, "toured": []}"#)
            .unwrap();
        let s = switches_in(&path);
        assert_eq!(s.get("pinhole.icloud"), Some(&false));
        assert_eq!(s.get("pinhole.folder"), Some(&true));
        assert_eq!(s.len(), 2);
        assert!(stamp(&path).is_some());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
