//! A HEIC's gain map, kept when the photo leaves as a JPEG (docs/hdr.md,
//! H5): read from the HEIC through libheif, and written beside the JPEG
//! ImageMagick made as an Ultra HDR JPEG (ISO 21496-1 and Adobe's XMP, an
//! MPF second image) by libultrahdr. Android, Chrome and Apple since iOS 18
//! show it above SDR white; anything else sees the JPEG it always saw.
//!
//! The map is ISO's when the HEIC has a `tmap` whose metadata parses (an
//! iPhone's since iOS 18), its metadata kept as it is; else Apple's legacy
//! map (`urn:com:apple:photo:2020:aux:hdrgainmap`), turned into ISO's form:
//! each code `g` (Rec. 709's curve) becomes `log2(1 + (Hi - 1) g)` over
//! `[0, log2 Hi]`, gamma 1, offsets 0, where `Hi` is its headroom (its XMP's
//! `HDRGainMapHeadroom`, else the MakerNote's tags 33 and 48).
//!
//! libheif and libultrahdr through a few declarations by hand: the few
//! functions used, with the C layouts of their headers.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

use anyhow::{anyhow, bail, Context, Result};

/// A gain map: its codes (one channel or three, row by row) and its
/// metadata as libultrahdr takes it (linear boosts and capacities).
#[derive(Clone, Debug, PartialEq)]
pub struct Map {
    pub width: u32,
    pub height: u32,
    pub channels: u32,
    pub pixels: Vec<u8>,
    pub meta: Meta,
    /// "iso" or "apple": where it came from.
    pub kind: &'static str,
}

/// ISO 21496-1's metadata, as libultrahdr has it: per channel the largest
/// and smallest boost (linear), gamma and offsets; the display boosts it
/// applies from and fully at (linear); the base's colour space or not.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Meta {
    pub max_content_boost: [f32; 3],
    pub min_content_boost: [f32; 3],
    pub gamma: [f32; 3],
    pub offset_sdr: [f32; 3],
    pub offset_hdr: [f32; 3],
    pub hdr_capacity_min: f32,
    pub hdr_capacity_max: f32,
    pub use_base_cg: c_int,
}

/// The gain map of the HEIF at `path`, if it has one that decodes.
pub fn read(path: &Path) -> Option<Map> {
    // A HEIF (HEIC, AVIF) by its content.
    let mut head = [0u8; 12];
    std::io::Read::read_exact(&mut std::fs::File::open(path).ok()?, &mut head).ok()?;
    if &head[4..8] != b"ftyp" {
        return None;
    }
    let heif = Heif::open(path).ok()?;
    heif.iso().or_else(|| heif.apple())
}

/// The JPEG `base` (its bytes, as ImageMagick made it) with `map` added: an
/// Ultra HDR JPEG. Its own segments (EXIF, ICC) stay where they were.
pub fn attach(base: &[u8], map: &Map) -> Result<Vec<u8>> {
    // At 90, as the photo's own 92: a tenth more than the JPEG alone for an
    // iPhone's 12 MP (its map a quarter of the pixels), each code within a
    // few of the HEIC's own (itself lossy, HEVC).
    let mut coded = Vec::new();
    let color = if map.channels == 3 { image::ExtendedColorType::Rgb8 } else { image::ExtendedColorType::L8 };
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut coded, 90)
        .encode(&map.pixels, map.width, map.height, color)
        .context("compressing the gain map")?;
    unsafe {
        let encoder = uhdr_create_encoder();
        if encoder.is_null() {
            bail!("libultrahdr: no encoder");
        }
        let done = (|| -> Result<Vec<u8>> {
            let mut base_image = CompressedImage {
                data: base.as_ptr() as *mut c_void,
                data_sz: base.len(),
                capacity: base.len(),
                cg: gamut_of(base),
                ct: UHDR_CT_SRGB,
                range: UHDR_CR_FULL_RANGE,
            };
            let mut map_image = CompressedImage {
                data: coded.as_mut_ptr() as *mut c_void,
                data_sz: coded.len(),
                capacity: coded.len(),
                cg: UHDR_CG_UNSPECIFIED,
                ct: UHDR_CT_UNSPECIFIED,
                range: UHDR_CR_UNSPECIFIED,
            };
            let mut meta = map.meta;
            check(uhdr_enc_set_compressed_image(encoder, &mut base_image, UHDR_BASE_IMG))?;
            check(uhdr_enc_set_gainmap_image(encoder, &mut map_image, &mut meta))?;
            check(uhdr_encode(encoder))?;
            let out = uhdr_get_encoded_stream(encoder);
            if out.is_null() || (*out).data.is_null() {
                bail!("libultrahdr wrote nothing");
            }
            Ok(std::slice::from_raw_parts((*out).data as *const u8, (*out).data_sz).to_vec())
        })();
        uhdr_release_encoder(encoder);
        done
    }
}

