//! A photo turned and cropped from Pinhole, written to iCloud as Photos on
//! an iPhone writes its own edit (docs/api-findings.md, Edits): the
//! original (CPLMaster) left as it is, and on the asset (CPLAsset) Apple's
//! recipe (`com.apple.photo`: a `Crop`, the orientation in its metadata)
//! with the photo as edited, rendered: full size, medium and thumbnail. An
//! iPhone can then open the edit, change it, or revert to the original.
//! Taken back (`revert`), the asset has none of it again.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{anyhow, Context, Result};
use base64::Engine;
use serde_json::{json, Value};

use crate::cloudkit::PhotosClient;

/// What the person did: quarter turns clockwise, and the part kept as
/// fractions of the photo as turned (x, y, width, height).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Adjustment {
    pub turn: u8,
    pub crop: [f64; 4],
}

impl Adjustment {
    pub fn is_identity(&self) -> bool {
        self.turn % 4 == 0 && self.crop == [0.0, 0.0, 1.0, 1.0]
    }
}

/// The photo upright as it was taken (its own orientation applied), in
/// pixels; what the edit is of.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Upright {
    pub width: u32,
    pub height: u32,
}

/// The part kept, in the upright photo's pixels turned as the edit turns
/// it: x, y, width, height.
pub fn crop_pixels(upright: Upright, adjustment: &Adjustment) -> (u32, u32, u32, u32) {
    let (w, h) = if adjustment.turn % 2 == 1 { (upright.height, upright.width) } else { (upright.width, upright.height) };
    let [x, y, cw, ch] = adjustment.crop;
    let px = (x * w as f64).floor().clamp(0.0, w as f64 - 1.0) as u32;
    let py = (y * h as f64).floor().clamp(0.0, h as f64 - 1.0) as u32;
    let pw = ((cw * w as f64).round() as u32).clamp(1, w - px);
    let ph = ((ch * h as f64).round() as u32).clamp(1, h - py);
    (px, py, pw, ph)
}

/// A size of the same shape holding at most `area` pixels: Photos'
/// medium is 2048x1536's, its thumbnail 480x360's, whatever the shape.
pub fn fit_area(width: u32, height: u32, area: u32) -> (u32, u32) {
    let pixels = width as f64 * height as f64;
    if pixels <= area as f64 {
        return (width, height);
    }
    let scale = (area as f64 / pixels).sqrt();
    (((width as f64 * scale).round() as u32).max(1), ((height as f64 * scale).round() as u32).max(1))
}

pub const MEDIUM_AREA: u32 = 2048 * 1536;
pub const THUMB_AREA: u32 = 480 * 360;

/// One rendered file, and its size.
#[derive(Debug, Clone)]
pub struct Render {
    pub path: PathBuf,
    pub width: u32,
    pub height: u32,
}

/// The photo as edited, from its original: full size, medium, thumbnail,
/// JPEGs at quality 92 with its colour profile, in `dir`: a private folder
/// the caller made for this edit alone, never one made here. ImageMagick
/// reads the HEIC upright (`-auto-orient`), then turns and crops it.
pub fn render(original: &Path, upright: Upright, adjustment: &Adjustment, dir: &Path) -> Result<[Render; 3]> {
    if !std::fs::symlink_metadata(dir).is_ok_and(|m| m.is_dir()) {
        return Err(anyhow!("no folder {} to render the edit in", dir.display()));
    }
    let (x, y, w, h) = crop_pixels(upright, adjustment);
    let full = dir.join("full.jpg");
    let status = Command::new("magick")
        .arg(format!("{}[0]", original.display()))
        .args(["-auto-orient", "-rotate", &(90 * (adjustment.turn % 4) as u32).to_string()])
        .args(["-crop", &format!("{w}x{h}+{x}+{y}"), "+repage", "-quality", "92"])
        .arg(&full)
        .status()
        .context("running ImageMagick")?;
    if !status.success() {
        return Err(anyhow!("ImageMagick could not render the edit ({status})"));
    }
    let mut renders = vec![Render { path: full.clone(), width: w, height: h }];
    for (name, area) in [("medium.jpg", MEDIUM_AREA), ("thumb.jpg", THUMB_AREA)] {
        let (rw, rh) = fit_area(w, h, area);
        let out = dir.join(name);
        let status = Command::new("magick")
            .arg(&full)
            .args(["-resize", &format!("{rw}x{rh}!"), "-quality", "92"])
            .arg(&out)
            .status()?;
        if !status.success() {
            return Err(anyhow!("ImageMagick could not make the {name}"));
        }
        renders.push(Render { path: out, width: rw, height: rh });
    }
    Ok(renders.try_into().map_err(|_| anyhow!("three renders"))?)
}

/// The part kept as Photos' `Crop` measures it: in the upright photo's
/// pixels before the person's turn, its origin at the bottom left (Core
/// Image's), as an iPhone wrote it for a photo turned a quarter right and
/// cropped (docs/api-findings.md, Edits). x, y from the bottom, width,
/// height.
pub fn crop_in_master(upright: Upright, adjustment: &Adjustment) -> (u32, u32, u32, u32) {
    // Back from the turned frame to the upright one, a quarter
    // anticlockwise at a time: what is at the top went to the left.
    let [mut x, mut y, mut w, mut h] = adjustment.crop;
    for _ in 0..adjustment.turn % 4 {
        (x, y, w, h) = (y, 1.0 - x - w, h, w);
    }
    let (uw, uh) = (upright.width as f64, upright.height as f64);
    let px = (x * uw).round().clamp(0.0, uw - 1.0) as u32;
    let top = (y * uh).round().clamp(0.0, uh - 1.0) as u32;
    let pw = ((w * uw).round() as u32).clamp(1, upright.width - px);
    let ph = ((h * uh).round() as u32).clamp(1, upright.height - top);
    (px, upright.height - top - ph, pw, ph)
}

