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
    /// The CPLAsset record name: one per photo in the library.
    pub id: String,
    /// The CPLMaster record name: the original file. A duplicated photo is
    /// a second asset on the same master, so masters can be shared.
    pub master_id: String,
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
    /// Records whose other half was not on this page. A page holds 200
    /// records, and an asset and its master can fall on either side of the
    /// boundary; `sync::walk` pairs these up afterwards.
    pub leftovers: Vec<Value>,
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
        let (assets, leftovers) = pair_records_with_leftovers(&records, self.keep_raw);
        Ok(Page {
            records: records.len(),
            assets,
            leftovers,
            sync_token: response["syncToken"].as_str().map(str::to_owned),
        })
    }
}

/// Turn CPLAsset + CPLMaster records into assets, in the order the masters
/// came. An asset without its master (or the other way round) is dropped.
pub fn pair_records(records: &[Value], keep_raw: bool) -> Vec<Asset> {
    pair_records_with_leftovers(records, keep_raw).0
}

/// Like `pair_records`, also returning the records left without a partner.
/// Pairing goes from each asset to its master, in the order the assets
/// came: several assets can share one master.
pub fn pair_records_with_leftovers(records: &[Value], keep_raw: bool) -> (Vec<Asset>, Vec<Value>) {
    let mut masters: HashMap<&str, (&Value, bool)> = HashMap::new();
    for r in records.iter().filter(|r| r["recordType"] == "CPLMaster") {
        if let Some(name) = r["recordName"].as_str() {
            masters.insert(name, (r, false));
        }
    }
    let mut assets = Vec::new();
    let mut leftovers = Vec::new();
    for asset in records.iter().filter(|r| r["recordType"] == "CPLAsset") {
        let master = asset["fields"]["masterRef"]["value"]["recordName"].as_str().and_then(|m| masters.get_mut(m));
        match master {
            Some((master, used)) => {
                *used = true;
                assets.push(build_asset(asset, master, keep_raw));
            }
            None => leftovers.push(asset.clone()),
        }
    }
    leftovers.extend(masters.into_values().filter(|(_, used)| !used).map(|(m, _)| m.clone()));
    (assets, leftovers)
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
        id: asset["recordName"].as_str().unwrap_or_default().to_owned(),
        master_id: master["recordName"].as_str().unwrap_or_default().to_owned(),
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

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn asset(name: &str, master: &str) -> Value {
        json!({"recordName": name, "recordType": "CPLAsset",
               "fields": {"masterRef": {"value": {"recordName": master}}, "assetDate": {"value": 1}}})
    }

    fn master(name: &str) -> Value {
        json!({"recordName": name, "recordType": "CPLMaster", "fields": {"itemType": {"value": "public.jpeg"}}})
    }

    #[test]
    fn halves_split_across_pages_pair_up_later() {
        // Page one ends with an asset whose master starts page two.
        let page1 = [master("m1"), asset("a1", "m1"), asset("a2", "m2")];
        let page2 = [master("m2"), master("m3"), asset("a3", "m3")];
        let (assets1, left1) = pair_records_with_leftovers(&page1, false);
        let (assets2, left2) = pair_records_with_leftovers(&page2, false);
        assert_eq!(assets1.iter().map(|a| a.id.as_str()).collect::<Vec<_>>(), ["a1"]);
        assert_eq!(assets2.iter().map(|a| a.id.as_str()).collect::<Vec<_>>(), ["a3"]);
        let leftovers: Vec<Value> = left1.into_iter().chain(left2).collect();
        let (repaired, rest) = pair_records_with_leftovers(&leftovers, false);
        assert_eq!(repaired.len(), 1);
        assert_eq!(repaired[0].id, "a2");
        assert_eq!(repaired[0].master_id, "m2");
        assert!(rest.is_empty());
    }

    #[test]
    fn duplicates_sharing_a_master_are_both_kept() {
        let page = [master("m1"), asset("a1", "m1"), asset("a1-copy", "m1")];
        let (assets, leftovers) = pair_records_with_leftovers(&page, false);
        assert_eq!(assets.iter().map(|a| a.id.as_str()).collect::<Vec<_>>(), ["a1", "a1-copy"]);
        assert!(assets.iter().all(|a| a.master_id == "m1"));
        assert!(leftovers.is_empty());
    }
}
