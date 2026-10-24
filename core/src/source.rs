//! What the rest of Pinhole asks for photos. iCloud is one implementation;
//! a synthetic library for tests and stress runs is the other one to come.

use std::collections::HashMap;

use anyhow::Result;
use base64::Engine;
use serde_json::Value;

use crate::cloudkit::{PhotosClient, LIBRARY_COUNT};

/// One rendition of an asset iCloud keeps: the original, the JPEG sizes
/// and the video sizes. Download URLs are signed and short-lived, so they
/// are fetched fresh when needed rather than relied on later.
#[derive(Debug, Clone)]
pub struct Resource {
    /// The CloudKit field prefix without "res": Original, JPEGThumb, VidMed...
    pub kind: String,
    pub width: Option<i64>,
    pub height: Option<i64>,
    pub size: Option<i64>,
    pub file_type: Option<String>,
    pub checksum: Option<String>,
    pub url: Option<String>,
}

#[derive(Debug, Clone)]
pub struct Asset {
    /// The CPLMaster record name: stable for the life of the photo.
    pub id: String,
    pub asset_record: String,
    pub change_tag: Option<String>,
    pub item_type: Option<String>,
    pub filename: Option<String>,
    /// Capture time and upload time, milliseconds since the epoch.
    pub asset_date: Option<i64>,
    pub added_date: Option<i64>,
    pub favorite: bool,
    pub duration: Option<f64>,
    pub orientation: Option<i64>,
    pub subtype: Option<i64>,
    pub has_location: bool,
    pub resources: Vec<Resource>,
    /// The two records as iCloud sent them, for inspecting fields during
    /// the probe.
    pub raw: Option<(Value, Value)>,
}

impl Asset {
    pub fn is_video(&self) -> bool {
        match self.item_type.as_deref() {
            Some(t) => t.contains("movie") || t.contains("mpeg") || t.contains("video"),
            // A Live Photo has a video half too, but it is a photo.
            None => !self.is_live() && self.resources.iter().any(|r| r.kind.starts_with("Vid")),
        }
    }

    pub fn is_live(&self) -> bool {
        self.resources.iter().any(|r| r.kind == "OriginalVidCompl")
    }

    pub fn resource(&self, kind: &str) -> Option<&Resource> {
        self.resources.iter().find(|r| r.kind == kind)
    }
}

pub trait PhotoSource {
    /// Number of assets in the library.
    fn count(&mut self) -> Result<u64>;
    /// Assets from `start` on, oldest first, at most `limit` of them, plus
    /// the change token iCloud reported with the page.
    fn page(&mut self, start: u64, limit: u32) -> Result<Page>;
}

pub struct Page {
    pub assets: Vec<Asset>,
    pub sync_token: Option<String>,
    /// Records iCloud returned, before pairing; tells a short page from the
    /// end of the library.
    pub records: usize,
}

const RESOURCE_KINDS: &[&str] = &[
    "Original", "OriginalAlt", "OriginalVidCompl",
    "JPEGFull", "JPEGLarge", "JPEGMed", "JPEGThumb",
    "VidFull", "VidMed", "VidSmall",
];

pub struct ICloudSource {
    pub client: PhotosClient,
    pub keep_raw: bool,
}

impl PhotoSource for ICloudSource {
    fn count(&mut self) -> Result<u64> {
        self.client.count(LIBRARY_COUNT)
    }

    fn page(&mut self, start: u64, limit: u32) -> Result<Page> {
        let response = self.client.list_page(start, limit * 2)?;
        let records = response["records"].as_array().cloned().unwrap_or_default();
        Ok(Page {
            records: records.len(),
            assets: pair_records(&records, self.keep_raw),
            sync_token: response["syncToken"].as_str().map(str::to_owned),
        })
    }
}

/// Turn CPLAsset + CPLMaster records into assets, in the order the masters
/// came. An asset without its master (or the other way round) is dropped.
pub fn pair_records(records: &[Value], keep_raw: bool) -> Vec<Asset> {
    let mut by_master: HashMap<&str, &Value> = HashMap::new();
    for r in records.iter().filter(|r| r["recordType"] == "CPLAsset") {
        if let Some(master) = r["fields"]["masterRef"]["value"]["recordName"].as_str() {
            by_master.insert(master, r);
        }
    }
    records
        .iter()
        .filter(|r| r["recordType"] == "CPLMaster")
        .filter_map(|master| {
            let name = master["recordName"].as_str()?;
            let asset = by_master.get(name)?;
            Some(build_asset(asset, master, keep_raw))
        })
        .collect()
}

fn build_asset(asset: &Value, master: &Value, keep_raw: bool) -> Asset {
    let af = &asset["fields"];
    let mf = &master["fields"];
    // An edited photo keeps its renditions on the asset record; the master
    // holds the original. The asset's copy wins, as on icloud.com.
    let field = |name: &str| -> Option<&Value> {
        af.get(name).filter(|v| !v.is_null()).or_else(|| mf.get(name).filter(|v| !v.is_null()))
    };
    let value = |name: &str| field(name).map(|f| &f["value"]);

    let resources = RESOURCE_KINDS
        .iter()
        .filter_map(|kind| {
            let res = value(&format!("res{kind}Res"))?;
            Some(Resource {
                kind: (*kind).to_owned(),
                width: value(&format!("res{kind}Width")).and_then(Value::as_i64),
                height: value(&format!("res{kind}Height")).and_then(Value::as_i64),
                size: res["size"].as_i64(),
                file_type: value(&format!("res{kind}FileType")).and_then(Value::as_str).map(str::to_owned),
                checksum: res["fileChecksum"].as_str().map(str::to_owned),
                url: res["downloadURL"].as_str().map(str::to_owned),
            })
        })
        .collect();

    Asset {
        id: master["recordName"].as_str().unwrap_or_default().to_owned(),
        asset_record: asset["recordName"].as_str().unwrap_or_default().to_owned(),
        change_tag: asset["recordChangeTag"].as_str().map(str::to_owned),
        item_type: mf["itemType"]["value"].as_str().map(str::to_owned),
        filename: decode_filename(&mf["filenameEnc"]),
        asset_date: af["assetDate"]["value"].as_i64(),
        added_date: af["addedDate"]["value"].as_i64(),
        favorite: af["isFavorite"]["value"].as_i64() == Some(1),
        // iCloud keeps durations in milliseconds.
        duration: af["duration"]["value"].as_f64().or_else(|| mf["duration"]["value"].as_f64()).map(|ms| ms / 1000.0),
        orientation: af["orientation"]["value"].as_i64().or_else(|| mf["originalOrientation"]["value"].as_i64()),
        subtype: af["assetSubtypeV2"]["value"].as_i64().or_else(|| af["assetSubtype"]["value"].as_i64()),
        has_location: !af["locationEnc"].is_null() || !af["locationV2Enc"].is_null(),
        resources,
        raw: keep_raw.then(|| (asset.clone(), master.clone())),
    }
}

fn decode_filename(field: &Value) -> Option<String> {
    let raw = field["value"].as_str()?;
    match field["type"].as_str() {
        Some("STRING") => Some(raw.to_owned()),
        _ => base64::engine::general_purpose::STANDARD
            .decode(raw)
            .ok()
            .and_then(|b| String::from_utf8(b).ok()),
    }
}