/// Apple's recipe for it, deflated, as an iPhone's
/// `adjustmentSimpleDataEnc` is: the `Crop` in the upright photo's pixels
/// (crop_in_master), the turn as an EXIF orientation in its metadata (0
/// for none, as Photos writes), the shape kept as the crop's own turned.
pub fn recipe(upright: Upright, adjustment: &Adjustment) -> Result<Vec<u8>> {
    deflate(&recipe_json(upright, adjustment, None))
}

/// The recipe as JSON. What the photo's recipe had besides a crop (the
/// camera's `SemanticStyle`) is kept, as Photos keeps it when one crops.
pub fn recipe_json(upright: Upright, adjustment: &Adjustment, previous: Option<&Value>) -> Value {
    let (x, y, w, h) = crop_in_master(upright, adjustment);
    // EXIF's codes for a picture turned 90, 180, 270 degrees clockwise.
    let orientation = [0, 6, 3, 8][(adjustment.turn % 4) as usize];
    let (shape_w, shape_h) = if adjustment.turn % 2 == 1 { (h, w) } else { (w, h) };
    // A photo never edited: the camera's standard style, as Photos on an
    // iPhone puts it first in every crop it writes.
    let mut adjustments: Vec<Value> = previous
        .and_then(|p| p["adjustments"].as_array())
        .map(|a| a.iter().filter(|j| j["identifier"] != "Crop").cloned().collect())
        .unwrap_or_else(|| vec![json!({
            "formatVersion": 1, "enabled": true, "identifier": "SemanticStyle", "formatIdentifier": "com.apple.photo",
            "settings": {"version": 1, "tone": 0, "enabled": true, "cast": "Standard", "intensity": 1, "color": 0},
        })]);
    adjustments.push(json!({
        "formatVersion": 1, "enabled": true, "identifier": "Crop", "formatIdentifier": "com.apple.photo",
        "settings": {"xOrigin": x, "yOrigin": y, "width": w, "height": h,
                     "constraintWidth": shape_w, "constraintHeight": shape_h,
                     "straightenAngle": 0, "pitch": 0, "yaw": 0, "auto": false},
    }));
    let version = previous
        .map(|p| p["versionInfo"].clone())
        .filter(|v| v.is_object())
        // As Photos on iOS 26 wrote Jacek's reference edit: Photos opens
        // to edit again only a recipe it takes for its own.
        .unwrap_or_else(|| json!({"platform": "iOS", "appVersion": "916.57.153", "buildNumber": "24B115", "schemaRevision": 0}));
    json!({
        "metadata": {"masterWidth": upright.width, "masterHeight": upright.height, "orientation": orientation},
        "formatVersion": 1,
        "versionInfo": version,
        "adjustments": adjustments,
    })
}

/// What a recipe besides a crop and a style would need Apple to render (a
/// portrait's blur, a Live Photo's key frame): Pinhole's render would be
/// without it, so such a photo is edited on the iPhone.
pub fn needs_apple(previous: &Value) -> Option<String> {
    previous["adjustments"].as_array().into_iter().flatten()
        .filter_map(|j| j["identifier"].as_str())
        .find(|id| !["Crop", "SemanticStyle"].contains(id))
        .map(str::to_owned)
}

/// A photo's recipe as its asset has it (`adjustmentSimpleDataEnc`), if
/// it is Photos' own.
pub fn previous_recipe(asset: &Value) -> Option<Value> {
    existing_recipe(asset).ok().flatten()
}

/// The edit an asset has now: none for a photo never edited, Photos' own
/// recipe for one Pinhole can carry on from, and an error for any other
/// (another app's, or one that cannot be read), which an edit from here
/// would lose.
pub fn existing_recipe(asset: &Value) -> Result<Option<Value>> {
    let fields = &asset["fields"];
    let has = |name: &str| !fields[name]["value"].is_null();
    if !["adjustmentType", "adjustmentSimpleDataEnc", "resJPEGFullRes"].iter().any(|n| has(n)) {
        return Ok(None);
    }
    let kind = fields["adjustmentType"]["value"].as_str().unwrap_or("");
    if kind != "com.apple.photo" {
        return Err(anyhow!("edited elsewhere ({kind}), which an edit from here would lose: edit it there or on the iPhone"));
    }
    let unreadable = || anyhow!("its edit in Photos cannot be read here, and an edit from here would lose it: edit it on the iPhone");
    let data = fields["adjustmentSimpleDataEnc"]["value"].as_str().ok_or_else(unreadable)?;
    let data = base64::engine::general_purpose::STANDARD.decode(data).map_err(|_| unreadable())?;
    let mut json = String::new();
    std::io::Read::read_to_string(&mut flate2::read::DeflateDecoder::new(&data[..]), &mut json).map_err(|_| unreadable())?;
    let recipe: Value = serde_json::from_str(&json).map_err(|_| unreadable())?;
    if !recipe["adjustments"].is_array() {
        return Err(unreadable());
    }
    Ok(Some(recipe))
}

