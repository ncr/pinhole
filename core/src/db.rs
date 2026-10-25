//! The local library index. The window reads it directly, so the schema is
//! shaped for "the asset at row N" lookups: one row per asset, ordered by
//! capture date. Only what every source has is here; a source keeps the
//! rest in tables of its own beside these (docs/sources.md).

use std::collections::{BTreeMap, HashSet};
use std::path::Path;

use anyhow::Result;
use rusqlite::{params, Connection, OptionalExtension};

use crate::source::{Asset, Kind};

/// The schema this code reads and writes; `meta.schema`. An index without
/// one is from before sources, iCloud's, and is migrated as it is opened.
pub const SCHEMA: i64 = 2;

pub fn open(path: &Path) -> Result<Connection> {
    let mut db = Connection::open(path)?;
    db.pragma_update(None, "journal_mode", "WAL")?;
    db.pragma_update(None, "synchronous", "NORMAL")?;
    db.busy_timeout(std::time::Duration::from_secs(10))?;
    let old = db
        .query_row("SELECT 1 FROM pragma_table_info('assets') WHERE name = 'master_id'", [], |_| Ok(()))
        .optional()?
        .is_some();
    if old {
        migrate(&mut db, path)?;
    }
    create(&db)?;
    Ok(db)
}

fn create(db: &Connection) -> Result<()> {
    db.execute_batch(
        "CREATE TABLE IF NOT EXISTS assets (
            id TEXT PRIMARY KEY,
            item_type TEXT,
            filename TEXT,
            asset_date INTEGER,
            added_date INTEGER,
            favorite INTEGER NOT NULL DEFAULT 0,
            duration REAL,
            orientation INTEGER,
            has_location INTEGER NOT NULL DEFAULT 0,
            is_video INTEGER NOT NULL DEFAULT 0,
            is_live INTEGER NOT NULL DEFAULT 0,
            -- The original's, in bytes.
            size INTEGER,
            -- The local thumbnail, relative to the library directory, and
            -- what the grid needs to draw it before it has decoded it.
            thumb_path TEXT,
            thumb_w INTEGER,
            thumb_h INTEGER,
            avg_color INTEGER
        );
        CREATE INDEX IF NOT EXISTS assets_by_date ON assets(asset_date);
        -- The file that serves each kind a window asks for (thumb, medium,
        -- full, original, video, live), as the source chose it.
        CREATE TABLE IF NOT EXISTS renditions (
            asset_id TEXT NOT NULL,
            kind TEXT NOT NULL,
            -- The source's name for it: iCloud's JPEGMed, a folder's file.
            name TEXT NOT NULL,
            -- What its cached file and thumbnail are named by; changes when
            -- the file does.
            key TEXT NOT NULL,
            width INTEGER,
            height INTEGER,
            size INTEGER,
            file_type TEXT,
            PRIMARY KEY (asset_id, kind)
        ) WITHOUT ROWID;
        CREATE TRIGGER IF NOT EXISTS assets_forget_renditions AFTER DELETE ON assets BEGIN
            DELETE FROM renditions WHERE asset_id = old.id;
        END;
        CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);",
    )?;
    db.execute("INSERT OR IGNORE INTO meta(key, value) VALUES ('schema', ?1)", [SCHEMA.to_string()])?;
    Ok(())
}

