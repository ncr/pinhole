//! The local library index. The window reads it directly, so the schema is
//! shaped for "the asset at row N" lookups: one row per asset, ordered by
//! capture date. Only what every source has is here; a source keeps the
//! rest in tables of its own beside these (docs/sources.md).

use std::collections::{BTreeMap, HashSet};
use std::path::Path;

use anyhow::Result;
use rusqlite::{params, Connection, OptionalExtension};

use crate::source::{Asset, Kind};

pub fn open(path: &Path) -> Result<Connection> {
    let db = Connection::open(path)?;
    db.busy_timeout(std::time::Duration::from_secs(10))?;
    // Before anything is written, the journal mode too: a library of a
    // later schema is left exactly as it is.
    let version: i64 = db.query_row("PRAGMA user_version", [], |r| r.get(0))?;
    if version > SCHEMA {
        anyhow::bail!("this library was made by a later Pinhole (schema {version}, this one knows {SCHEMA})");
    }
    db.pragma_update(None, "journal_mode", "WAL")?;
    db.pragma_update(None, "synchronous", "NORMAL")?;
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
            -- A Live Photo's effect as set on the iPhone: loop, bounce,
            -- long_exposure, or NULL.
            live_effect TEXT,
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
    migrate(db)
}

/// The index's schema, kept as `PRAGMA user_version`. Readers (the window,
/// plugins' helpers) may read a library of a later schema as long as the
/// columns they read mean what they did; a writer never writes one later
/// than its own. Each step up is a step in `migrate`.
pub const SCHEMA: i64 = 3;

/// Bring a library up to `SCHEMA`, one step at a time, each in a
/// transaction with the version it reaches.
fn migrate(db: &Connection) -> Result<()> {
    let version: i64 = db.query_row("PRAGMA user_version", [], |r| r.get(0))?;
    if version < 1 {
        // 0 to 1, from before the schema was numbered: a library from
        // before effects were kept gets the column, and the whole library
        // listed again at the next round to fill it (the change feed only
        // tells of what changes).
        let tx = db.unchecked_transaction()?;
        if tx.prepare("SELECT live_effect FROM assets LIMIT 0").is_err() {
            tx.execute_batch(
                "ALTER TABLE assets ADD COLUMN live_effect TEXT;
                 DELETE FROM meta WHERE key = 'change_token';",
            )?;
        }
        tx.execute_batch("PRAGMA user_version = 1")?;
        tx.commit()?;
    }
    if version < 2 {
        let tx = db.unchecked_transaction()?;
        tx.execute_batch(
            "ALTER TABLE assets ADD COLUMN latitude REAL;
             ALTER TABLE assets ADD COLUMN longitude REAL;
             CREATE INDEX assets_with_location ON assets(id) WHERE latitude IS NOT NULL;
             DELETE FROM meta WHERE key = 'change_token';
             PRAGMA user_version = 2;",
        )?;
        tx.commit()?;
    }
    if version < 3 {
        // The camera, as the photo's own file says, read where a file of it
        // is on this computer (sync::read_cameras): no listing again.
        let tx = db.unchecked_transaction()?;
        tx.execute_batch(
            "ALTER TABLE assets ADD COLUMN camera_make TEXT;
             ALTER TABLE assets ADD COLUMN camera_model TEXT;
             ALTER TABLE assets ADD COLUMN lens_model TEXT;
             ALTER TABLE assets ADD COLUMN camera_read INTEGER NOT NULL DEFAULT 0;
             PRAGMA user_version = 3;",
        )?;
        tx.commit()?;
    }
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
    /// Asked before each `put`: true gives up with `Stopped`.
    pub stop: Option<&'a (dyn Fn() -> bool + Sync)>,
}

impl<'a> Index<'a> {
    pub fn new(conn: &'a mut Connection) -> Self {
        Index { conn, seen: HashSet::new(), changes: Changes::default(), stop: None }
    }

    /// Whoever listed into this should give up (see `stop`).
    pub fn stopping(&self) -> bool {
        self.stop.is_some_and(|stop| stop())
    }

    pub fn known(&self, id: &str) -> bool {
        self.conn.query_row("SELECT 1 FROM assets WHERE id = ?1", [id], |_| Ok(())).is_ok()
    }

