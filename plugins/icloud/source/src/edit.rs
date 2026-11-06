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

/// Apple's recipe for it: a `Crop` in the upright photo's pixels and the
/// turn as an EXIF orientation in its metadata, deflated, as an iPhone's
/// `adjustmentSimpleDataEnc` is.
pub fn recipe(upright: Upright, adjustment: &Adjustment) -> Result<Vec<u8>> {
    let (x, y, w, h) = crop_pixels(upright, adjustment);
    // EXIF's codes for a picture turned 0, 90, 180, 270 degrees clockwise.
    let orientation = [1, 6, 3, 8][(adjustment.turn % 4) as usize];
    let data = json!({
        "metadata": {"masterWidth": upright.width, "masterHeight": upright.height, "orientation": orientation},
        "formatVersion": 1,
        "versionInfo": {"platform": "OSX", "appVersion": "Pinhole", "buildNumber": "1", "schemaRevision": 0},
        "adjustments": [{
            "formatVersion": 1, "enabled": true, "identifier": "Crop", "formatIdentifier": "com.apple.photo",
            "settings": {"xOrigin": x, "yOrigin": y, "width": w, "height": h,
                         "constraintWidth": 0, "constraintHeight": 0,
                         "straightenAngle": 0, "pitch": 0, "yaw": 0, "auto": false},
        }],
    });
    let mut deflated = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::default());
    deflated.write_all(data.to_string().as_bytes())?;
    Ok(deflated.finish()?)
}

/// The asset's fields for the edit, the three renders' receipts given
/// (`receipt(field)` uploads a file as that field and answers its value).
pub fn fields(
    renders: &[Render; 3],
    recipe: &[u8],
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
        // A render that is the photo turned and cropped: what an iPhone's
        // crop edits carry.
        "adjustmentRenderType": {"value": 0},
    });
    for (render, prefix) in renders.iter().zip(["resJPEGFull", "resJPEGMed", "resJPEGThumb"]) {
        let value = receipt(&format!("{prefix}Res"), &render.path)?;
        let size = std::fs::metadata(&render.path)?.len();
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
    fn the_recipe_is_apples_inflated_json() {
        let bytes = recipe(Upright { width: 4032, height: 3024 }, &Adjustment { turn: 1, crop: [0.0, 0.0, 1.0, 0.5] }).unwrap();
        let mut json = String::new();
        std::io::Read::read_to_string(&mut flate2::read::DeflateDecoder::new(&bytes[..]), &mut json).unwrap();
        let data: Value = serde_json::from_str(&json).unwrap();
        assert_eq!(data["metadata"]["orientation"], 6);
        assert_eq!(data["adjustments"][0]["identifier"], "Crop");
        assert_eq!(data["adjustments"][0]["settings"]["height"], 2016);
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
        let fields = fields(&renders, b"r", 0, |field, _| {
            asked.push(field.to_owned());
            Ok(json!({"receipt": field}))
        }).unwrap();
        assert_eq!(asked, ["resJPEGFullRes", "resJPEGMedRes", "resJPEGThumbRes"]);
        assert_eq!(fields["resJPEGMedRes"]["value"]["receipt"], "resJPEGMedRes");
        assert_eq!(fields["adjustmentType"]["value"], "com.apple.photo");
        assert!(revert_fields()["resJPEGFullRes"]["value"].is_null());
        assert_eq!(&timestamp_plist(978_307_200_000)[..8], b"bplist00");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