/// What of an asset its edit is (`fields` writes and `revert_fields` takes
/// off): two looks at it alike are the same edit.
fn edit_of(asset: &Value) -> Value {
    let fields = &asset["fields"];
    let mut edit = serde_json::Map::new();
    for name in EDIT_FIELDS.iter().copied().chain(["orientation"]) {
        edit.insert(name.to_owned(), fields[name]["value"].clone());
    }
    for prefix in RENDITIONS {
        // A rendition's own value holds a link that changes from one look
        // to the next; its fingerprint is the file.
        for part in ["Width", "Height", "FileType", "FileSize", "Fingerprint"] {
            let name = format!("{prefix}{part}");
            edit.insert(name.clone(), fields[&name]["value"].clone());
        }
        edit.insert(format!("{prefix}Res"), fields[format!("{prefix}Res")]["value"]["fileChecksum"].clone());
    }
    Value::Object(edit)
}

/// The asset's fields an edit is written in, besides its renditions.
const EDIT_FIELDS: [&str; 8] = ["adjustmentType", "adjustmentCreatorCode", "adjustmentCompoundVersion", "adjustmentSimpleDataEnc",
                                "adjustmentTimestampEnc", "adjustmentRenderType", "adjustedMediaMetaDataEnc", "adjustedMediaMetaDataStrType"];
/// The edit's renditions: full size, medium, thumbnail.
const RENDITIONS: [&str; 3] = ["resJPEGFull", "resJPEGMed", "resJPEGThumb"];

fn deflate(data: &Value) -> Result<Vec<u8>> {
    let mut deflated = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::default());
    deflated.write_all(data.to_string().as_bytes())?;
    Ok(deflated.finish()?)
}

/// The asset's fields for the edit, the three renders' receipts given
/// (`receipt(field)` uploads a file as that field and answers its value).
pub fn fields(
    renders: &[Render; 3],
    recipe: &[u8],
    styled: bool,
    now_ms: i64,
    mut receipt: impl FnMut(&str, &Path) -> Result<Value>,
) -> Result<Value> {
    let b64 = base64::engine::general_purpose::STANDARD;
    let mut fields = json!({
        "adjustmentType": {"value": "com.apple.photo"},
        // Pinhole's: a device shows the edit and can revert it, but opens
        // no edit made elsewhere again, Photos' own replayed byte for byte
        // included (docs/api-findings.md, Edits).
        "adjustmentCreatorCode": {"value": "net.becela.pinhole"},
        "adjustmentCompoundVersion": {"value": "1.12"},
        "adjustmentSimpleDataEnc": {"value": b64.encode(recipe)},
        "adjustmentTimestampEnc": {"value": b64.encode(timestamp_plist(now_ms))},
        // As an iPhone's crop of a photo with the camera's style carries
        // (0x8200), or without one.
        "adjustmentRenderType": {"value": if styled { 33280 } else { 32768 }},
        // The render is upright: shown as it is, as an iPhone's edit says.
        "orientation": {"value": 1},
    });
    // What the render is, as Image I/O describes a picture
    // (CGImageProperties): an iPhone's edit carries its own.
    let full = &renders[0];
    let properties = json!({
        "PixelWidth": full.width, "PixelHeight": full.height, "Orientation": 1, "Depth": 8,
        "ColorModel": "RGB", "ProfileName": "sRGB IEC61966-2.1", "DPIWidth": 72.0, "DPIHeight": 72.0,
        "PrimaryImage": true,
        "{TIFF}": {"Orientation": 1},
        "{Exif}": {"PixelXDimension": full.width, "PixelYDimension": full.height, "ColorSpace": 1},
    });
    fields["adjustedMediaMetaDataEnc"] = json!({"value": b64.encode(bplist(&properties))});
    fields["adjustedMediaMetaDataStrType"] = json!({"value": "CGImageProperties"});
    for (render, prefix) in renders.iter().zip(["resJPEGFull", "resJPEGMed", "resJPEGThumb"]) {
        let value = receipt(&format!("{prefix}Res"), &render.path)?;
        let size = std::fs::metadata(&render.path)?.len();
        // Its fingerprint is the file's checksum, as an iPhone's are.
        fields[format!("{prefix}Fingerprint")] = json!({"value": value["fileChecksum"]});
        fields[format!("{prefix}Res")] = json!({"value": value});
        fields[format!("{prefix}Width")] = json!({"value": render.width});
        fields[format!("{prefix}Height")] = json!({"value": render.height});
        fields[format!("{prefix}FileType")] = json!({"value": "public.jpeg"});
        fields[format!("{prefix}FileSize")] = json!({"value": size});
    }
    Ok(fields)
}

/// The fields an edit has, each taken off again: the asset as it was
/// before any edit, its original shown.
pub fn revert_fields(original_orientation: i64) -> Value {
    // Shown again as the original is turned.
    let mut fields = json!({"orientation": {"value": original_orientation}});
    for name in EDIT_FIELDS {
        fields[name] = json!({"value": null});
    }
    for prefix in RENDITIONS {
        for part in ["Res", "Width", "Height", "FileType", "FileSize", "Fingerprint"] {
            fields[format!("{prefix}{part}")] = json!({"value": null});
        }
    }
    fields
}