/// The base's gamut, which libultrahdr asks for, by the profile it carries:
/// Display P3 (an iPhone's), BT.2020, else sRGB's (BT.709's primaries). The
/// profile itself stays in the JPEG.
fn gamut_of(jpeg: &[u8]) -> c_int {
    let has = |s: &[u8]| jpeg.windows(s.len()).any(|w| w == s);
    if has(b"Display P3") || has(b"D\0i\0s\0p\0l\0a\0y\0 \0P\03") {
        UHDR_CG_DISPLAY_P3
    } else if has(b"2020") {
        UHDR_CG_BT_2100
    } else {
        UHDR_CG_BT_709
    }
}

/// What libultrahdr reads back of an Ultra HDR JPEG: its map's codes
/// (decoded), its metadata, and its primary image as it is in the file.
pub fn probe(jpeg: &[u8]) -> Result<(image::DynamicImage, Meta, Vec<u8>)> {
    unsafe {
        let decoder = uhdr_create_decoder();
        if decoder.is_null() {
            bail!("libultrahdr: no decoder");
        }
        let done = (|| -> Result<(image::DynamicImage, Meta, Vec<u8>)> {
            let mut input = CompressedImage {
                data: jpeg.as_ptr() as *mut c_void,
                data_sz: jpeg.len(),
                capacity: jpeg.len(),
                cg: UHDR_CG_UNSPECIFIED,
                ct: UHDR_CT_UNSPECIFIED,
                range: UHDR_CR_UNSPECIFIED,
            };
            check(uhdr_dec_set_image(decoder, &mut input))?;
            check(uhdr_dec_probe(decoder))?;
            let meta = uhdr_dec_get_gainmap_metadata(decoder);
            let map = uhdr_dec_get_gainmap_image(decoder);
            let base = uhdr_dec_get_base_image(decoder);
            if meta.is_null() || map.is_null() || base.is_null() {
                bail!("no gain map");
            }
            let coded = std::slice::from_raw_parts((*map).data as *const u8, (*map).data_sz);
            let image = image::load_from_memory_with_format(coded, image::ImageFormat::Jpeg)?;
            let base = std::slice::from_raw_parts((*base).data as *const u8, (*base).data_sz).to_vec();
            Ok((image, *meta, base))
        })();
        uhdr_release_decoder(decoder);
        done
    }
}

/// Apple's legacy map in ISO's form: a code `g` (Rec. 709's curve) to
/// `log2(1 + (Hi - 1) g) / log2(Hi)`, over boosts 1 to `Hi`, gamma 1.
pub fn apple_to_iso(code: u8, headroom: f64) -> u8 {
    let e = code as f64 / 255.0;
    let g = if e <= 0.081 { e / 4.5 } else { ((e + 0.099) / 1.099).powf(1.0 / 0.45) };
    let l = (1.0 + (headroom - 1.0) * g).log2() / headroom.log2();
    (l * 255.0).round().clamp(0.0, 255.0) as u8
}

/// The stops Apple's MakerNote tags 33 and 48 make (docs/hdr.md), at least 0.
pub fn apple_maker_stops(m33: f64, m48: f64) -> f64 {
    let stops = if m33 < 1.0 {
        if m48 <= 0.01 { -20.0 * m48 + 1.8 } else { -0.101 * m48 + 1.601 }
    } else if m48 <= 0.01 {
        -70.0 * m48 + 3.0
    } else {
        -0.303 * m48 + 2.303
    };
    stops.max(0.0)
}

