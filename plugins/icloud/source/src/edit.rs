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
/// JPEGs at quality 92 with its colour profile, in `dir`. ImageMagick reads
/// the HEIC upright (`-auto-orient`), then turns and crops it.
pub fn render(original: &Path, upright: Upright, adjustment: &Adjustment, dir: &Path) -> Result<[Render; 3]> {
    std::fs::create_dir_all(dir)?;
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
    let mut adjustments: Vec<Value> = previous
        .and_then(|p| p["adjustments"].as_array())
        .map(|a| a.iter().filter(|j| j["identifier"] != "Crop").cloned().collect())
        .unwrap_or_default();
    adjustments.push(json!({
        "formatVersion": 1, "enabled": true, "identifier": "Crop", "formatIdentifier": "com.apple.photo",
        "settings": {"xOrigin": x, "yOrigin": y, "width": w, "height": h,
                     "constraintWidth": shape_w, "constraintHeight": shape_h,
                     "straightenAngle": 0, "pitch": 0, "yaw": 0, "auto": false},
    }));
    let version = previous
        .map(|p| p["versionInfo"].clone())
        .filter(|v| v.is_object())
        .unwrap_or_else(|| json!({"platform": "OSX", "appVersion": "Pinhole", "buildNumber": "1", "schemaRevision": 0}));
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
    if asset["fields"]["adjustmentType"]["value"] != "com.apple.photo" {
        return None;
    }
    let data = base64::engine::general_purpose::STANDARD
        .decode(asset["fields"]["adjustmentSimpleDataEnc"]["value"].as_str()?)
        .ok()?;
    let mut json = String::new();
    std::io::Read::read_to_string(&mut flate2::read::DeflateDecoder::new(&data[..]), &mut json).ok()?;
    serde_json::from_str(&json).ok()
}

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
        "adjustmentCreatorCode": {"value": "net.becela.pinhole"},
        "adjustmentCompoundVersion": {"value": "1.12"},
        "adjustmentSimpleDataEnc": {"value": b64.encode(recipe)},
        "adjustmentTimestampEnc": {"value": b64.encode(timestamp_plist(now_ms))},
        // As an iPhone's crop of a photo with the camera's style carries
        // (0x8200), or without one.
        "adjustmentRenderType": {"value": if styled { 33280 } else { 32768 }},
        // What described the render before (its EXIF), not this one's.
        "adjustedMediaMetaDataEnc": {"value": null},
        "adjustedMediaMetaDataStrType": {"value": null},
    });
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
pub fn revert_fields() -> Value {
    let mut fields = json!({});
    for name in ["adjustmentType", "adjustmentCreatorCode", "adjustmentCompoundVersion", "adjustmentSimpleDataEnc",
                 "adjustmentTimestampEnc", "adjustmentRenderType", "adjustedMediaMetaDataEnc", "adjustedMediaMetaDataStrType"] {
        fields[name] = json!({"value": null});
    }
    for prefix in ["resJPEGFull", "resJPEGMed", "resJPEGThumb"] {
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
/// and them put on the asset in one modify. Changed since on a device, it
/// is looked up for its tag now and written once more. Its new tag.
pub fn write(client: &mut PhotosClient, record: &str, tag: Option<&str>, original: &Path, adjustment: &Adjustment,
             dir: &Path) -> Result<String> {
    // What the photo's recipe has now (the camera's style, an earlier
    // crop), all its fields asked for.
    let found = client.lookup_all(&[record])?;
    let asset = found["records"].as_array().into_iter().flatten().find(|r| r["recordName"] == record).cloned().unwrap_or_default();
    let previous = previous_recipe(&asset);
    if let Some(effect) = previous.as_ref().and_then(needs_apple) {
        return Err(anyhow!("edited with {effect}, which only Photos renders: edit it on the iPhone"));
    }
    let tag = asset["recordChangeTag"].as_str().or(tag);
    let fields = if adjustment.is_identity() {
        revert_fields()
    } else {
        let upright = upright_of(original)?;
        let renders = render(original, upright, adjustment, dir)?;
        let data = recipe_json(upright, adjustment, previous.as_ref());
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
    let now = client.lookup(&[record])?;
    let tag = now["records"][0]["recordChangeTag"].as_str().map(str::to_owned);
    let answer = client.modify(&[(record, tag.as_deref())], &fields)?;
    let first = &answer["records"][0];
    match first["recordChangeTag"].as_str().filter(|_| first["serverErrorCode"].is_null()) {
        Some(tag) => Ok(tag.to_owned()),
        None => Err(anyhow!("iCloud would not take the edit: {first}")),
    }
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
        let crop = &data["adjustments"][0]["settings"];
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
        assert!(revert_fields()["resJPEGFullRes"]["value"].is_null());
        assert_eq!(&timestamp_plist(978_307_200_000)[..8], b"bplist00");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