/// The photo upright (its original's orientation applied, as ImageMagick
/// and libheif read it), in pixels.
pub fn upright_of(original: &Path) -> Result<Upright> {
    let out = Command::new("magick")
        .arg(format!("{}[0]", original.display()))
        .args(["-auto-orient", "-format", "%w %h", "info:"])
        .output()
        .context("running ImageMagick")?;
    let text = String::from_utf8_lossy(&out.stdout);
    let mut parts = text.split_whitespace().filter_map(|n| n.parse::<u32>().ok());
    match (parts.next(), parts.next()) {
        (Some(width), Some(height)) if width > 0 && height > 0 => Ok(Upright { width, height }),
        _ => Err(anyhow!("could not read the size of {}", original.display())),
    }
}

/// The edit written to the asset `record` (its change tag as last read):
/// rendered from `original` in `dir`, the three renders sent, the recipe
/// and them put on the asset in one modify. Changed since on a device
/// (another change tag), it is written once more only if its edit is still
/// the one this was made from: an edit made meanwhile elsewhere is never
/// written over. Its new tag.
pub fn write(client: &mut PhotosClient, record: &str, tag: Option<&str>, original: &Path, adjustment: &Adjustment,
             dir: &Path) -> Result<String> {
    // What the photo's recipe has now (the camera's style, an earlier
    // crop), all its fields asked for.
    let asset = look_up_asset(client, record)?;
    // Only an edit Pinhole can carry on from is written over.
    let previous = existing_recipe(&asset)?;
    if let Some(effect) = previous.as_ref().and_then(needs_apple) {
        return Err(anyhow!("edited with {effect}, which only Photos renders: edit it on the iPhone"));
    }
    let tag = asset["recordChangeTag"].as_str().or(tag);
    // The original's own orientation, from its master, to show it so again.
    let master_name = asset["fields"]["masterRef"]["value"]["recordName"].as_str().unwrap_or_default().to_owned();
    let master = if master_name.is_empty() { Value::Null } else { client.lookup_all(&[master_name.as_str()])? };
    let original_orientation = master["records"][0]["fields"]["originalOrientation"]["value"].as_i64().unwrap_or(1);
    let fields = if adjustment.is_identity() {
        revert_fields(original_orientation)
    } else {
        let upright = upright_of(original)?;
        let renders = render(original, upright, adjustment, dir)?;
        // Photos measures the crop in the original's own pixels as stored
        // (a portrait from the camera: landscape, turned by its
        // orientation), and the recipe's orientation is the whole turn from
        // those to what is shown: the original's and the person's.
        let quarters = match original_orientation {
            1 => 0,
            6 => 1,
            3 => 2,
            8 => 3,
            other => return Err(anyhow!("the original is mirrored (orientation {other}): edit it on the iPhone")),
        };
        let raw = if quarters % 2 == 1 { Upright { width: upright.height, height: upright.width } } else { upright };
        let whole = Adjustment { turn: (quarters + adjustment.turn) % 4, crop: adjustment.crop };
        let data = recipe_json(raw, &whole, previous.as_ref());
        let styled = data["adjustments"].as_array().is_some_and(|a| a.iter().any(|j| j["identifier"] == "SemanticStyle"));
        let recipe = deflate(&data)?;
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_millis() as i64);
        fields(&renders, &recipe, styled, now, |field, path| client.upload_asset(record, field, std::fs::read(path)?))?
    };
    let answer = client.modify(&[(record, tag)], &fields)?;
    let first = &answer["records"][0];
    if let Some(tag) = first["recordChangeTag"].as_str().filter(|_| first["serverErrorCode"].is_null()) {
        return Ok(tag.to_owned());
    }
    if first["serverErrorCode"] != "CONFLICT" {
        return Err(anyhow!("iCloud would not take the edit: {first}"));
    }
    // Changed since: something else (a favourite) is let be; another edit
    // is not written over.
    let now = look_up_asset(client, record)?;
    if edit_of(&now) != edit_of(&asset) {
        return Err(anyhow!("the photo was edited elsewhere meanwhile, so this edit was not written: look at it again"));
    }
    let tag = now["recordChangeTag"].as_str().map(str::to_owned);
    let answer = client.modify(&[(record, tag.as_deref())], &fields)?;
    let first = &answer["records"][0];
    match first["recordChangeTag"].as_str().filter(|_| first["serverErrorCode"].is_null()) {
        Some(tag) => Ok(tag.to_owned()),
        None => Err(anyhow!("iCloud would not take the edit: {first}")),
    }
}

/// The asset's record as iCloud has it now, all its fields.
fn look_up_asset(client: &mut PhotosClient, record: &str) -> Result<Value> {
    let found = client.lookup_all(&[record])?;
    let asset = found["records"].as_array().into_iter().flatten().find(|r| r["recordName"] == record).cloned();
    match asset {
        Some(asset) if asset["serverErrorCode"].is_null() && asset["fields"].is_object() => Ok(asset),
        _ => Err(anyhow!("iCloud has no record of {record}")),
    }
}