/// ISO 21496-1's metadata (a `tmap` item after its version byte), as
/// libultrahdr takes it; None if it does not parse or its base is the HDR
/// picture (which an Ultra HDR JPEG cannot say).
pub fn iso_metadata(data: &[u8]) -> Option<Meta> {
    let mut r = Bytes { data, at: 0 };
    if r.u16()? != 0 {
        return None;
    }
    r.u16()?;
    let flags = r.u8()?;
    let channels = if flags & 0x80 != 0 { 3 } else { 1 };
    let use_base = flags & 0x40 != 0;
    if flags & 0x04 != 0 {
        return None;
    }
    let common = flags & 0x08 != 0;
    let denominator = if common { Some(r.u32()?) } else { None };
    let mut fraction = |signed: bool| -> Option<f64> {
        let n = r.u32()?;
        let d = match denominator {
            Some(d) => d,
            None => r.u32()?,
        };
        if d == 0 {
            return None;
        }
        Some(if signed { n as i32 as f64 } else { n as f64 } / d as f64)
    };
    let base_headroom = fraction(false)?;
    let alternate_headroom = fraction(false)?;
    let mut m = Meta {
        max_content_boost: [1.0; 3],
        min_content_boost: [1.0; 3],
        gamma: [1.0; 3],
        offset_sdr: [0.0; 3],
        offset_hdr: [0.0; 3],
        hdr_capacity_min: base_headroom.exp2() as f32,
        hdr_capacity_max: alternate_headroom.exp2() as f32,
        use_base_cg: use_base as c_int,
    };
    for c in 0..channels {
        m.min_content_boost[c] = fraction(true)?.exp2() as f32;
        m.max_content_boost[c] = fraction(true)?.exp2() as f32;
        m.gamma[c] = fraction(false)? as f32;
        m.offset_sdr[c] = fraction(true)? as f32;
        m.offset_hdr[c] = fraction(true)? as f32;
        if m.gamma[c] <= 0.0 {
            return None;
        }
    }
    for c in channels..3 {
        m.min_content_boost[c] = m.min_content_boost[0];
        m.max_content_boost[c] = m.max_content_boost[0];
        m.gamma[c] = m.gamma[0];
        m.offset_sdr[c] = m.offset_sdr[0];
        m.offset_hdr[c] = m.offset_hdr[0];
    }
    (alternate_headroom > base_headroom).then_some(m)
}

/// Tags 33 and 48 of Apple's MakerNote in an Exif block (a TIFF header and
/// on).
pub fn apple_maker_tags(tiff: &[u8]) -> Option<(f64, f64)> {
    let little = match tiff.get(..2)? {
        b"II" => true,
        b"MM" => false,
        _ => return None,
    };
    let ifd0 = read_u32(tiff, 4, little)? as usize;
    let pointer = tiff_entry(tiff, little, ifd0, 0x8769)?;
    let exif_ifd = read_u32(tiff, pointer + 8, little)? as usize;
    let maker = tiff_entry(tiff, little, exif_ifd, 0x927c)?;
    let size = read_u32(tiff, maker + 4, little)? as usize;
    let at = read_u32(tiff, maker + 8, little)? as usize;
    let note = tiff.get(at..at.checked_add(size)?)?;
    // "Apple iOS\0", a version, "MM", then the IFD; offsets from its start.
    if size < 16 || !note.starts_with(b"Apple iOS") {
        return None;
    }
    let note_little = note[12] == b'I';
    let e33 = tiff_entry(note, note_little, 14, 33)?;
    let e48 = tiff_entry(note, note_little, 14, 48)?;
    Some((tiff_number(note, note_little, e33)?, tiff_number(note, note_little, e48)?))
}

fn read_u16(data: &[u8], at: usize, little: bool) -> Option<u16> {
    let b: [u8; 2] = data.get(at..at.checked_add(2)?)?.try_into().ok()?;
    Some(if little { u16::from_le_bytes(b) } else { u16::from_be_bytes(b) })
}

fn read_u32(data: &[u8], at: usize, little: bool) -> Option<u32> {
    let b: [u8; 4] = data.get(at..at.checked_add(4)?)?.try_into().ok()?;
    Some(if little { u32::from_le_bytes(b) } else { u32::from_be_bytes(b) })
}

/// The entry of `tag` in the IFD at `ifd`.
fn tiff_entry(data: &[u8], little: bool, ifd: usize, tag: u16) -> Option<usize> {
    let count = read_u16(data, ifd, little)? as usize;
    (0..count).map(|i| ifd + 2 + 12 * i).find(|&entry| read_u16(data, entry, little) == Some(tag))
}

