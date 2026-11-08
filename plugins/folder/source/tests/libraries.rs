//! A folder's library, among the others in the folder Pinhole keeps
//! everything in (pinhole-core's `storage::library_for`): one to a path,
//! never another's, and always the same one.

use std::path::{Path, PathBuf};

use pinhole_core::storage::library_for;
use pinhole_folder::FolderSource;

fn temp(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("pinhole-folder-libraries-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn folders_whose_paths_differ_have_libraries_apart() {
    let location = temp("names");
    let for_path = |p: &str| library_for(&location, &FolderSource::new(PathBuf::from(p)));
    assert_ne!(for_path("/a-b/c"), for_path("/a/b-c"));
    assert_ne!(for_path("/root"), for_path("/"));
    // A tilde of its own is told apart from the hash's.
    assert_ne!(for_path("/home/me/My-Photos"), for_path("/home/me/My/Photos~7f094097f3a39b1b"));
    assert_eq!(for_path("/home/me/Pictures"), location.join("library/folder/home-me-Pictures"));
    // The same as the window's (Storage::library) test of this path.
    assert_eq!(for_path("/home/me/My-Photos"), location.join("library/folder/home-me-My-Photos~7f094097f3a39b1b"));

    // A folder whose name another's library has: one by its id.
    let own = location.join("library/folder/a-b-c");
    std::fs::create_dir_all(&own).unwrap();
    let db = pinhole_core::db::open(&own.join("library.sqlite")).unwrap();
    pinhole_core::db::set_meta(&db, "source", "folder:/elsewhere").unwrap();
    drop(db);
    assert_eq!(for_path("/a/b/c"), location.join("library/folder/a-b-c~43215282df502c0e"));
    // One with no owner yet is anyone's to claim.
    assert_eq!(for_path("/x/y"), location.join("library/folder/x-y"));
    // That name by its id another's too: the next.
    let claimed = |dir: &Path, owner: &str| {
        std::fs::create_dir_all(dir).unwrap();
        pinhole_core::db::set_meta(&pinhole_core::db::open(&dir.join("library.sqlite")).unwrap(), "source", owner).unwrap();
    };
    let by_id = location.join("library/folder/a-b-c~43215282df502c0e");
    claimed(&by_id, "folder:/a/b/c~43215282df502c0e");
    let next = location.join("library/folder/a-b-c~43215282df502c0e-1");
    assert_eq!(for_path("/a/b/c"), next);
    // Its own there, it is kept to, even once its name, and the one by
    // its id before it, are no one's.
    claimed(&next, "folder:/a/b/c");
    assert_eq!(for_path("/a/b/c"), next);
    std::fs::remove_dir_all(&own).unwrap();
    assert_eq!(for_path("/a/b/c"), next);
    std::fs::remove_dir_all(&by_id).unwrap();
    assert_eq!(for_path("/a/b/c"), next);
    let _ = std::fs::remove_dir_all(&location);
}
