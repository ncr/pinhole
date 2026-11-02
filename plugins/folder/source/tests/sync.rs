//! pinhole-core's sync over a folder: a library is only ever written by
//! its own source, a listing or thumbnails stopped part way lose nothing,
//! and a video with a dull thumbnail gets a cover once.

use std::path::PathBuf;

use pinhole_core::db;
use pinhole_core::source::{Source, Stopped};
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

/// Covers made before HDR video was tone mapped (cover::VERSION) are made
/// again once where the video is here: an HLG one's, not an SDR one's.
#[test]
fn an_hdr_videos_cover_from_before_is_made_again_once() {
    use std::process::{Command, Stdio};
    if Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_err() {
        return;
    }
    let dir = std::env::temp_dir().join(format!("pinhole-hdr-covers-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let (videos, library) = (dir.join("videos"), dir.join("library"));
    std::fs::create_dir_all(&videos).unwrap();
    std::fs::create_dir_all(&library).unwrap();
    for (name, hdr) in [("sdr.mp4", false), ("hlg.mp4", true)] {
        let mut ffmpeg = Command::new("ffmpeg");
        ffmpeg.args(["-v", "error", "-y", "-f", "lavfi", "-i", "color=black:s=64x36:d=3:r=10"]);
        if hdr {
            ffmpeg.args(["-pix_fmt", "yuv420p10le", "-x264-params", "colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc"]);
        } else {
            ffmpeg.args(["-pix_fmt", "yuv420p"]);
        }
        assert!(ffmpeg.arg(videos.join(name)).status().unwrap().success());
    }
    let source = FolderSource::new(videos);
    let mut conn = db::open(&library.join("library.sqlite")).unwrap();
    list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
    make_thumbs(&source, &mut conn, &library, 1, &|_, _| {}, &|| false).unwrap();
    assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 2);
    assert_eq!(db::meta(&conn, "covers").as_deref(), Some(pinhole_core::cover::VERSION));
    // As made by a Pinhole from before: marked, so that a cover made again
    // shows.
    let cover = |conn: &Connection, id: &str| -> String {
        conn.query_row("SELECT thumb_path FROM assets WHERE id = ?1", [id], |r| r.get(0)).unwrap()
    };
    let (sdr, hlg) = (cover(&conn, "sdr.mp4"), cover(&conn, "hlg.mp4"));
    let mut marker = Vec::new();
    image::codecs::jpeg::JpegEncoder::new(&mut marker)
        .encode(&[255u8; 64 * 36 * 3], 64, 36, image::ExtendedColorType::Rgb8)
        .unwrap();
    std::fs::write(library.join(&sdr), &marker).unwrap();
    std::fs::write(library.join(&hlg), &marker).unwrap();
    db::set_meta(&conn, "covers", "2").unwrap();
    assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 1);
    assert_eq!(cover(&conn, "hlg.mp4"), hlg);
    assert_ne!(std::fs::read(library.join(&hlg)).unwrap(), marker);
    assert!(library.join(medium_cover_path(&hlg)).exists());
    assert_eq!(std::fs::read(library.join(&sdr)).unwrap(), marker);
    assert_eq!(db::meta(&conn, "covers").as_deref(), Some(pinhole_core::cover::VERSION));
    // Once.
    assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 0);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn a_video_ffmpeg_turns_down_is_not_tried_for_a_cover_again_until_it_changes() {
    use std::process::{Command, Stdio};
    if Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_err() {
        return;
    }
    let dir = std::env::temp_dir().join(format!("pinhole-turned-down-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let (videos, library) = (dir.join("videos"), dir.join("library"));
    std::fs::create_dir_all(&videos).unwrap();
    std::fs::create_dir_all(&library).unwrap();
    let black = |seconds: &str| {
        let status = Command::new("ffmpeg")
            .args(["-v", "error", "-y", "-f", "lavfi", "-i", &format!("color=black:s=64x36:d={seconds}:r=10"), "-pix_fmt", "yuv420p"])
            .arg(videos.join("dark.mp4"))
            .status()
            .unwrap();
        assert!(status.success());
    };
    black("3");
    let source = FolderSource::new(videos.clone());
    let mut conn = db::open(&library.join("library.sqlite")).unwrap();
    list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
    make_thumbs(&source, &mut conn, &library, 1, &|_, _| {}, &|| false).unwrap();
    let key: String = conn.query_row("SELECT key FROM renditions WHERE kind = 'video'", [], |r| r.get(0)).unwrap();

    // Damaged under the library's feet (as an iCloud video fetched for a
    // cover may be): turned down, and kept in mind.
    std::fs::write(videos.join("dark.mp4"), b"\0\0\0\x18ftypmp42 and nothing after").unwrap();
    assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 0);
    assert!(pinhole_core::source::unreadable(&source.id(), &key).is_some());
    // Good again, but not looked at since (its key the same): not tried.
    black("3");
    assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 0);
    // Changed, and so listed with another key: its cover is made.
    black("4");
    list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
    make_thumbs(&source, &mut conn, &library, 1, &|_, _| {}, &|| false).unwrap();
    assert_eq!(make_covers(&source, &mut conn, &library, &|| false).unwrap(), 1);
    let _ = std::fs::remove_dir_all(&dir);
}