/// An entry's value as a number: a short, a long, a rational or a float;
/// offsets count from the start of `data`.
fn tiff_number(data: &[u8], little: bool, entry: usize) -> Option<f64> {
    let kind = read_u16(data, entry + 2, little)?;
    let value = entry + 8;
    let far = || read_u32(data, value, little).map(|o| o as usize);
    match kind {
        3 => read_u16(data, value, little).map(f64::from),
        4 => read_u32(data, value, little).map(f64::from),
        9 => read_u32(data, value, little).map(|v| v as i32 as f64),
        5 | 10 => {
            let at = far()?;
            let (n, d) = (read_u32(data, at, little)?, read_u32(data, at + 4, little)?);
            if d == 0 {
                return None;
            }
            Some(if kind == 5 { n as f64 / d as f64 } else { n as i32 as f64 / d as i32 as f64 })
        }
        11 => read_u32(data, value, little).map(|v| f32::from_bits(v) as f64),
        _ => None,
    }
}

struct Bytes<'a> {
    data: &'a [u8],
    at: usize,
}

impl Bytes<'_> {
    fn take<const N: usize>(&mut self) -> Option<[u8; N]> {
        let b = self.data.get(self.at..self.at + N)?.try_into().ok()?;
        self.at += N;
        Some(b)
    }
    fn u8(&mut self) -> Option<u8> {
        self.take::<1>().map(|b| b[0])
    }
    fn u16(&mut self) -> Option<u16> {
        self.take::<2>().map(u16::from_be_bytes)
    }
    fn u32(&mut self) -> Option<u32> {
        self.take::<4>().map(u32::from_be_bytes)
    }
}

fn check(e: ErrorInfo) -> Result<()> {
    if e.error_code == UHDR_CODEC_OK {
        return Ok(());
    }
    let detail = if e.has_detail != 0 {
        unsafe { CStr::from_ptr(e.detail.as_ptr()) }.to_string_lossy().into_owned()
    } else {
        String::new()
    };
    Err(anyhow!("libultrahdr: error {} {detail}", e.error_code))
}

// ---- libheif ----

const APPLE_AUX: &[u8] = b"urn:com:apple:photo:2020:aux:hdrgainmap";

fn fourcc(s: &[u8; 4]) -> u32 {
    u32::from_be_bytes(*s)
}

/// A HEIF file open in libheif, with its primary picture.
struct Heif {
    context: *mut c_void,
    primary: *mut c_void,
}

impl Drop for Heif {
    fn drop(&mut self) {
        unsafe {
            if !self.primary.is_null() {
                heif_image_handle_release(self.primary);
            }
            heif_context_free(self.context);
        }
    }
}

impl Heif {
    fn open(path: &Path) -> Result<Heif> {
        let name = CString::new(path.as_os_str().as_bytes())?;
        unsafe {
            let mut heif = Heif { context: heif_context_alloc(), primary: std::ptr::null_mut() };
            if heif.context.is_null() {
                bail!("libheif: no context");
            }
            if heif_context_read_from_file(heif.context, name.as_ptr(), std::ptr::null()).code != 0
                || heif_context_get_primary_image_handle(heif.context, &mut heif.primary).code != 0
            {
                bail!("libheif cannot read {}", path.display());
            }
            Ok(heif)
        }
    }

    fn items(&self) -> Vec<u32> {
        unsafe {
            let count = heif_context_get_number_of_items(self.context).max(0);
            let mut ids = vec![0u32; count as usize];
            let got = heif_context_get_list_of_item_IDs(self.context, ids.as_mut_ptr(), count);
            ids.truncate(got.max(0) as usize);
            ids
        }
    }

    /// The items `id` refers to by `kind` (all of them, in order).
    fn references(&self, id: u32, kind: u32) -> Vec<u32> {
        let mut out = Vec::new();
        unsafe {
            for index in 0.. {
                let mut found_kind = 0u32;
                let mut to: *mut u32 = std::ptr::null_mut();
                let count = heif_context_get_item_references(self.context, id, index, &mut found_kind, &mut to);
                if count == 0 {
                    break;
                }
                if found_kind == kind {
                    out.extend_from_slice(std::slice::from_raw_parts(to, count));
                }
                heif_release_item_references(self.context, &mut to);
            }
        }
        out
    }

    fn data(&self, id: u32) -> Option<Vec<u8>> {
        unsafe {
            let mut bytes: *mut u8 = std::ptr::null_mut();
            let mut size = 0usize;
            let ok = heif_item_get_item_data(self.context, id, std::ptr::null_mut(), &mut bytes, &mut size).code == 0;
            let out = (ok && !bytes.is_null() && size < 64 << 20).then(|| std::slice::from_raw_parts(bytes, size).to_vec());
            if !bytes.is_null() {
                heif_release_item_data(self.context, &mut bytes);
            }
            out
        }
    }