/// The turn and crop Photos' recipe on an asset says, as the core keeps
/// them ("turn,x,y,width,height", source.rs's Asset::edit): the inverse of
/// `recipe` and `crop_in_master`. None for a photo not edited, or edited
/// otherwise than Photos' own way.
pub fn from_record(asset: &Value, master: &Value) -> Option<String> {
    let recipe = previous_recipe(&json!({"fields": asset}))?;
    let crop = recipe["adjustments"].as_array()?.iter().find(|j| j["identifier"] == "Crop").map(|j| &j["settings"]);
    let quarters = |code: i64| match code { 6 => Some(1u8), 3 => Some(2), 8 => Some(3), 0 | 1 => Some(0), _ => None };
    let total = quarters(recipe["metadata"]["orientation"].as_i64().unwrap_or(0))?;
    let original = quarters(master["originalOrientation"]["value"].as_i64().unwrap_or(1))?;
    let turn = (4 + total - original) % 4;
    // The stored size: the original's.
    let (w, h) = (master["resOriginalWidth"]["value"].as_f64()?, master["resOriginalHeight"]["value"].as_f64()?);
    let [mut x, mut y, mut cw, mut ch] = match crop {
        Some(c) => {
            let (cx, cy) = (c["xOrigin"].as_f64()?, c["yOrigin"].as_f64()?);
            let (cw, ch) = (c["width"].as_f64()?, c["height"].as_f64()?);
            // From the bottom to from the top, in fractions.
            [cx / w, (h - cy - ch) / h, cw / w, ch / h]
        }
        None => [0.0, 0.0, 1.0, 1.0],
    };
    // From the stored frame to the one shown, a quarter right at a time.
    for _ in 0..total {
        (x, y, cw, ch) = (1.0 - y - ch, x, ch, cw);
    }
    let whole = turn == 0 && x.abs() < 1e-4 && y.abs() < 1e-4 && (cw - 1.0).abs() < 1e-4 && (ch - 1.0).abs() < 1e-4;
    (!whole).then(|| format!("{turn},{x:.6},{y:.6},{cw:.6},{ch:.6}"))
}

/// A binary plist of a JSON value (objects, strings, numbers, booleans),
/// as Apple's `…Enc` fields hold them.
pub fn bplist(value: &Value) -> Vec<u8> {
    // Every object flattened, each a reference by its index.
    fn flatten(value: &Value, objects: &mut Vec<Vec<u8>>) -> usize {
        let at = objects.len();
        objects.push(Vec::new());
        let encoded = match value {
            Value::Object(map) => {
                let keys: Vec<usize> = map.keys().map(|k| flatten(&Value::String(k.clone()), objects)).collect();
                let values: Vec<usize> = map.values().map(|v| flatten(v, objects)).collect();
                let mut out = marker(0xD0, map.len());
                out.extend(keys.iter().chain(values.iter()).map(|&r| r as u8));
                out
            }
            Value::String(s) => {
                let mut out = marker(0x50, s.len());
                out.extend_from_slice(s.as_bytes());
                out
            }
            Value::Bool(b) => vec![if *b { 0x09 } else { 0x08 }],
            Value::Number(n) if n.is_i64() || n.is_u64() => {
                let mut out = vec![0x13];
                out.extend_from_slice(&n.as_i64().unwrap_or(0).to_be_bytes());
                out
            }
            Value::Number(n) => {
                let mut out = vec![0x23];
                out.extend_from_slice(&n.as_f64().unwrap_or(0.0).to_be_bytes());
                out
            }
            _ => vec![0x00],
        };
        objects[at] = encoded;
        at
    }
    // A marker with its count, or the count after it as an int.
    fn marker(kind: u8, count: usize) -> Vec<u8> {
        if count < 15 {
            vec![kind | count as u8]
        } else {
            let mut out = vec![kind | 0x0F, 0x13];
            out.extend_from_slice(&(count as u64).to_be_bytes());
            out
        }
    }
    let mut objects = Vec::new();
    flatten(value, &mut objects);
    let mut out = b"bplist00".to_vec();
    let mut offsets = Vec::new();
    for object in &objects {
        offsets.push(out.len() as u64);
        out.extend_from_slice(object);
    }
    let table = out.len() as u64;
    for offset in &offsets {
        out.extend_from_slice(&offset.to_be_bytes());
    }
    let mut trailer = vec![0u8; 6];
    trailer.extend_from_slice(&[8, 1]); // offsets 8 bytes, references 1
    trailer.extend_from_slice(&(objects.len() as u64).to_be_bytes());
    trailer.extend_from_slice(&0u64.to_be_bytes());
    trailer.extend_from_slice(&table.to_be_bytes());
    out.extend_from_slice(&trailer);
    out
}