/// An index from before sources (iCloud's, the only one there was) to
/// this schema, in place: a copy is saved beside it first, then one
/// transaction moves what only iCloud means into iCloud's tables and
/// fills `renditions`. The window's columns are left as they were.
fn migrate(db: &mut Connection, path: &Path) -> Result<()> {
    let backup = path.with_file_name("library.v1.sqlite");
    if !backup.exists() {
        // Whole even with a write-ahead log beside it, and an earlier copy
        // is never overwritten.
        let part = path.with_file_name("library.v1.sqlite.part");
        let _ = std::fs::remove_file(&part);
        db.execute("VACUUM INTO ?1", [part.to_string_lossy()])?;
        std::fs::rename(&part, &backup)?;
    }
    // Indexes made before the thumbnail columns existed get them first.
    for column in ["thumb_path TEXT", "thumb_w INTEGER", "thumb_h INTEGER", "avg_color INTEGER"] {
        match db.execute(&format!("ALTER TABLE assets ADD COLUMN {column}"), []) {
            Ok(_) => {}
            Err(e) if e.to_string().contains("duplicate column") => {}
            Err(e) => return Err(e.into()),
        }
    }
    let tx = db.transaction()?;
    // `renditions` and the rest, beside the old `assets`.
    create(&tx)?;
    crate::icloud::index::migrate_from_v1(&tx)?;
    tx.execute_batch(
        "ALTER TABLE assets ADD COLUMN size INTEGER;
        UPDATE assets SET size = (SELECT size FROM renditions WHERE asset_id = assets.id AND kind = 'original');
        ALTER TABLE assets DROP COLUMN rank;
        ALTER TABLE assets DROP COLUMN master_id;
        ALTER TABLE assets DROP COLUMN change_tag;
        ALTER TABLE assets DROP COLUMN subtype;
        INSERT OR REPLACE INTO meta(key, value)
            SELECT 'change_token', coalesce((SELECT value FROM meta WHERE key = 'sync_token'),
                                            (SELECT value FROM meta WHERE key = 'walk_sync_token'))
            WHERE EXISTS (SELECT 1 FROM meta WHERE key IN ('sync_token', 'walk_sync_token'));
        DELETE FROM meta WHERE key IN ('sync_token', 'walk_sync_token');
        INSERT OR REPLACE INTO meta(key, value) VALUES ('source', 'icloud');",
    )?;
    tx.execute("INSERT OR REPLACE INTO meta(key, value) VALUES ('schema', ?1)", [SCHEMA.to_string()])?;
    tx.commit()?;
    Ok(())
}

pub fn set_meta(db: &Connection, key: &str, value: &str) -> Result<()> {
    db.execute("INSERT OR REPLACE INTO meta(key, value) VALUES (?1, ?2)", params![key, value])?;
    Ok(())
}

pub fn meta(db: &Connection, key: &str) -> Option<String> {
    db.query_row("SELECT value FROM meta WHERE key = ?1", [key], |r| r.get(0)).ok()
}

/// What putting and removing assets changed, as far as a window cares.
#[derive(Debug, Default)]
pub struct Changes {
    /// Assets the index did not have.
    pub added: Vec<String>,
    /// Assets it had, with something about them changed.
    pub updated: Vec<String>,
    /// For the changed ones, what changed as far as Pinhole cares: counts by
    /// reason (favourite, edited, date, other).
    pub reasons: BTreeMap<&'static str, usize>,
    pub removed: Vec<String>,
}

impl Changes {
    pub fn is_empty(&self) -> bool {
        self.added.is_empty() && self.updated.is_empty() && self.removed.is_empty()
    }

    /// Whether anything a window shows changed: a source's own background
    /// work (Photos' on iCloud) touches many assets in ways Pinhole does
    /// not draw ("other").
    pub fn visible(&self) -> bool {
        !self.added.is_empty() || !self.removed.is_empty() || self.reasons.iter().any(|(r, n)| *r != "other" && *n > 0)
    }
}

/// The index as a source's catalog writes it: assets put and removed, and
/// the source's own tables beside them, through `conn`.
pub struct Index<'a> {
    pub conn: &'a mut Connection,
    /// Every asset put since this began: after a listing, what is not here
    /// is gone.
    pub seen: HashSet<String>,
    pub changes: Changes,
    /// Nothing is written: only `changes` says what would be.
    pub dry_run: bool,
}

impl<'a> Index<'a> {
    pub fn new(conn: &'a mut Connection) -> Self {
        Index { conn, seen: HashSet::new(), changes: Changes::default(), dry_run: false }
    }

    pub fn known(&self, id: &str) -> bool {
        self.conn.query_row("SELECT 1 FROM assets WHERE id = ?1", [id], |_| Ok(())).is_ok()
    }