    /// The image item `id`'s pixels at 8 bits, its own rotation applied:
    /// one channel or three.
    fn pixels(handle: *mut c_void, channels: u32) -> Option<(u32, u32, Vec<u8>)> {
        unsafe {
            let mut image: *mut c_void = std::ptr::null_mut();
            let (space, chroma, channel) = if channels == 3 {
                (HEIF_COLORSPACE_RGB, HEIF_CHROMA_INTERLEAVED_RGB, HEIF_CHANNEL_INTERLEAVED)
            } else {
                (HEIF_COLORSPACE_MONOCHROME, HEIF_CHROMA_PLANAR, HEIF_CHANNEL_Y)
            };
            if heif_decode_image(handle, &mut image, space, chroma, std::ptr::null()).code != 0 || image.is_null() {
                return None;
            }
            let mut stride = 0usize;
            let plane = heif_image_get_plane_readonly2(image, channel, &mut stride);
            let width = heif_image_get_width(image, channel);
            let height = heif_image_get_height(image, channel);
            let bits = heif_image_get_bits_per_pixel_range(image, channel);
            let out = (!plane.is_null() && width > 0 && height > 0 && bits == 8).then(|| {
                let row = width as usize * channels as usize;
                let mut pixels = Vec::with_capacity(row * height as usize);
                for y in 0..height as usize {
                    pixels.extend_from_slice(std::slice::from_raw_parts(plane.add(y * stride), row));
                }
                (width as u32, height as u32, pixels)
            });
            heif_image_release(image);
            out
        }
    }

    /// The handle of image item `id`: an item of its own, or the primary's
    /// auxiliary image (an iPhone's map is both).
    fn handle(&self, id: u32) -> Option<*mut c_void> {
        unsafe {
            let mut handle: *mut c_void = std::ptr::null_mut();
            if heif_context_get_image_handle(self.context, id, &mut handle).code == 0 && !handle.is_null() {
                return Some(handle);
            }
            (heif_image_handle_get_auxiliary_image_handle(self.primary, id, &mut handle).code == 0 && !handle.is_null())
                .then_some(handle)
        }
    }

    /// ISO's: a tmap whose inputs are the primary and the map.
    fn iso(&self) -> Option<Map> {
        let primary = unsafe { heif_image_handle_get_item_id(self.primary) };
        for id in self.items() {
            if unsafe { heif_item_get_item_type(self.context, id) } != fourcc(b"tmap") {
                continue;
            }
            let inputs = self.references(id, fourcc(b"dimg"));
            if inputs.len() != 2 || inputs[0] != primary {
                continue;
            }
            let Some(meta) = self.data(id).and_then(|d| match d.split_first() {
                Some((0, rest)) => iso_metadata(rest),
                _ => None,
            }) else {
                continue;
            };
            let same = (1..3).all(|c| {
                meta.max_content_boost[c] == meta.max_content_boost[0]
                    && meta.min_content_boost[c] == meta.min_content_boost[0]
                    && meta.gamma[c] == meta.gamma[0]
            });
            let Some(handle) = self.handle(inputs[1]) else { continue };
            let channels = if same { 1 } else { 3 };
            let pixels = Heif::pixels(handle, channels);
            unsafe { heif_image_handle_release(handle) };
            if let Some((width, height, pixels)) = pixels {
                return Some(Map { width, height, channels, pixels, meta, kind: "iso" });
            }
        }
        None
    }

