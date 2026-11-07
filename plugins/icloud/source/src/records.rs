//! iCloud's records: a CPLAsset (one per photo in the library) and the
//! CPLMaster it points to (the original file), paired into one asset with
//! its renditions.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr};

use anyhow::Result;
use base64::Engine;
use serde_json::Value;

use crate::cloudkit::PhotosClient;
use crate::index::CANDIDATES;
use pinhole_core::source::{Asset, Effect, Kind, Location, Rendition, Want};

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
pub struct CloudAsset {
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
    pub location: Option<Location>,
    /// `adjustmentRenderType`: flags of what the edit is (`effect`).
    pub render_type: i64,
    /// Its turn and crop as Photos' recipe says them (edit::from_record).
    pub edit: Option<String>,
    /// In Recently Deleted, hidden, or erased: not shown in the library.
    pub gone: bool,
    pub resources: Vec<Resource>,
    /// The two records as iCloud sent them, for inspecting fields during
    /// the probe.
    pub raw: Option<(Value, Value)>,
}

impl CloudAsset {
    /// Its subtype as the index's `subtype` says it: iCloud's 3 is a
    /// screenshot (an iPhone's, an iPad's).
    fn subtype_name(&self) -> Option<String> {
        (self.subtype == Some(3)).then(|| "screenshot".to_owned())
    }

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

    /// A Live Photo's effect, from the edit's flags as icloud.com reads
    /// them: 1 loop, 2 portrait, 4 long exposure, 8 bounce ("mirror").
    pub fn effect(&self) -> Option<Effect> {
        if !self.is_live() || self.is_video() {
            return None;
        }
        [(1, Effect::Loop), (8, Effect::Bounce), (4, Effect::LongExposure)]
            .into_iter()
            .find(|(bit, _)| self.render_type & bit != 0)
            .map(|(_, e)| e)
    }

    pub fn resource(&self, kind: &str) -> Option<&Resource> {
        self.resources.iter().find(|r| r.kind == kind)
    }

    /// As every source describes an asset, with the rendition that serves
    /// each kind a window asks for (`index::CANDIDATES`).
    pub fn neutral(&self) -> Asset {
        let video = self.is_video();
        let effect = self.effect();
        let rendition = |want: Want, r: &Resource| Rendition {
            want,
            name: r.kind.clone(),
            key: r.checksum.clone().unwrap_or_else(|| format!("{}-{}", self.id, r.kind)),
            width: r.width,
            height: r.height,
            size: r.size,
            file_type: r.file_type.clone(),
        };
        let mut renditions: Vec<Rendition> = CANDIDATES
            .iter()
            .filter(|(want, _)| *want != Want::Video || video)
            .filter_map(|(want, names)| Some(rendition(*want, names.iter().find_map(|n| self.resource(n))?)))
            .collect();
        // The effect as the iPhone rendered it: the motion looping or going
        // back and forth (VidCompl, the edited motion), or the long
        // exposure (the edited photo itself).
        let rendered = match effect {
            Some(Effect::Loop | Effect::Bounce) => self.resource("VidCompl"),
            Some(Effect::LongExposure) => self.resource("JPEGFull"),
            None => None,
        };
        if let (Some(e), Some(r)) = (effect, rendered) {
            renditions.push(rendition(e.want(), r));
        }
        Asset {
            id: self.id.clone(),
            filename: self.filename.clone(),
            item_type: self.item_type.clone(),
            taken: self.asset_date,
            added: self.added_date,
            kind: if video { Kind::Video } else if self.is_live() { Kind::Live } else { Kind::Photo },
            favorite: self.favorite,
            duration: self.duration,
            orientation: self.orientation,
            has_location: self.has_location,
            location: self.location,
            size: self.resource("Original").and_then(|r| r.size),
            effect,
            renditions,
            edit: self.edit.clone(),
            subtype: self.subtype_name(),
        }
    }
}

pub struct Page {
    pub assets: Vec<CloudAsset>,
    pub sync_token: Option<String>,
    /// Records iCloud returned, before pairing; tells a short page from the
    /// end of the library.
    pub records: usize,
    /// Records whose other half was not on this page. A page holds 200
    /// records, and an asset and its master can fall on either side of the
    /// boundary; `walk::walk` pairs these up afterwards.
    pub leftovers: Vec<Value>,
}

const RESOURCE_KINDS: &[&str] = &[
    "Original", "OriginalAlt", "OriginalVidCompl", "VidCompl",
    "JPEGFull", "JPEGLarge", "JPEGMed", "JPEGThumb",
    "VidFull", "VidMed", "VidSmall",
];

/// Reads the library list a page at a time.
pub struct Pager {
    pub client: PhotosClient,
    pub keep_raw: bool,
}