    /// Store assets, new or changed, in one transaction.
    pub fn put(&mut self, assets: &[Asset]) -> Result<()> {
        let tx = self.conn.transaction()?;
        {
            let mut before = tx.prepare_cached(
                "SELECT a.favorite, a.asset_date, r.key FROM assets a
                 LEFT JOIN renditions r ON r.asset_id = a.id AND r.kind = 'thumb' WHERE a.id = ?1",
            )?;
            // An update in place, not a replace: the thumbnail columns
            // belong to whoever makes thumbnails and survive a re-listing.
            let mut asset_stmt = tx.prepare_cached(
                "INSERT INTO assets
                 (id, item_type, filename, asset_date, added_date, favorite, duration, orientation,
                  has_location, is_video, is_live, size)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)
                 ON CONFLICT(id) DO UPDATE SET
                   item_type = excluded.item_type, filename = excluded.filename,
                   asset_date = excluded.asset_date, added_date = excluded.added_date, favorite = excluded.favorite,
                   duration = excluded.duration, orientation = excluded.orientation,
                   has_location = excluded.has_location, is_video = excluded.is_video, is_live = excluded.is_live,
                   size = excluded.size",
            )?;
            let mut stale_thumb = tx.prepare_cached("UPDATE assets SET thumb_path = NULL WHERE id = ?1")?;
            let mut clear = tx.prepare_cached("DELETE FROM renditions WHERE asset_id = ?1")?;
            let mut rendition = tx.prepare_cached(
                "INSERT INTO renditions (asset_id, kind, name, key, width, height, size, file_type)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
            )?;
            for a in assets {
                let old: Option<(bool, Option<i64>, Option<String>)> =
                    before.query_row([&a.id], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?))).optional()?;
                let thumb = a.rendition(crate::source::Want::Thumb).map(|r| r.key.clone());
                match &old {
                    None => self.changes.added.push(a.id.clone()),
                    Some((favorite, date, old_thumb)) => {
                        let reason = if *favorite != a.favorite {
                            "favourite"
                        } else if *old_thumb != thumb {
                            "edited"
                        } else if *date != a.taken {
                            "date"
                        } else {
                            "other"
                        };
                        self.changes.updated.push(a.id.clone());
                        *self.changes.reasons.entry(reason).or_default() += 1;
                    }
                }
                self.seen.insert(a.id.clone());
                if self.dry_run {
                    continue;
                }
                asset_stmt.execute(params![
                    a.id, a.item_type, a.filename, a.taken, a.added, a.favorite, a.duration, a.orientation,
                    a.has_location, a.kind == Kind::Video, a.kind == Kind::Live, a.size,
                ])?;
                // An edit gives the photo a new thumbnail: forget the old one.
                if old.as_ref().is_some_and(|o| o.2.is_some() && o.2 != thumb) {
                    stale_thumb.execute([&a.id])?;
                }
                clear.execute([&a.id])?;
                for r in &a.renditions {
                    rendition.execute(params![
                        a.id, r.want.name(), r.name, r.key, r.width, r.height, r.size, r.file_type,
                    ])?;
                }
            }
        }
        tx.commit()?;
        Ok(())
    }

    /// Remove assets the source no longer has, with everything kept about
    /// them (the sources' tables follow by trigger).
    pub fn remove(&mut self, ids: &[String]) -> Result<()> {
        let tx = self.conn.transaction()?;
        for id in ids {
            let had = tx.query_row("SELECT 1 FROM assets WHERE id = ?1", [id], |_| Ok(())).is_ok();
            if had && !self.changes.removed.contains(id) {
                self.changes.removed.push(id.clone());
            }
            if !self.dry_run {
                tx.execute("DELETE FROM assets WHERE id = ?1", [id])?;
            }
        }
        tx.commit()?;
        Ok(())
    }

    /// Everything not put since this began: after a full listing, what the
    /// source no longer has.
    pub fn remove_unseen(&mut self) -> Result<usize> {
        let stale: Vec<String> = self
            .conn
            .prepare("SELECT id FROM assets")?
            .query_map([], |r| r.get::<_, String>(0))?
            .filter_map(|r| r.ok())
            .filter(|id| !self.seen.contains(id))
            .collect();
        self.remove(&stale)?;
        Ok(stale.len())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::source::{Rendition, Want};

    fn temp(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("pinhole-db-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn asset(id: &str, favorite: bool, thumb: &str) -> Asset {
        Asset {
            id: id.into(),
            filename: Some(format!("{id}.jpg")),
            item_type: Some("public.jpeg".into()),
            taken: Some(1000),
            added: Some(2000),
            kind: Kind::Photo,
            favorite,
            duration: None,
            orientation: Some(1),
            has_location: false,
            size: Some(10),
            renditions: vec![Rendition {
                want: Want::Thumb, name: "t".into(), key: thumb.into(),
                width: None, height: None, size: None, file_type: Some("public.jpeg".into()),
            }],
        }
    }

    /// The schema as it was before sources, with one photo (edited, so its
    /// full size is JPEGFull), one video and one Live Photo.
    const V1: &str = "
        CREATE TABLE assets (id TEXT PRIMARY KEY, rank INTEGER, master_id TEXT NOT NULL, change_tag TEXT,
            item_type TEXT, filename TEXT, asset_date INTEGER, added_date INTEGER,
            favorite INTEGER NOT NULL DEFAULT 0, duration REAL, orientation INTEGER, subtype INTEGER,
            has_location INTEGER NOT NULL DEFAULT 0, is_video INTEGER NOT NULL DEFAULT 0,
            is_live INTEGER NOT NULL DEFAULT 0, thumb_path TEXT, thumb_w INTEGER, thumb_h INTEGER, avg_color INTEGER);
        CREATE INDEX assets_by_date ON assets(asset_date);
        CREATE TABLE resources (asset_id TEXT NOT NULL, kind TEXT NOT NULL, width INTEGER, height INTEGER,
            size INTEGER, file_type TEXT, checksum TEXT, url TEXT, url_fetched_at INTEGER,
            PRIMARY KEY (asset_id, kind)) WITHOUT ROWID;
        CREATE TABLE raw_records (asset_id TEXT PRIMARY KEY, asset_json TEXT NOT NULL, master_json TEXT NOT NULL);
        CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
        INSERT INTO assets VALUES ('p', 0, 'mp', 'tp', 'public.heic', 'IMG_1.HEIC', 100, 200, 1, NULL, 6, 0, 1, 0, 0, 'thumbs/aa/c1.jpg', 480, 360, 123);
        INSERT INTO assets VALUES ('v', 1, 'mv', 'tv', 'com.apple.quicktime-movie', 'IMG_2.MOV', 300, 400, 0, 12.5, 1, 0, 0, 1, 0, NULL, NULL, NULL, NULL);
        INSERT INTO assets VALUES ('l', 2, 'ml', 'tl', 'public.heic', 'IMG_3.HEIC', 500, 600, 0, NULL, 1, 2, 0, 0, 1, NULL, NULL, NULL, NULL);
        INSERT INTO resources VALUES ('p', 'Original', 4032, 3024, 2000000, 'public.heic', 'c0', 'https://o', 50);
        INSERT INTO resources VALUES ('p', 'JPEGFull', 4032, 3024, 900000, 'public.jpeg', 'c2', NULL, NULL);
        INSERT INTO resources VALUES ('p', 'JPEGMed', 2048, 1536, 300000, 'public.jpeg', 'c3', NULL, NULL);
        INSERT INTO resources VALUES ('p', 'JPEGThumb', 480, 360, 30000, 'public.jpeg', 'c1', NULL, NULL);
        INSERT INTO resources VALUES ('v', 'Original', 1920, 1080, 9000000, 'com.apple.quicktime-movie', 'c4', NULL, NULL);
        INSERT INTO resources VALUES ('v', 'VidMed', 1280, 720, 3000000, 'com.apple.quicktime-movie', NULL, NULL, NULL);
        INSERT INTO resources VALUES ('l', 'Original', 4032, 3024, 2500000, 'public.heic', 'c5', NULL, NULL);
        INSERT INTO resources VALUES ('l', 'OriginalVidCompl', 1440, 1080, 3000000, 'com.apple.quicktime-movie', 'c6', NULL, NULL);
        INSERT INTO resources VALUES ('l', 'JPEGLarge', 1600, 1200, 400000, 'public.jpeg', 'c7', NULL, NULL);
        INSERT INTO meta VALUES ('walk_sync_token', 'old'), ('sync_token', 'newer'), ('library_version', '6');";

    #[test]
    fn an_index_from_before_sources_is_migrated_in_place() {
        let dir = temp("migrate");
        let path = dir.join("library.sqlite");
        Connection::open(&path).unwrap().execute_batch(V1).unwrap();
        let conn = open(&path).unwrap();
        assert!(dir.join("library.v1.sqlite").exists());

        let renditions: Vec<String> = conn
            .prepare("SELECT asset_id || ' ' || kind || ' ' || name || ' ' || key FROM renditions ORDER BY 1").unwrap()
            .query_map([], |r| r.get(0)).unwrap().map(|r| r.unwrap()).collect();
        assert_eq!(renditions, [
            "l full Original c5", "l live OriginalVidCompl c6", "l medium JPEGLarge c7", "l original Original c5",
            "p full JPEGFull c2", "p medium JPEGMed c3", "p original Original c0", "p thumb JPEGThumb c1",
            "v full Original c4", "v original Original c4", "v video VidMed v-VidMed",
        ]);
        // The window's columns as they were, iCloud's in its own table.
        let (thumb, favorite, size): (String, bool, i64) = conn
            .query_row("SELECT thumb_path, favorite, size FROM assets WHERE id = 'p'", [], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))
            .unwrap();
        assert_eq!((thumb.as_str(), favorite, size), ("thumbs/aa/c1.jpg", true, 2_000_000));
        let gone: i64 = conn
            .query_row("SELECT count(*) FROM pragma_table_info('assets') WHERE name IN ('rank', 'master_id', 'change_tag', 'subtype')", [], |r| r.get(0))
            .unwrap();
        assert_eq!(gone, 0);
        let (master, rank, url): (String, i64, String) = conn
            .query_row("SELECT m.master_id, m.rank, r.url FROM icloud_assets m
                        JOIN icloud_resources r ON r.asset_id = m.asset_id AND r.kind = 'Original' WHERE m.asset_id = 'p'",
                       [], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))
            .unwrap();
        assert_eq!((master.as_str(), rank, url.as_str()), ("mp", 0, "https://o"));
        assert_eq!(meta(&conn, "change_token").as_deref(), Some("newer"));
        assert_eq!(meta(&conn, "sync_token"), None);
        assert_eq!(meta(&conn, "source").as_deref(), Some("icloud"));
        assert_eq!(meta(&conn, "schema"), Some(SCHEMA.to_string()));
        assert_eq!(meta(&conn, "library_version").as_deref(), Some("6"));

        // Removing an asset takes iCloud's rows with it.
        drop(conn);
        let mut conn = open(&path).unwrap();
        Index::new(&mut conn).remove(&["p".into()]).unwrap();
        let left: i64 = conn
            .query_row("SELECT (SELECT count(*) FROM icloud_assets WHERE asset_id = 'p')
                              + (SELECT count(*) FROM icloud_resources WHERE asset_id = 'p')
                              + (SELECT count(*) FROM renditions WHERE asset_id = 'p')", [], |r| r.get(0))
            .unwrap();
        assert_eq!(left, 0);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn put_tells_new_from_changed_and_forgets_an_edited_thumbnail() {
        let dir = temp("put");
        let mut conn = open(&dir.join("library.sqlite")).unwrap();
        Index::new(&mut conn).put(&[asset("a", false, "k1"), asset("b", false, "k1")]).unwrap();
        conn.execute("UPDATE assets SET thumb_path = 'thumbs/x.jpg'", []).unwrap();

        let mut index = Index::new(&mut conn);
        index.put(&[asset("a", true, "k1"), asset("b", false, "k2"), asset("c", false, "k3")]).unwrap();
        assert_eq!(index.changes.added, ["c"]);
        assert_eq!(index.changes.updated, ["a", "b"]);
        assert_eq!(index.changes.reasons.get("favourite"), Some(&1));
        assert_eq!(index.changes.reasons.get("edited"), Some(&1));
        let thumbs: Vec<Option<String>> = conn
            .prepare("SELECT thumb_path FROM assets ORDER BY id").unwrap()
            .query_map([], |r| r.get(0)).unwrap().map(|r| r.unwrap()).collect();
        assert_eq!(thumbs, [Some("thumbs/x.jpg".into()), None, None]);

        let mut index = Index::new(&mut conn);
        index.put(&[asset("a", true, "k1")]).unwrap();
        assert_eq!(index.remove_unseen().unwrap(), 2);
        assert_eq!(index.changes.removed, ["b", "c"]);
        let left: i64 = conn.query_row("SELECT count(*) FROM renditions", [], |r| r.get(0)).unwrap();
        assert_eq!(left, 1);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