    /// Apple's, made ISO's: the auxiliary image and its headroom.
    fn apple(&self) -> Option<Map> {
        let filter = (1 << 1) | (2 << 1);
        let ids = unsafe {
            let count = heif_image_handle_get_number_of_auxiliary_images(self.primary, filter).max(0);
            let mut ids = vec![0u32; count as usize];
            heif_image_handle_get_list_of_auxiliary_image_IDs(self.primary, filter, ids.as_mut_ptr(), count);
            ids
        };
        for id in ids {
            let mut handle: *mut c_void = std::ptr::null_mut();
            unsafe {
                if heif_image_handle_get_auxiliary_image_handle(self.primary, id, &mut handle).code != 0 || handle.is_null() {
                    continue;
                }
                let mut kind: *const c_char = std::ptr::null();
                let apple = heif_image_handle_get_auxiliary_type(handle, &mut kind).code == 0
                    && !kind.is_null()
                    && CStr::from_ptr(kind).to_bytes() == APPLE_AUX;
                if !kind.is_null() {
                    heif_image_handle_release_auxiliary_type(handle, &mut kind);
                }
                let pixels = if apple { Heif::pixels(handle, 1) } else { None };
                heif_image_handle_release(handle);
                let Some((width, height, pixels)) = pixels else { continue };
                let stops = self.apple_stops(id)?;
                if stops <= 0.01 {
                    return None;
                }
                let headroom = stops.exp2();
                let table: Vec<u8> = (0..=255u8).map(|c| apple_to_iso(c, headroom)).collect();
                let meta = Meta {
                    max_content_boost: [headroom as f32; 3],
                    min_content_boost: [1.0; 3],
                    gamma: [1.0; 3],
                    offset_sdr: [0.0; 3],
                    offset_hdr: [0.0; 3],
                    hdr_capacity_min: 1.0,
                    hdr_capacity_max: headroom as f32,
                    use_base_cg: 1,
                };
                let pixels = pixels.iter().map(|&c| table[c as usize]).collect();
                return Some(Map { width, height, channels: 1, pixels, meta, kind: "apple" });
            }
        }
        None
    }

    /// Apple's map's headroom in stops: its XMP's HDRGainMapHeadroom
    /// (linear), else the MakerNote's.
    fn apple_stops(&self, map: u32) -> Option<f64> {
        let primary = unsafe { heif_image_handle_get_item_id(self.primary) };
        let mut maker = None;
        for id in self.items() {
            let kind = unsafe { heif_item_get_item_type(self.context, id) };
            if kind == fourcc(b"mime") && self.references(id, fourcc(b"cdsc")).contains(&map) {
                if let Some(h) = self.data(id).and_then(|d| xmp_number(&d, "HDRGainMap:HDRGainMapHeadroom")) {
                    if h > 0.0 {
                        return Some(h.log2().max(0.0));
                    }
                }
            } else if kind == fourcc(b"Exif") && self.references(id, fourcc(b"cdsc")).iter().any(|&t| t == primary || t == map) {
                // Four bytes first: where in it the TIFF header is.
                maker = maker.or_else(|| {
                    let data = self.data(id)?;
                    let skip = u32::from_be_bytes(data.get(..4)?.try_into().ok()?) as usize;
                    let (m33, m48) = apple_maker_tags(data.get(4usize.checked_add(skip)?..)?)?;
                    Some(apple_maker_stops(m33, m48))
                });
            }
        }
        maker
    }
}

/// A number in an XMP packet: an attribute or an element of that name.
fn xmp_number(xmp: &[u8], name: &str) -> Option<f64> {
    let text = String::from_utf8_lossy(xmp);
    let at = text.find(name)? + name.len();
    let rest = text[at..].trim_start();
    let rest = rest.strip_prefix('=').map(|r| r.trim_start().trim_start_matches(['"', '\''])).or_else(|| rest.strip_prefix('>'))?;
    let end = rest.find(|c: char| !(c.is_ascii_digit() || matches!(c, '.' | '-' | '+' | 'e' | 'E'))).unwrap_or(rest.len());
    rest[..end].parse().ok().filter(|v: &f64| v.is_finite())
}

// The C side. libheif 1.23 (heif.h and the headers it includes), libultrahdr
// 2.0 (ultrahdr_api.h).

#[repr(C)]
struct HeifError {
    code: c_int,
    subcode: c_int,
    message: *const c_char,
}

const HEIF_COLORSPACE_RGB: c_int = 1;
const HEIF_COLORSPACE_MONOCHROME: c_int = 2;
const HEIF_CHROMA_PLANAR: c_int = 0;
const HEIF_CHROMA_INTERLEAVED_RGB: c_int = 10;
const HEIF_CHANNEL_Y: c_int = 0;
const HEIF_CHANNEL_INTERLEAVED: c_int = 10;