impl Pager {
    /// Assets from `start` on, oldest first, at most `limit` of them, plus
    /// the change token iCloud reported with the page.
    pub fn page(&mut self, start: u64, limit: u32) -> Result<Page> {
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
pub fn pair_records(records: &[Value], keep_raw: bool) -> Vec<CloudAsset> {
    pair_records_with_leftovers(records, keep_raw).0
}

/// Like `pair_records`, also returning the records left without a partner.
/// Pairing goes from each asset to its master, in the order the assets
/// came: several assets can share one master.
pub fn pair_records_with_leftovers(records: &[Value], keep_raw: bool) -> (Vec<CloudAsset>, Vec<Value>) {
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

fn build_asset(asset: &Value, master: &Value, keep_raw: bool) -> CloudAsset {
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

    // Coordinates belong to the asset (including edits), not its original.
    let location = af["locationEnc"]["value"].as_str().and_then(plist_location)
        .or_else(|| Location::new(af["locationLatitude"]["value"].as_f64()?,
                                  af["locationLongitude"]["value"].as_f64()?));

    CloudAsset {
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
        has_location: location.is_some() || !af["locationEnc"].is_null() || !af["locationV2Enc"].is_null(),
        location,
        render_type: af["adjustmentRenderType"]["value"].as_i64().unwrap_or(0),
        edit: crate::edit::from_record(af, mf),
        gone: ["isDeleted", "isHidden", "isExpunged"].iter().any(|f| af[*f]["value"].as_i64() == Some(1)),
        resources,
        raw: keep_raw.then(|| (asset.clone(), master.clone())),
    }
}

type Plist = *mut c_void;
#[link(name = "plist-2.0")]
extern "C" {
    fn plist_from_bin(data: *const c_char, len: u32, root: *mut Plist) -> c_int;
    fn plist_free(root: Plist);
    fn plist_dict_get_item(root: Plist, key: *const c_char) -> Plist;
    fn plist_get_node_type(node: Plist) -> c_int;
    fn plist_get_int_val(node: Plist, out: *mut i64);
    fn plist_get_real_val(node: Plist, out: *mut f64);
}

// CloudKit's locationEnc is a base64 binary property list, not EXIF.
fn plist_location(encoded: &str) -> Option<Location> {
    if encoded.len() > 4096 {
        return None;
    }
    let bytes = base64::engine::general_purpose::STANDARD.decode(encoded).ok()?;
    if !bytes.starts_with(b"bplist00") {
        return None;
    }
    struct Root(Plist);
    impl Drop for Root {
        fn drop(&mut self) { unsafe { plist_free(self.0) } }
    }
    let mut root = Root(std::ptr::null_mut());
    // The root owns all the nodes returned by the lookup.
    unsafe {
        if plist_from_bin(bytes.as_ptr().cast(), bytes.len() as u32, &mut root.0) != 0 || root.0.is_null() {
            return None;
        }
        let number = |key: &CStr| {
            let node = plist_dict_get_item(root.0, key.as_ptr());
            if node.is_null() {
                return None;
            }
            match plist_get_node_type(node) {
                1 => { // PLIST_INT
                    let mut value = 0;
                    plist_get_int_val(node, &mut value);
                    Some(value as f64)
                }
                2 => { // PLIST_REAL
                    let mut value = 0.0;
                    plist_get_real_val(node, &mut value);
                    Some(value)
                }
                _ => None,
            }
        };
        Location::new(number(c"lat")?, number(c"lon")?)
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

    #[test]
    fn location_is_decoded_from_cloudkit_binary_plist() {
        let encoded = "YnBsaXN0MDDTAQIDBAUGU2FsdFNsYXRTbG9uECAjwEDuFHrhR64jQGLmuFHrhR8IDxMXGx0mAAAAAAAAAQEAAAAAAAAABwAAAAAAAAAAAAAAAAAAAC8=";
        assert_eq!(plist_location(encoded).map(Location::degrees), Some((-33.86, 151.21)));
        assert!(plist_location("YnBsaXN0MDA=").is_none());
    }

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

    #[test]
    fn a_live_photo_has_the_effect_set_on_the_iphone_with_its_render() {
        let res = |checksum: &str| json!({"value": {"fileChecksum": checksum, "size": 10}});
        let mut bounce = asset("a1", "m1");
        bounce["fields"]["adjustmentRenderType"] = json!({"value": 16392}); // 0x4008: bounce
        bounce["fields"]["resVidComplRes"] = res("rendered");
        bounce["fields"]["resJPEGFullRes"] = res("still");
        let mut long = asset("a2", "m2");
        long["fields"]["adjustmentRenderType"] = json!({"value": 16390}); // portrait and long exposure
        long["fields"]["resJPEGFullRes"] = res("blurred");
        let mut portrait = asset("a3", "m3");
        portrait["fields"]["adjustmentRenderType"] = json!({"value": 26626});
        let live = |name: &str| {
            let mut m = master(name);
            m["fields"]["itemType"] = json!({"value": "public.heic"});
            m["fields"]["resOriginalVidComplRes"] = res(&format!("{name}-motion"));
            m
        };
        let page = [live("m1"), bounce, live("m2"), long, live("m3"), portrait];
        let assets: Vec<Asset> = pair_records(&page, false).iter().map(CloudAsset::neutral).collect();
        assert_eq!(assets.iter().map(|a| a.effect).collect::<Vec<_>>(), [Some(Effect::Bounce), Some(Effect::LongExposure), None]);
        assert_eq!(assets[0].rendition(Want::Bounce).map(|r| r.key.as_str()), Some("rendered"));
        assert_eq!(assets[0].rendition(Want::Live).map(|r| r.key.as_str()), Some("m1-motion"));
        assert_eq!(assets[1].rendition(Want::LongExposure).map(|r| r.key.as_str()), Some("blurred"));
        assert!(assets[2].rendition(Want::Loop).is_none());
    }
}
