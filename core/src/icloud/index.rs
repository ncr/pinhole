//! What iCloud keeps in the index beside what every source has: each
//! asset's master and change tag, its renditions under CloudKit's names
//! with their checksums and download links, and, for pinhole-probe, the
//! records as iCloud sent them.

use anyhow::Result;
use rusqlite::{params, Connection};

use crate::db::Index;
use crate::icloud::records::CloudAsset;
use crate::source::{Asset, Want};

/// Which of iCloud's renditions serves each kind a window asks for: the
/// first of these the asset has. `full` is the photo as it looks now: the
/// edit when it has been edited (the asset's own renditions), else the
/// original. `video` is for videos only.
pub const CANDIDATES: &[(Want, &[&str])] = &[
    (Want::Thumb, &["JPEGThumb"]),
    (Want::Medium, &["JPEGMed", "JPEGFull", "JPEGLarge"]),
    (Want::Full, &["JPEGFull", "VidFull", "Original"]),
    (Want::Original, &["Original"]),
    (Want::Video, &["VidMed", "VidFull", "Original"]),
    (Want::Live, &["OriginalVidCompl"]),
];

pub fn create(db: &Connection) -> Result<()> {
    db.execute_batch(
        "CREATE TABLE IF NOT EXISTS icloud_assets (
            asset_id TEXT PRIMARY KEY,
            -- The CPLMaster record: the original file. A duplicated photo is
            -- a second asset on the same master.
            master_id TEXT NOT NULL,
            change_tag TEXT,
            -- Position in iCloud's date-ordered list when last fetched;
            -- shifts as the library changes.
            rank INTEGER,
            subtype INTEGER
        );
        CREATE INDEX IF NOT EXISTS icloud_assets_by_master ON icloud_assets(master_id);
        CREATE TABLE IF NOT EXISTS icloud_resources (
            asset_id TEXT NOT NULL,
            -- Original, JPEGThumb, VidMed...
            kind TEXT NOT NULL,
            width INTEGER,
            height INTEGER,
            size INTEGER,
            file_type TEXT,
            checksum TEXT,
            -- Signed and short-lived: looked up again after 20 minutes.
            url TEXT,
            url_fetched_at INTEGER,
            PRIMARY KEY (asset_id, kind)
        ) WITHOUT ROWID;
        CREATE TABLE IF NOT EXISTS icloud_records (
            asset_id TEXT PRIMARY KEY,
            asset_json TEXT NOT NULL,
            master_json TEXT NOT NULL
        );
        CREATE TRIGGER IF NOT EXISTS icloud_forget AFTER DELETE ON assets BEGIN
            DELETE FROM icloud_assets WHERE asset_id = old.id;
            DELETE FROM icloud_resources WHERE asset_id = old.id;
            DELETE FROM icloud_records WHERE asset_id = old.id;
        END;",
    )?;
    Ok(())
}

/// Store assets as iCloud describes them: the neutral part through the
/// index, and iCloud's own beside it. `first_rank` is the list position of
/// the first one, or None when they did not come from the list (the change
/// feed).
pub fn store(index: &mut Index, assets: &[CloudAsset], first_rank: Option<u64>, fetched_at: i64) -> Result<()> {
    let neutral: Vec<Asset> = assets.iter().map(CloudAsset::neutral).collect();
    index.put(&neutral)?;
    create(index.conn)?;
    let tx = index.conn.transaction()?;
    {
        let mut asset_stmt = tx.prepare_cached(
            "INSERT INTO icloud_assets (asset_id, master_id, change_tag, rank, subtype) VALUES (?1, ?2, ?3, ?4, ?5)
             ON CONFLICT(asset_id) DO UPDATE SET master_id = excluded.master_id, change_tag = excluded.change_tag,
               rank = coalesce(excluded.rank, rank), subtype = excluded.subtype",
        )?;
        let mut res_stmt = tx.prepare_cached(
            "INSERT OR REPLACE INTO icloud_resources
             (asset_id, kind, width, height, size, file_type, checksum, url, url_fetched_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        )?;
        let mut raw_stmt = tx.prepare_cached(
            "INSERT OR REPLACE INTO icloud_records (asset_id, asset_json, master_json) VALUES (?1, ?2, ?3)",
        )?;
        for (i, a) in assets.iter().enumerate() {
            let rank = first_rank.map(|r| (r + i as u64) as i64);
            asset_stmt.execute(params![a.id, a.master_id, a.change_tag, rank, a.subtype])?;
            for r in &a.resources {
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