#[link(name = "heif")]
extern "C" {
    fn heif_context_alloc() -> *mut c_void;
    fn heif_context_free(context: *mut c_void);
    fn heif_context_read_from_file(context: *mut c_void, filename: *const c_char, options: *const c_void) -> HeifError;
    fn heif_context_get_primary_image_handle(context: *mut c_void, handle: *mut *mut c_void) -> HeifError;
    fn heif_context_get_image_handle(context: *mut c_void, id: u32, handle: *mut *mut c_void) -> HeifError;
    fn heif_image_handle_release(handle: *mut c_void);
    fn heif_image_handle_get_item_id(handle: *const c_void) -> u32;
    fn heif_context_get_number_of_items(context: *const c_void) -> c_int;
    fn heif_context_get_list_of_item_IDs(context: *const c_void, ids: *mut u32, count: c_int) -> c_int;
    fn heif_item_get_item_type(context: *const c_void, id: u32) -> u32;
    fn heif_context_get_item_references(
        context: *const c_void,
        from: u32,
        index: c_int,
        kind: *mut u32,
        to: *mut *mut u32,
    ) -> usize;
    fn heif_release_item_references(context: *const c_void, references: *mut *mut u32);
    fn heif_item_get_item_data(
        context: *const c_void,
        id: u32,
        compression: *mut c_int,
        data: *mut *mut u8,
        size: *mut usize,
    ) -> HeifError;
    fn heif_release_item_data(context: *const c_void, data: *mut *mut u8);
    fn heif_image_handle_get_number_of_auxiliary_images(handle: *const c_void, filter: c_int) -> c_int;
    fn heif_image_handle_get_list_of_auxiliary_image_IDs(handle: *const c_void, filter: c_int, ids: *mut u32, count: c_int)
        -> c_int;
    fn heif_image_handle_get_auxiliary_image_handle(handle: *const c_void, id: u32, out: *mut *mut c_void) -> HeifError;
    fn heif_image_handle_get_auxiliary_type(handle: *const c_void, kind: *mut *const c_char) -> HeifError;
    fn heif_image_handle_release_auxiliary_type(handle: *const c_void, kind: *mut *const c_char);
    fn heif_decode_image(
        handle: *const c_void,
        image: *mut *mut c_void,
        colorspace: c_int,
        chroma: c_int,
        options: *const c_void,
    ) -> HeifError;
    fn heif_image_release(image: *const c_void);
    fn heif_image_get_plane_readonly2(image: *const c_void, channel: c_int, stride: *mut usize) -> *const u8;
    fn heif_image_get_width(image: *const c_void, channel: c_int) -> c_int;
    fn heif_image_get_height(image: *const c_void, channel: c_int) -> c_int;
    fn heif_image_get_bits_per_pixel_range(image: *const c_void, channel: c_int) -> c_int;
}

#[repr(C)]
struct ErrorInfo {
    error_code: c_int,
    has_detail: c_int,
    detail: [c_char; 256],
}

#[repr(C)]
struct CompressedImage {
    data: *mut c_void,
    data_sz: usize,
    capacity: usize,
    cg: c_int,
    ct: c_int,
    range: c_int,
}

#[repr(C)]
struct MemBlock {
    data: *mut c_void,
    data_sz: usize,
    capacity: usize,
}

const UHDR_CODEC_OK: c_int = 0;
const UHDR_CG_UNSPECIFIED: c_int = -1;
const UHDR_CG_BT_709: c_int = 0;
const UHDR_CG_DISPLAY_P3: c_int = 1;
const UHDR_CG_BT_2100: c_int = 2;
const UHDR_CT_UNSPECIFIED: c_int = -1;
const UHDR_CT_SRGB: c_int = 3;
const UHDR_CR_UNSPECIFIED: c_int = -1;
const UHDR_CR_FULL_RANGE: c_int = 1;
const UHDR_BASE_IMG: c_int = 2;

