//! The local library index. The window reads it directly, so the schema is
//! shaped for "the asset at row N" lookups: one row per asset, ordered by
//! capture date.

use std::path::Path;

use anyhow::Result;
use rusqlite::{params, Connection};

use crate::source::Asset;

pub fn open(path: &Path) -> Result<Connection> {
    let db = Connection::open(path)?;
    db.pragma_update(None, "journal_mode", "WAL")?;
    db.pragma_update(None, "synchronous", "NORMAL")?;
    db.execute_batch(
        "CREATE TABLE IF NOT EXISTS assets (
            id TEXT PRIMARY KEY,
            -- Position in iCloud's date-ordered list when last fetched; shifts
            -- as the library changes.
            rank INTEGER,
            master_id TEXT NOT NULL,
            change_tag TEXT,
            item_type TEXT,
            filename TEXT,
            asset_date INTEGER,
            added_date INTEGER,
            favorite INTEGER NOT NULL DEFAULT 0,
            duration REAL,
            orientation INTEGER,
            subtype INTEGER,
            has_location INTEGER NOT NULL DEFAULT 0,
            is_video INTEGER NOT NULL DEFAULT 0,
            is_live INTEGER NOT NULL DEFAULT 0,
            -- The local thumbnail, relative to the library directory, and
            -- what the grid needs to draw it before it has decoded it.
            thumb_path TEXT,
            thumb_w INTEGER,
            thumb_h INTEGER,
            avg_color INTEGER
        );
        CREATE INDEX IF NOT EXISTS assets_by_date ON assets(asset_date);
        CREATE TABLE IF NOT EXISTS resources (
            asset_id TEXT NOT NULL,
            kind TEXT NOT NULL,
            width INTEGER,
            height INTEGER,
            size INTEGER,
            file_type TEXT,
            checksum TEXT,
            url TEXT,
            url_fetched_at INTEGER,
            PRIMARY KEY (asset_id, kind)
        ) WITHOUT ROWID;
        CREATE TABLE IF NOT EXISTS raw_records (
            asset_id TEXT PRIMARY KEY,
            asset_json TEXT NOT NULL,
            master_json TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);",
    )?;
    // Indexes made before the thumbnail columns existed get them added.
    for column in ["thumb_path TEXT", "thumb_w INTEGER", "thumb_h INTEGER", "avg_color INTEGER"] {
        match db.execute(&format!("ALTER TABLE assets ADD COLUMN {column}"), []) {
            Ok(_) => {}
            Err(e) if e.to_string().contains("duplicate column") => {}
            Err(e) => return Err(e.into()),
        }
    }
    Ok(db)
}

pub fn set_meta(db: &Connection, key: &str, value: &str) -> Result<()> {
    db.execute("INSERT OR REPLACE INTO meta(key, value) VALUES (?1, ?2)", params![key, value])?;
    Ok(())
}

pub fn meta(db: &Connection, key: &str) -> Option<String> {
    db.query_row("SELECT value FROM meta WHERE key = ?1", [key], |r| r.get(0)).ok()
}

/// Store a page of assets. `first_rank` is the list position of the first
/// one, or None when they did not come from the list (the change feed).
pub fn upsert_assets(db: &mut Connection, assets: &[Asset], first_rank: Option<u64>, fetched_at: i64) -> Result<()> {
    let tx = db.transaction()?;
    {
        // An update in place, not a replace: the thumbnail columns belong to
        // the downloader and survive a re-walk.
        let mut asset_stmt = tx.prepare_cached(
            "INSERT INTO assets
             (id, rank, master_id, change_tag, item_type, filename, asset_date, added_date, favorite,
              duration, orientation, subtype, has_location, is_video, is_live)
             VALUES (?1, ?15, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)
             ON CONFLICT(id) DO UPDATE SET
               rank = coalesce(excluded.rank, rank), master_id = excluded.master_id,
               change_tag = excluded.change_tag, item_type = excluded.item_type, filename = excluded.filename,
               asset_date = excluded.asset_date, added_date = excluded.added_date, favorite = excluded.favorite,
               duration = excluded.duration, orientation = excluded.orientation, subtype = excluded.subtype,
               has_location = excluded.has_location, is_video = excluded.is_video, is_live = excluded.is_live",
        )?;
        // An edit gives the photo a new thumbnail: forget the old one.
        let mut stale_thumb = tx.prepare_cached(
            "UPDATE assets SET thumb_path = NULL WHERE id = ?1 AND EXISTS
             (SELECT 1 FROM resources WHERE asset_id = ?1 AND kind = 'JPEGThumb' AND checksum IS NOT ?2)",
        )?;
        let mut res_stmt = tx.prepare_cached(
            "INSERT OR REPLACE INTO resources
             (asset_id, kind, width, height, size, file_type, checksum, url, url_fetched_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        )?;
        let mut raw_stmt = tx.prepare_cached(
            "INSERT OR REPLACE INTO raw_records (asset_id, asset_json, master_json) VALUES (?1, ?2, ?3)",
        )?;
        for (i, a) in assets.iter().enumerate() {
            let rank = first_rank.map(|r| (r + i as u64) as i64);
            asset_stmt.execute(params![
                a.id, a.master_id, a.change_tag, a.item_type, a.filename, a.asset_date, a.added_date,
                a.favorite, a.duration, a.orientation, a.subtype, a.has_location, a.is_video(), a.is_live(),
                rank,
            ])?;
            for r in &a.resources {
                if r.kind == "JPEGThumb" {
                    stale_thumb.execute(params![a.id, r.checksum])?;
                }
                res_stmt.execute(params![
                    a.id, r.kind, r.width, r.height, r.size, r.file_type, r.checksum, r.url, fetched_at,
                ])?;
            }
            if let Some((asset, master)) = &a.raw {
                raw_stmt.execute(params![a.id, asset.to_string(), master.to_string()])?;
            }
        }
    }
    tx.commit()?;
    Ok(())
}

pub fn remove_assets(db: &mut Connection, ids: &[String]) -> Result<()> {
    let tx = db.transaction()?;
    for id in ids {
        tx.execute("DELETE FROM assets WHERE id = ?1", [id])?;
        tx.execute("DELETE FROM resources WHERE asset_id = ?1", [id])?;
        tx.execute("DELETE FROM raw_records WHERE asset_id = ?1", [id])?;
    }
    tx.commit()?;
    Ok(())
}
