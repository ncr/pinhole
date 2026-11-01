//! pinhole-core's sync over a folder: a library is only ever written by
//! its own source, a listing or thumbnails stopped part way lose nothing,
//! and a video with a dull thumbnail gets a cover once.

use std::path::PathBuf;

use pinhole_core::db;
use pinhole_core::source::Stopped;
use pinhole_core::sync::{claim, list, make_covers, make_thumbs, medium_cover_path};
use pinhole_folder::FolderSource;
use rusqlite::Connection;

#[test]
fn a_library_is_only_ever_written_by_its_own_source() {
        let dir = std::env::temp_dir().join(format!("pinhole-claim-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(dir.join("thumbs")).unwrap();
    let mut conn = db::open(&dir.join("library.sqlite")).unwrap();
    let photos = FolderSource::new(PathBuf::from("/photos"));
    let others = FolderSource::new(PathBuf::from("/other"));

    // A new library becomes the first source's, and no other's.
    claim(&photos, &mut conn, &dir).unwrap();
    conn.execute("INSERT INTO assets (id, thumb_path) VALUES ('a.jpg', 'thumbs/x.jpg')", []).unwrap();
    claim(&photos, &mut conn, &dir).unwrap();
    assert!(claim(&others, &mut conn, &dir).is_err());
    assert_eq!(conn.query_row("SELECT count(*) FROM assets", [], |r| r.get::<_, i64>(0)).unwrap(), 1);
    conn.execute("DELETE FROM assets", []).unwrap();
    // iCloud's is never touched by a folder, nor one that says nothing
    // of where it came from.
    db::set_meta(&conn, "source", "icloud:me@example.com").unwrap();
    conn.execute("INSERT INTO assets (id) VALUES ('B5379A3C')", []).unwrap();
    assert!(claim(&photos, &mut conn, &dir).is_err());
    conn.execute("DELETE FROM meta WHERE key = 'source'", []).unwrap();
    assert!(claim(&photos, &mut conn, &dir).is_err());
    assert_eq!(conn.query_row("SELECT count(*) FROM assets", [], |r| r.get::<_, i64>(0)).unwrap(), 1);
    assert!(dir.join("thumbs").exists());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn a_listing_or_thumbnails_stopped_part_way_lose_nothing() {
        let dir = std::env::temp_dir().join(format!("pinhole-stop-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let (photos, library) = (dir.join("photos"), dir.join("library"));
    std::fs::create_dir_all(&photos).unwrap();
    std::fs::create_dir_all(&library).unwrap();
    for name in ["a.jpg", "b.jpg", "c.jpg"] {
        image::RgbImage::new(4, 4).save(photos.join(name)).unwrap();
    }
    let source = FolderSource::new(photos.clone());
    let mut conn = db::open(&library.join("library.sqlite")).unwrap();
    let count = |conn: &Connection, sql: &str| conn.query_row(sql, [], |r| r.get::<_, i64>(0)).unwrap();
    list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
    assert_eq!(count(&conn, "SELECT count(*) FROM assets"), 3);

    // Stopped: nothing put, and above all nothing removed for not being
    // seen, though a file went meanwhile.
    std::fs::remove_file(photos.join("c.jpg")).unwrap();
    let e = list(&source, &mut conn, &|_, _| {}, &|| true).unwrap_err();
    assert!(e.downcast_ref::<Stopped>().is_some());
    assert_eq!(count(&conn, "SELECT count(*) FROM assets"), 3);
    let e = make_thumbs(&source, &mut conn, &library, 2, &|_, _| {}, &|| true).unwrap_err();
    assert!(e.downcast_ref::<Stopped>().is_some());
    assert_eq!(count(&conn, "SELECT count(thumb_path) FROM assets"), 0);

    // Shown again: listed again, and the thumbnails made.
    list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
    assert_eq!(count(&conn, "SELECT count(*) FROM assets"), 2);
    let made = make_thumbs(&source, &mut conn, &library, 2, &|_, _| {}, &|| false).unwrap();
    assert_eq!(made.made, 2);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn a_video_with_a_dull_thumbnail_gets_a_cover_once() {
        use std::process::{Command, Stdio};
    if Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_err() {
        return;
    }
    let dir = std::env::temp_dir().join(format!("pinhole-covers-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let (videos, library) = (dir.join("videos"), dir.join("library"));
    std::fs::create_dir_all(&videos).unwrap();
    std::fs::create_dir_all(&library).unwrap();
    // Black all through: its thumbnail is dull, and so is any cover.
    let status = Command::new("ffmpeg")
        .args(["-v", "error", "-y", "-f", "lavfi", "-i", "color=black:s=64x36:d=3:r=10", "-pix_fmt", "yuv420p"])
        .arg(videos.join("dark.mp4"))
        .status()
        .unwrap();
    assert!(status.success());
    let source = FolderSource::new(videos);
    let mut conn = db::open(&library.join("library.sqlite")).unwrap();
    list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
    make_thumbs(&source, &mut conn, &library, 1, &|_, _| {}, &|| false).unwrap();
    assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 1);
    let thumb: String = conn.query_row("SELECT thumb_path FROM assets", [], |r| r.get(0)).unwrap();
    assert!(thumb.ends_with("-cover.jpg") && library.join(&thumb).exists());
    assert!(library.join(medium_cover_path(&thumb)).exists());
    // Made once, never again, however dull.
    assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 0);
    let _ = std::fs::remove_dir_all(&dir);
}