    /// Store assets, new or changed, in one transaction.
    pub fn put(&mut self, assets: &[Asset]) -> Result<()> {
        if self.stopping() {
            return Err(crate::source::Stopped.into());
        }
        let tx = self.conn.transaction()?;
        {
            let mut before = tx.prepare_cached(
                "SELECT a.favorite, a.asset_date, r.key, a.live_effect, a.latitude, a.longitude, a.has_location FROM assets a
                 LEFT JOIN renditions r ON r.asset_id = a.id AND r.kind = 'thumb' WHERE a.id = ?1",
            )?;
            // An update in place, not a replace: the thumbnail columns
            // belong to whoever makes thumbnails and survive a re-listing.
            let mut asset_stmt = tx.prepare_cached(
                "INSERT INTO assets
                 (id, item_type, filename, asset_date, added_date, favorite, duration, orientation,
                  has_location, is_video, is_live, size, live_effect, latitude, longitude)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)
                 ON CONFLICT(id) DO UPDATE SET
                   item_type = excluded.item_type, filename = excluded.filename,
                   asset_date = excluded.asset_date, added_date = excluded.added_date, favorite = excluded.favorite,
                   duration = excluded.duration, orientation = excluded.orientation,
                   has_location = excluded.has_location, is_video = excluded.is_video, is_live = excluded.is_live,
                   size = excluded.size, live_effect = excluded.live_effect,
                   latitude = excluded.latitude, longitude = excluded.longitude",
            )?;
            let mut stale_thumb = tx.prepare_cached("UPDATE assets SET thumb_path = NULL WHERE id = ?1")?;
            let mut clear = tx.prepare_cached("DELETE FROM renditions WHERE asset_id = ?1")?;
            let mut rendition = tx.prepare_cached(
                "INSERT INTO renditions (asset_id, kind, name, key, width, height, size, file_type)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
            )?;
            for a in assets {
                let old = before.query_row([&a.id], |r| {
                    Ok((r.get::<_, bool>(0)?, r.get::<_, Option<i64>>(1)?,
                        r.get::<_, Option<String>>(2)?, r.get::<_, Option<String>>(3)?,
                        (r.get::<_, Option<f64>>(4)?, r.get::<_, Option<f64>>(5)?), r.get::<_, bool>(6)?))
                }).optional()?;
                let (latitude, longitude) = a.location.map(|p| p.degrees())
                    .map_or((None, None), |(lat, lon)| (Some(lat), Some(lon)));
                let has_location = a.has_location || a.location.is_some();
                let effect = a.effect.map(|e| e.name());
                let thumb = a.rendition(crate::source::Want::Thumb).map(|r| r.key.clone());
                match &old {
                    None => self.changes.added.push(a.id.clone()),
                    Some((favorite, date, old_thumb, old_effect, old_location, old_has_location)) => {
                        let reason = if *favorite != a.favorite {
                            "favourite"
                        } else if *old_thumb != thumb || old_effect.as_deref() != effect {
                            "edited"
                        } else if *date != a.taken {
                            "date"
                        } else if *old_location != (latitude, longitude) || *old_has_location != has_location {
                            "location"
                        } else {
                            "other"
                        };
                        self.changes.updated.push(a.id.clone());
                        *self.changes.reasons.entry(reason).or_default() += 1;
                    }
                }
                self.seen.insert(a.id.clone());
                asset_stmt.execute(params![
                    a.id, a.item_type, a.filename, a.taken, a.added, a.favorite, a.duration, a.orientation,
                    has_location, a.kind == Kind::Video, a.kind == Kind::Live, a.size, effect, latitude, longitude,
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
            tx.execute("DELETE FROM assets WHERE id = ?1", [id])?;
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
            location: None,
            size: Some(10),
            effect: None,
            renditions: vec![Rendition {
                want: Want::Thumb, name: "t".into(), key: thumb.into(),
                width: None, height: None, size: None, file_type: Some("public.jpeg".into()),
            }],
        }
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

    #[test]
    fn a_library_from_before_effects_gets_the_column_and_lists_again() {
        let dir = temp("effects");
        let path = dir.join("library.sqlite");
        {
            let old = Connection::open(&path).unwrap();
            old.execute_batch(
                "CREATE TABLE assets (id TEXT PRIMARY KEY, item_type TEXT, filename TEXT, asset_date INTEGER,
                   added_date INTEGER, favorite INTEGER NOT NULL DEFAULT 0, duration REAL, orientation INTEGER,
                   has_location INTEGER NOT NULL DEFAULT 0, is_video INTEGER NOT NULL DEFAULT 0,
                   is_live INTEGER NOT NULL DEFAULT 0, size INTEGER, thumb_path TEXT, thumb_w INTEGER,
                   thumb_h INTEGER, avg_color INTEGER);
                 CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
                 INSERT INTO meta VALUES ('change_token', 't1');",
            )
            .unwrap();
        }
        let mut conn = open(&path).unwrap();
        assert_eq!(meta(&conn, "change_token"), None);
        let mut bounce = asset("a", false, "k1");
        bounce.kind = Kind::Live;
        bounce.effect = Some(crate::source::Effect::Bounce);
        Index::new(&mut conn).put(&[bounce.clone()]).unwrap();
        set_meta(&conn, "change_token", "t2").unwrap();
        drop(conn);

        // Opened again, the token stays; an effect changed on the iPhone
        // is an edit.
        let mut conn = open(&path).unwrap();
        assert_eq!(meta(&conn, "change_token").as_deref(), Some("t2"));
        let effect: Option<String> = conn.query_row("SELECT live_effect FROM assets", [], |r| r.get(0)).unwrap();
        assert_eq!(effect.as_deref(), Some("bounce"));
        bounce.effect = Some(crate::source::Effect::Loop);
        let mut index = Index::new(&mut conn);
        index.put(&[bounce]).unwrap();
        assert_eq!(index.changes.reasons.get("edited"), Some(&1));
        let version: i64 = conn.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(version, SCHEMA);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn location_only_changes_notify_readers_and_can_be_removed() {
        use crate::source::Location;
        let mut conn = Connection::open_in_memory().unwrap();
        create(&conn).unwrap();
        let mut photo = asset("a", false, "k1");
        Index::new(&mut conn).put(&[photo.clone()]).unwrap();
        for point in [Location::new(0.0, -22.5), None] {
            photo.location = point;
            let mut index = Index::new(&mut conn);
            index.put(&[photo.clone()]).unwrap();
            assert!(index.changes.visible());
            assert_eq!(index.changes.reasons.get("location"), Some(&1));
            let stored = conn.query_row("SELECT latitude, longitude, has_location FROM assets", [], |r| {
                Ok((r.get(0)?, r.get(1)?, r.get::<_, bool>(2)?))
            }).unwrap();
            let expected = point.map_or((None, None, false), |p| {
                let (lat, lon) = p.degrees();
                (Some(lat), Some(lon), true)
            });
            assert_eq!(stored, expected);
        }
    }

    #[test]
    fn a_library_of_a_later_schema_is_not_written() {
        let dir = temp("later");
        let path = dir.join("library.sqlite");
        // A later schema with none of this one's tables, in the journal
        // mode SQLite starts with.
        Connection::open(&path)
            .unwrap()
            .execute_batch(&format!("CREATE TABLE things (x); INSERT INTO things VALUES (1); PRAGMA user_version = {}", SCHEMA + 1))
            .unwrap();
        let before = std::fs::read(&path).unwrap();
        let error = open(&path).unwrap_err().to_string();
        assert!(error.contains("later Pinhole"), "{error}");
        // Not a byte of it changed, and nothing beside it made.
        assert_eq!(std::fs::read(&path).unwrap(), before);
        let check = Connection::open(&path).unwrap();
        let tables: Vec<String> = check
            .prepare("SELECT name FROM sqlite_master ORDER BY name").unwrap()
            .query_map([], |r| r.get(0)).unwrap().map(|r| r.unwrap()).collect();
        assert_eq!(tables, ["things"]);
        let mode: String = check.query_row("PRAGMA journal_mode", [], |r| r.get(0)).unwrap();
        assert_eq!(mode, "delete");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