/// A binary plist of one date, as `adjustmentTimestampEnc` is: seconds
/// since 2001 as a real.
fn timestamp_plist(now_ms: i64) -> Vec<u8> {
    let seconds = now_ms as f64 / 1000.0 - 978_307_200.0;
    let mut out = b"bplist00".to_vec();
    out.push(0x33); // a date, 8 bytes
    out.extend_from_slice(&seconds.to_be_bytes());
    // Offset table: one object at 8; trailer.
    let offsets = out.len();
    out.push(8);
    let mut trailer = vec![0u8; 6];
    trailer.extend_from_slice(&[1, 1]); // offset int size, object ref size
    trailer.extend_from_slice(&1u64.to_be_bytes()); // objects
    trailer.extend_from_slice(&0u64.to_be_bytes()); // top object
    trailer.extend_from_slice(&(offsets as u64).to_be_bytes());
    out.extend_from_slice(&trailer);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_crop_is_in_the_upright_photos_pixels_turned() {
        let upright = Upright { width: 4032, height: 3024 };
        let left_half = Adjustment { turn: 0, crop: [0.0, 0.0, 0.5, 1.0] };
        assert_eq!(crop_pixels(upright, &left_half), (0, 0, 2016, 3024));
        // Turned a quarter: 3024 wide, 4032 high, the top half kept.
        let turned = Adjustment { turn: 1, crop: [0.0, 0.0, 1.0, 0.5] };
        assert_eq!(crop_pixels(upright, &turned), (0, 0, 3024, 2016));
        assert!(Adjustment { turn: 4, crop: [0.0, 0.0, 1.0, 1.0] }.is_identity());
    }

    #[test]
    fn renditions_are_photos_sizes() {
        assert_eq!(fit_area(3024, 4032, MEDIUM_AREA), (1536, 2048));
        assert_eq!(fit_area(4284, 4284, MEDIUM_AREA), (1774, 1774));
        assert_eq!(fit_area(4032, 3024, THUMB_AREA), (480, 360));
        assert_eq!(fit_area(300, 200, THUMB_AREA), (300, 200));
    }

    #[test]
    fn the_recipe_is_what_an_iphone_wrote_for_the_same_edit() {
        // The iPhone's: a 4032x3024 photo turned a quarter right, cropped
        // to x 831, y 295 from the bottom, 2874 by 2155, rendered 2155x2874
        // (Jacek's reference edit, 2026-11-06).
        let upright = Upright { width: 4032, height: 3024 };
        let turned = Adjustment {
            turn: 1,
            crop: [295.0 / 3024.0, 831.0 / 4032.0, 2155.0 / 3024.0, 2874.0 / 4032.0],
        };
        assert_eq!(crop_in_master(upright, &turned), (831, 295, 2874, 2155));
        assert_eq!(crop_pixels(upright, &turned), (295, 831, 2155, 2874));
        let bytes = recipe(upright, &turned).unwrap();
        let mut json = String::new();
        std::io::Read::read_to_string(&mut flate2::read::DeflateDecoder::new(&bytes[..]), &mut json).unwrap();
        let data: Value = serde_json::from_str(&json).unwrap();
        assert_eq!(data["metadata"]["orientation"], 6);
        assert_eq!(data["metadata"]["masterWidth"], 4032);
        let crop = &data["adjustments"].as_array().unwrap().iter().find(|j| j["identifier"] == "Crop").unwrap()["settings"];
        assert_eq!((crop["xOrigin"].as_u64(), crop["yOrigin"].as_u64(), crop["width"].as_u64(), crop["height"].as_u64()),
                   (Some(831), Some(295), Some(2874), Some(2155)));
        assert_eq!((crop["constraintWidth"].as_u64(), crop["constraintHeight"].as_u64()), (Some(2155), Some(2874)));
        // Untouched: the whole photo, no orientation.
        let whole = Adjustment { turn: 0, crop: [0.0, 0.0, 1.0, 1.0] };
        assert_eq!(crop_in_master(upright, &whole), (0, 0, 4032, 3024));
        // Turned twice and cropped to its top half: the bottom half of the
        // upright photo, which Core Image counts from 0.
        let upside = Adjustment { turn: 2, crop: [0.0, 0.0, 1.0, 0.5] };
        assert_eq!(crop_in_master(upright, &upside), (0, 0, 4032, 1512));
    }

    #[test]
    fn an_iphones_recipe_reads_back_as_the_edit_it_is() {
        // Jacek's reference: stored 4032x3024 upright, turned a quarter
        // right, cropped to 831, 295 (from the bottom), 2874x2155.
        let recipe = recipe_json(Upright { width: 4032, height: 3024 }, &Adjustment {
            turn: 1, crop: [295.0 / 3024.0, 831.0 / 4032.0, 2155.0 / 3024.0, 2874.0 / 4032.0] }, None);
        let asset = json!({"adjustmentType": {"value": "com.apple.photo"},
                           "adjustmentSimpleDataEnc": {"value": base64::engine::general_purpose::STANDARD.encode(deflate(&recipe).unwrap())}});
        let master = json!({"originalOrientation": {"value": 1}, "resOriginalWidth": {"value": 4032}, "resOriginalHeight": {"value": 3024}});
        let edit = from_record(&asset, &master).unwrap();
        let parts: Vec<f64> = edit.split(',').map(|p| p.parse().unwrap()).collect();
        assert_eq!(parts[0], 1.0);
        assert!((parts[1] - 295.0 / 3024.0).abs() < 1e-5 && (parts[2] - 831.0 / 4032.0).abs() < 1e-5, "{edit}");
        // A portrait from the camera, turned no more and cropped to its
        // left half as shown, reads back so.
        let raw = Upright { width: 4032, height: 3024 };
        let recipe = recipe_json(raw, &Adjustment { turn: 1, crop: [0.0, 0.0, 0.5, 1.0] }, None);
        let asset = json!({"adjustmentType": {"value": "com.apple.photo"},
                           "adjustmentSimpleDataEnc": {"value": base64::engine::general_purpose::STANDARD.encode(deflate(&recipe).unwrap())}});
        let master = json!({"originalOrientation": {"value": 6}, "resOriginalWidth": {"value": 4032}, "resOriginalHeight": {"value": 3024}});
        assert_eq!(from_record(&asset, &master).as_deref(), Some("0,0.000000,0.000000,0.500000,1.000000"));
        assert_eq!(from_record(&json!({}), &master), None);
    }

    #[test]
    fn a_portrait_from_the_camera_is_cropped_in_its_stored_pixels() {
        // Stored 4032x3024, shown turned a quarter right (orientation 6),
        // turned no more: the camera's way, the crop in stored pixels and
        // the recipe's orientation 6. Its left half as shown is the bottom
        // half of what is stored, which Core Image counts from 0.
        let raw = Upright { width: 4032, height: 3024 };
        let shown_left_half = Adjustment { turn: 1, crop: [0.0, 0.0, 0.5, 1.0] };
        assert_eq!(crop_in_master(raw, &shown_left_half), (0, 0, 4032, 1512));
    }

    #[test]
    fn a_crop_keeps_the_cameras_style_and_refuses_what_only_apple_renders() {
        let previous = json!({"versionInfo": {"platform": "iOS"}, "adjustments": [
            {"identifier": "SemanticStyle", "settings": {"tone": 0}},
            {"identifier": "Crop", "settings": {"xOrigin": 1}}]});
        let data = recipe_json(Upright { width: 40, height: 30 }, &Adjustment { turn: 0, crop: [0.0, 0.0, 0.5, 1.0] }, Some(&previous));
        let ids: Vec<&str> = data["adjustments"].as_array().unwrap().iter().map(|j| j["identifier"].as_str().unwrap()).collect();
        assert_eq!(ids, ["SemanticStyle", "Crop"]);
        assert_eq!(data["versionInfo"]["platform"], "iOS");
        assert_eq!(needs_apple(&previous), None);
        assert_eq!(needs_apple(&json!({"adjustments": [{"identifier": "DepthEffect"}]})).as_deref(), Some("DepthEffect"));
    }

    #[test]
    fn an_edit_made_elsewhere_or_unreadable_is_never_written_over() {
        let b64 = base64::engine::general_purpose::STANDARD;
        let photos = |data: &str| json!({"fields": {"adjustmentType": {"value": "com.apple.photo"}, "adjustmentSimpleDataEnc": {"value": data}}});
        // Never edited.
        assert_eq!(existing_recipe(&json!({"fields": {"isFavorite": {"value": 1}}})).unwrap(), None);
        // Photos' own, readable.
        let recipe = json!({"adjustments": [{"identifier": "Crop"}]});
        assert_eq!(existing_recipe(&photos(&b64.encode(deflate(&recipe).unwrap()))).unwrap(), Some(recipe));
        // Another app's, Photos' unreadable, Photos' without its recipe,
        // or renditions without a recipe: none of them is lost.
        assert!(existing_recipe(&json!({"fields": {"adjustmentType": {"value": "com.example.editor"}}})).is_err());
        assert!(existing_recipe(&photos("not base64!")).is_err());
        assert!(existing_recipe(&photos(&b64.encode(b"not deflated"))).is_err());
        assert!(existing_recipe(&json!({"fields": {"adjustmentType": {"value": "com.apple.photo"}}})).is_err());
        assert!(existing_recipe(&json!({"fields": {"resJPEGFullRes": {"value": {"fileChecksum": "c"}}}})).is_err());
        assert_eq!(previous_recipe(&json!({"fields": {"adjustmentType": {"value": "com.example.editor"}}})), None);
    }

    #[test]
    fn a_render_goes_only_into_the_folder_it_is_given() {
        let dir = std::env::temp_dir().join(format!("pinhole-edit-none-{}", std::process::id()));
        let e = render(Path::new("/nonexistent.heic"), Upright { width: 4, height: 3 },
                       &Adjustment { turn: 1, crop: [0.0, 0.0, 1.0, 1.0] }, &dir).unwrap_err();
        assert!(format!("{e}").contains("no folder"), "{e}");
        assert!(!dir.exists());
    }

    /// CloudKit as far as one asset's edit goes: its fields and change tag,
    /// modified only with the tag it has now; `meanwhile` is what another
    /// device changes in it just before the first modify gets there.
    struct FakeAsset {
        fields: Value,
        tag: u32,
        meanwhile: Option<Value>,
        modifies: u32,
    }

    fn edit_client(fake: std::sync::Arc<std::sync::Mutex<FakeAsset>>) -> PhotosClient {
        use std::io::{BufRead, BufReader, Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { return };
                let mut reader = BufReader::new(stream.try_clone().unwrap());
                let (mut line, mut length) = (String::new(), 0);
                reader.read_line(&mut line).unwrap();
                let path = line.split(' ').nth(1).unwrap_or_default().split('?').next().unwrap_or_default().to_owned();
                loop {
                    let mut header = String::new();
                    reader.read_line(&mut header).unwrap();
                    if header.trim().is_empty() {
                        break;
                    }
                    if let Some((name, value)) = header.split_once(':') {
                        if name.eq_ignore_ascii_case("content-length") {
                            length = value.trim().parse().unwrap();
                        }
                    }
                }
                let mut body = vec![0; length];
                reader.read_exact(&mut body).unwrap();
                let body: Value = serde_json::from_slice(&body).unwrap();
                let mut fake = fake.lock().unwrap();
                let record = |f: &FakeAsset| json!({"recordName": "A", "recordType": "CPLAsset", "recordChangeTag": format!("t{}", f.tag), "fields": f.fields});
                let answer = if path.ends_with("records/modify") {
                    fake.modifies += 1;
                    if let Some(change) = fake.meanwhile.take() {
                        for (k, v) in change.as_object().unwrap() {
                            fake.fields[k] = v.clone();
                        }
                        fake.tag += 1;
                    }
                    let r = &body["operations"][0]["record"];
                    if r["recordChangeTag"] != format!("t{}", fake.tag) {
                        json!({"records": [{"recordName": "A", "serverErrorCode": "CONFLICT"}]})
                    } else {
                        for (k, v) in r["fields"].as_object().unwrap() {
                            fake.fields[k] = v.clone();
                        }
                        fake.tag += 1;
                        json!({"records": [record(&fake)]})
                    }
                } else {
                    json!({"records": [record(&fake)]})
                };
                let answer = answer.to_string();
                let _ = write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{answer}", answer.len());
            }
        });
        let session: crate::session::Session = serde_json::from_value(json!({
            "apple_id": "a@example.com", "dsid": "1", "services": {"ckdatabasews": format!("http://127.0.0.1:{port}")},
            "params": {}, "headers": {}, "cookies": [], "validated_at": ""
        }))
        .unwrap();
        PhotosClient::new(Box::new(crate::auth::FixedSession(session))).unwrap()
    }

    #[test]
    fn a_change_made_meanwhile_is_written_over_only_if_it_is_not_an_edit() {
        use std::sync::{Arc, Mutex};
        let whole = Adjustment { turn: 0, crop: [0.0, 0.0, 1.0, 1.0] };
        let b64 = base64::engine::general_purpose::STANDARD;
        let pinholes = json!({"adjustmentType": {"value": "com.apple.photo"},
            "adjustmentSimpleDataEnc": {"value": b64.encode(deflate(&json!({"adjustments": [{"identifier": "Crop"}]})).unwrap())},
            "orientation": {"value": 1}});
        // Made a favourite on the iPhone meanwhile: the edit (taken back
        // here) is written again with the tag now.
        let fake = Arc::new(Mutex::new(FakeAsset { fields: pinholes.clone(), tag: 1,
            meanwhile: Some(json!({"isFavorite": {"value": 1}})), modifies: 0 }));
        let tag = write(&mut edit_client(fake.clone()), "A", Some("t1"), Path::new("/unused"), &whole, Path::new("/unused")).unwrap();
        let f = fake.lock().unwrap();
        assert_eq!((tag.as_str(), f.modifies), ("t3", 2));
        assert!(f.fields["adjustmentType"]["value"].is_null() && f.fields["isFavorite"]["value"] == 1);
        drop(f);
        // Edited on the iPhone meanwhile: that edit stays, and this says so.
        let theirs = json!({"adjustmentSimpleDataEnc": {"value": "theirs"}, "adjustmentTimestampEnc": {"value": "now"}});
        let fake = Arc::new(Mutex::new(FakeAsset { fields: pinholes.clone(), tag: 1, meanwhile: Some(theirs), modifies: 0 }));
        let e = write(&mut edit_client(fake.clone()), "A", Some("t1"), Path::new("/unused"), &whole, Path::new("/unused")).unwrap_err();
        assert!(format!("{e}").contains("edited elsewhere"), "{e}");
        let f = fake.lock().unwrap();
        assert_eq!((f.modifies, f.fields["adjustmentSimpleDataEnc"]["value"].as_str()), (1, Some("theirs")));
        drop(f);
        // Another app's edit is not touched at all.
        let fake = Arc::new(Mutex::new(FakeAsset { fields: json!({"adjustmentType": {"value": "com.example.editor"}}),
            tag: 1, meanwhile: None, modifies: 0 }));
        assert!(write(&mut edit_client(fake.clone()), "A", Some("t1"), Path::new("/unused"), &whole, Path::new("/unused")).is_err());
        assert_eq!(fake.lock().unwrap().modifies, 0);
    }

    #[test]
    fn its_fields_carry_each_render_and_revert_takes_them_off() {
        let dir = std::env::temp_dir().join(format!("pinhole-edit-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let renders: [Render; 3] = ["full", "med", "thumb"].map(|n| {
            let path = dir.join(n);
            std::fs::write(&path, b"x").unwrap();
            Render { path, width: 10, height: 20 }
        });
        let mut asked = Vec::new();
        let fields = fields(&renders, b"r", true, 0, |field, _| {
            asked.push(field.to_owned());
            Ok(json!({"receipt": field}))
        }).unwrap();
        assert_eq!(asked, ["resJPEGFullRes", "resJPEGMedRes", "resJPEGThumbRes"]);
        assert_eq!(fields["resJPEGMedRes"]["value"]["receipt"], "resJPEGMedRes");
        assert_eq!(fields["adjustmentType"]["value"], "com.apple.photo");
        assert_eq!(fields["adjustmentRenderType"]["value"], 33280);
        assert!(revert_fields(6)["resJPEGFullRes"]["value"].is_null());
        assert_eq!(revert_fields(6)["orientation"]["value"], 6);
        assert_eq!(fields["orientation"]["value"], 1);
        assert_eq!(fields["adjustedMediaMetaDataStrType"]["value"], "CGImageProperties");
        assert_eq!(&timestamp_plist(978_307_200_000)[..8], b"bplist00");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