#[link(name = "uhdr")]
extern "C" {
    fn uhdr_create_encoder() -> *mut c_void;
    fn uhdr_release_encoder(encoder: *mut c_void);
    fn uhdr_enc_set_compressed_image(encoder: *mut c_void, image: *mut CompressedImage, intent: c_int) -> ErrorInfo;
    fn uhdr_enc_set_gainmap_image(encoder: *mut c_void, image: *mut CompressedImage, meta: *mut Meta) -> ErrorInfo;
    fn uhdr_encode(encoder: *mut c_void) -> ErrorInfo;
    fn uhdr_get_encoded_stream(encoder: *mut c_void) -> *mut CompressedImage;
    fn uhdr_create_decoder() -> *mut c_void;
    fn uhdr_release_decoder(decoder: *mut c_void);
    fn uhdr_dec_set_image(decoder: *mut c_void, image: *mut CompressedImage) -> ErrorInfo;
    fn uhdr_dec_probe(decoder: *mut c_void) -> ErrorInfo;
    fn uhdr_dec_get_gainmap_metadata(decoder: *mut c_void) -> *mut Meta;
    fn uhdr_dec_get_gainmap_image(decoder: *mut c_void) -> *mut MemBlock;
    fn uhdr_dec_get_base_image(decoder: *mut c_void) -> *mut MemBlock;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_iphones_tmap_is_read_as_libultrahdr_takes_it() {
        // An iPhone's (iOS 18), after its version byte: one channel, the
        // base's colour space.
        let hex = "000000004000000000000f42400024543b000f424000000000000f42400024543b000f4240000cb9c0000f42400000000a000f4240\
                   0000000a000f4240";
        let bytes: Vec<u8> = (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect();
        let m = iso_metadata(&bytes).unwrap();
        assert_eq!(m.hdr_capacity_min, 1.0);
        assert!((m.hdr_capacity_max - 2.380859f64.exp2() as f32).abs() < 1e-5);
        assert_eq!(m.min_content_boost, [1.0; 3]);
        assert!((m.gamma[2] - 0.833984).abs() < 1e-6);
        assert!((m.offset_hdr[1] - 1e-5).abs() < 1e-9);
        assert_eq!(m.use_base_cg, 1);
        // Cut short anywhere, none.
        for cut in 0..bytes.len() {
            assert!(iso_metadata(&bytes[..cut]).is_none());
        }
        // Its base the HDR picture: none.
        let mut backward = bytes.clone();
        backward[4] |= 0x04;
        assert!(iso_metadata(&backward).is_none());
    }

    #[test]
    fn apples_map_becomes_isos() {
        // Black no gain, white all of it, between in the log domain.
        assert_eq!(apple_to_iso(0, 8.0), 0);
        assert_eq!(apple_to_iso(255, 8.0), 255);
        let g = ((0.5f64 + 0.099) / 1.099).powf(1.0 / 0.45);
        assert_eq!(apple_to_iso(128, 8.0) as f64, ((1.0 + 7.0 * ((128.0 / 255.0 + 0.099f64) / 1.099).powf(1.0 / 0.45)).log2() / 3.0 * 255.0).round());
        assert!(g > 0.0);
        assert!((apple_maker_stops(0.5, 0.005) - 1.7).abs() < 1e-9);
        assert!((apple_maker_stops(1.5, 0.5) - 2.1515).abs() < 1e-9);
        assert_eq!(apple_maker_stops(1.5, 20.0), 0.0);
        assert_eq!(xmp_number(b"<HDRGainMap:HDRGainMapHeadroom>6.9</HDRGainMap:HDRGainMapHeadroom>", "HDRGainMap:HDRGainMapHeadroom"), Some(6.9));
        assert_eq!(xmp_number(b"HDRGainMap:HDRGainMapHeadroom=\"2.5\"", "HDRGainMap:HDRGainMapHeadroom"), Some(2.5));
    }

    #[test]
    fn a_map_attached_is_read_back_by_libultrahdr() {
        // A base and a map made here; the map back within 2 of 255.
        let (w, h) = (96u32, 64u32);
        let base = image::RgbImage::from_fn(w, h, |x, y| image::Rgb([(x * 2) as u8, (y * 3) as u8, 128]));
        let mut jpeg = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut jpeg, 92).encode_image(&base).unwrap();
        let pixels: Vec<u8> = (0..w * h).map(|i| ((i % w) * 255 / (w - 1)) as u8).collect();
        let map = Map {
            width: w,
            height: h,
            channels: 1,
            pixels: pixels.clone(),
            meta: Meta {
                max_content_boost: [4.0; 3],
                min_content_boost: [1.0; 3],
                gamma: [1.0; 3],
                offset_sdr: [0.0; 3],
                offset_hdr: [0.0; 3],
                hdr_capacity_min: 1.0,
                hdr_capacity_max: 4.0,
                use_base_cg: 1,
            },
            kind: "apple",
        };
        let ultra = attach(&jpeg, &map).unwrap();
        let (back, meta, primary) = probe(&ultra).unwrap();
        assert_eq!(meta, map.meta);
        let back = back.to_luma8();
        assert_eq!(back.dimensions(), (w, h));
        let worst = back.as_raw().iter().zip(&pixels).map(|(a, b)| (*a as i32 - *b as i32).abs()).max().unwrap();
        assert!(worst <= 2, "the map back within {worst} of 255");
        // The base: the very picture, its scan as it was.
        let decoded = image::load_from_memory(&primary).unwrap().to_rgb8();
        assert_eq!(decoded, image::load_from_memory(&jpeg).unwrap().to_rgb8());
    }
}
