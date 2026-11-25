//! Colours as the window shows them: sRGB (docs/hdr.md). A thumbnail's
//! pixels are in its profile's colours (an iPhone's Display P3, a camera's
//! own), and the window makes them sRGB as it decodes them (app/src/colour.h).
//! Its average colour, which the grid draws until it has the thumbnail and
//! Mosaic picks its tiles by, is made sRGB here the same way: the average of
//! its codes, through the profile, to sRGB. Profiles of three curves and a
//! matrix, which photos' are; any other is taken as sRGB, as untagged
//! pixels are.

use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

/// sRGB's primaries adapted to D50 (lcms2's sRGB profile has these), as
/// ICC keeps every profile's.
const SRGB_D50: [[f64; 3]; 3] = [
    [0.4360747, 0.3850649, 0.1430804],
    [0.2225045, 0.7168786, 0.0606169],
    [0.0139322, 0.0971045, 0.7141733],
];

/// One channel's curve, from its code (0 to 1) to linear light.
#[derive(Debug, Clone)]
enum Curve {
    Gamma(f64),
    Table(Vec<f64>),
    /// ICC's parametric curves, types 0 to 4: g, a, b, c, d, e, f.
    Parametric(u16, [f64; 7]),
}

impl Curve {
    fn eval(&self, x: f64) -> f64 {
        match self {
            Curve::Gamma(g) => x.max(0.0).powf(*g),
            Curve::Table(t) => {
                let at = x.clamp(0.0, 1.0) * (t.len() - 1) as f64;
                let i = (at.floor() as usize).min(t.len() - 2);
                t[i] + (t[i + 1] - t[i]) * (at - i as f64)
            }
            Curve::Parametric(kind, [g, a, b, c, d, e, f]) => match kind {
                0 => x.max(0.0).powf(*g),
                1 => if x >= -b / a { (a * x + b).max(0.0).powf(*g) } else { 0.0 },
                2 => if x >= -b / a { (a * x + b).max(0.0).powf(*g) + c } else { *c },
                3 => if x >= *d { (a * x + b).max(0.0).powf(*g) } else { c * x },
                _ => if x >= *d { (a * x + b).max(0.0).powf(*g) + e } else { c * x + f },
            },
        }
    }
}

/// An RGB profile of three curves and a matrix: what a photo's codes are.
#[derive(Debug, Clone)]
pub struct Profile {
    curves: [Curve; 3],
    /// From the profile's linear light to sRGB's.
    to_srgb: [[f64; 3]; 3],
}

fn be32(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_be_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

fn be16(b: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_be_bytes(b.get(at..at + 2)?.try_into().ok()?))
}

fn s15f16(b: &[u8], at: usize) -> Option<f64> {
    Some(be32(b, at)? as i32 as f64 / 65536.0)
}

fn invert(m: &[[f64; 3]; 3]) -> [[f64; 3]; 3] {
    let det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    [
        [
            (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det,
            (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det,
            (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det,
        ],
        [
            (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det,
            (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det,
            (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det,
        ],
        [
            (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det,
            (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det,
            (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det,
        ],
    ]
}

fn srgb_encode(x: f64) -> f64 {
    let x = x.clamp(0.0, 1.0);
    if x <= 0.0031308 {
        12.92 * x
    } else {
        1.055 * x.powf(1.0 / 2.4) - 0.055
    }
}

impl Profile {
    /// The profile an ICC describes, or None for one not of RGB, not of
    /// curves and a matrix, or not readable.
    pub fn parse(icc: &[u8]) -> Option<Profile> {
        if icc.len() < 132 || icc.get(16..20)? != b"RGB " || icc.get(20..24)? != b"XYZ " {
            return None;
        }
        let tag = |want: &[u8; 4]| -> Option<&[u8]> {
            let count = be32(icc, 128)? as usize;
            (0..count.min(1000)).find_map(|k| {
                let at = 132 + 12 * k;
                if icc.get(at..at + 4)? != want {
                    return None;
                }
                let (offset, size) = (be32(icc, at + 4)? as usize, be32(icc, at + 8)? as usize);
                icc.get(offset..offset.checked_add(size)?)
            })
        };
        let xyz = |want: &[u8; 4]| -> Option<[f64; 3]> {
            let t = tag(want)?;
            if t.get(0..4)? != b"XYZ " {
                return None;
            }
            Some([s15f16(t, 8)?, s15f16(t, 12)?, s15f16(t, 16)?])
        };
        let curve = |want: &[u8; 4]| -> Option<Curve> {
            let t = tag(want)?;
            match t.get(0..4)? {
                b"curv" => {
                    let n = be32(t, 8)? as usize;
                    match n {
                        0 => Some(Curve::Gamma(1.0)),
                        1 => Some(Curve::Gamma(be16(t, 12)? as f64 / 256.0)),
                        _ => Some(Curve::Table((0..n).map(|i| Some(be16(t, 12 + 2 * i)? as f64 / 65535.0)).collect::<Option<_>>()?)),
                    }
                }
                b"para" => {
                    let kind = be16(t, 8)?;
                    let count = [1, 3, 4, 5, 7].get(kind as usize)?;
                    let mut p = [0.0; 7];
                    for (i, v) in p.iter_mut().enumerate().take(*count) {
                        *v = s15f16(t, 12 + 4 * i)?;
                    }
                    Some(Curve::Parametric(kind, p))
                }
                _ => None,
            }
        };
        let (r, g, b) = (xyz(b"rXYZ")?, xyz(b"gXYZ")?, xyz(b"bXYZ")?);
        let from = [[r[0], g[0], b[0]], [r[1], g[1], b[1]], [r[2], g[2], b[2]]];
        let to = invert(&SRGB_D50);
        let mut to_srgb = [[0.0; 3]; 3];
        for (row, out) in to_srgb.iter_mut().enumerate() {
            for (col, v) in out.iter_mut().enumerate() {
                *v = (0..3).map(|k| to[row][k] * from[k][col]).sum();
            }
        }
        Some(Profile { curves: [curve(b"rTRC")?, curve(b"gTRC")?, curve(b"bTRC")?], to_srgb })
    }

    /// A colour of the profile's codes, in sRGB's: what lies outside sRGB
    /// clipped to its edge.
    pub fn to_srgb(&self, rgb: [u8; 3]) -> [u8; 3] {
        let linear: Vec<f64> = (0..3).map(|k| self.curves[k].eval(rgb[k] as f64 / 255.0)).collect();
        let mut out = [0u8; 3];
        for (row, o) in out.iter_mut().enumerate() {
            let v: f64 = (0..3).map(|k| self.to_srgb[row][k] * linear[k]).sum();
            *o = (srgb_encode(v) * 255.0).round() as u8;
        }
        out
    }
}

/// The ICC profile in a JPEG's APP2 markers, its parts put together; None
/// if it has none.
pub fn jpeg_icc(bytes: &[u8]) -> Option<Vec<u8>> {
    let mut reader = std::io::Cursor::new(bytes);
    read_icc(&mut reader).ok().flatten()
}

/// The same, of a JPEG file: only its markers read, up to the image.
pub fn jpeg_icc_of_file(path: &Path) -> std::io::Result<Option<Vec<u8>>> {
    let mut file = std::io::BufReader::with_capacity(16 * 1024, std::fs::File::open(path)?);
    read_icc(&mut file)
}

fn read_icc(r: &mut (impl Read + Seek)) -> std::io::Result<Option<Vec<u8>>> {
    let mut two = [0u8; 2];
    r.read_exact(&mut two)?;
    if two != [0xff, 0xd8] {
        return Ok(None);
    }
    let mut parts: Vec<(u8, Vec<u8>)> = Vec::new();
    loop {
        r.read_exact(&mut two)?;
        if two[0] != 0xff {
            break;
        }
        let marker = two[1];
        // The image's own data: no more markers of these.
        if marker == 0xda || marker == 0xd9 {
            break;
        }
        if marker == 0xff || (0xd0..=0xd7).contains(&marker) || marker == 0x01 {
            continue;
        }
        r.read_exact(&mut two)?;
        let length = u16::from_be_bytes(two) as usize;
        if length < 2 {
            break;
        }
        if marker == 0xe2 && length >= 16 {
            let mut data = vec![0u8; length - 2];
            r.read_exact(&mut data)?;
            if data.starts_with(b"ICC_PROFILE\0") {
                parts.push((data[12], data[14..].to_vec()));
            }
        } else {
            r.seek(SeekFrom::Current(length as i64 - 2))?;
        }
    }
    if parts.is_empty() {
        return Ok(None);
    }
    parts.sort_by_key(|p| p.0);
    Ok(Some(parts.into_iter().flat_map(|p| p.1).collect()))
}

/// An average colour, 0xRRGGBB, of codes in `icc`'s profile, in sRGB's.
pub fn average_in_srgb(colour: u32, icc: Option<&[u8]>) -> u32 {
    let Some(profile) = icc.and_then(Profile::parse) else { return colour };
    let [r, g, b] = profile.to_srgb([(colour >> 16) as u8, (colour >> 8) as u8, colour as u8]);
    (r as u32) << 16 | (g as u32) << 8 | b as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Display P3 as lcms2 writes it (primaries 12, sRGB's curve).
    fn display_p3() -> Vec<u8> {
        std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../app/tests/data/display-p3.icc")).unwrap()
    }

    #[test]
    fn display_p3_green_is_srgb_green_clipped_and_grey_stays() {
        let p3 = Profile::parse(&display_p3()).unwrap();
        // P3's pure green is outside sRGB: clipped to sRGB's green, nearly.
        let [r, g, b] = p3.to_srgb([0, 255, 0]);
        assert!(r == 0 && g == 255 && b <= 1, "{r} {g} {b}");
        // P3 red (255, 0, 0) is sRGB's (255, 0, 0) and beyond; a mid red
        // is redder in sRGB's codes than in P3's.
        let [r, g, b] = p3.to_srgb([200, 60, 50]);
        assert!(r > 200 && g < 60 && b < 50, "{r} {g} {b}");
        for v in [0u8, 1, 17, 128, 254, 255] {
            let out = p3.to_srgb([v, v, v]);
            assert!(out.iter().all(|&c| (c as i32 - v as i32).abs() <= 1), "{v}: {out:?}");
        }
    }

    #[test]
    fn real_profiles_convert_as_lcms2_does() {
        // lcms2's own (cmsDoTransform, relative colorimetric), as the
        // window's references are: an iPhone's Display P3, and the "Camera
        // RGB Profile" older iPhones wrote.
        let cases: [(&str, [([u8; 3], [u8; 3]); 7]); 2] = [
            ("display-p3.icc", [
                ([200, 30, 40], [218, 0, 27]),
                ([0, 255, 0], [0, 255, 0]),
                ([30, 160, 60], [0, 163, 41]),
                ([250, 200, 20], [255, 198, 0]),
                ([20, 60, 200], [0, 61, 208]),
                ([128, 128, 128], [128, 128, 128]),
                ([90, 70, 50], [94, 69, 47]),
            ]),
            ("camera-rgb.icc", [
                ([200, 30, 40], [203, 40, 34]),
                ([0, 255, 0], [0, 249, 0]),
                ([30, 160, 60], [0, 158, 57]),
                ([250, 200, 20], [251, 200, 2]),
                ([20, 60, 200], [14, 68, 202]),
                ([128, 128, 128], [129, 129, 129]),
                ([90, 70, 50], [90, 68, 46]),
            ]),
        ];
        for (name, pairs) in cases {
            let icc = std::fs::read(format!("{}/../app/tests/data/{name}", env!("CARGO_MANIFEST_DIR"))).unwrap();
            let profile = Profile::parse(&icc).unwrap();
            for (code, lcms2) in pairs {
                let ours = profile.to_srgb(code);
                assert!((0..3).all(|k| (ours[k] as i32 - lcms2[k] as i32).abs() <= 1), "{name} {code:?}: {ours:?}, lcms2 {lcms2:?}");
            }
        }
    }

    #[test]
    fn untagged_and_unreadable_stay_as_they_are() {
        assert_eq!(average_in_srgb(0x40c020, None), 0x40c020);
        assert_eq!(average_in_srgb(0x40c020, Some(b"not a profile")), 0x40c020);
        assert_ne!(average_in_srgb(0x40c020, Some(&display_p3())), 0x40c020);
    }

    #[test]
    fn a_jpeg_profile_split_over_markers_comes_back_whole() {
        let icc = display_p3();
        let (first, second) = icc.split_at(100);
        let mut jpeg = vec![0xff, 0xd8];
        // An Exif APP1 first, skipped.
        jpeg.extend([0xff, 0xe1, 0x00, 0x08, b'E', b'x', b'i', b'f', 0, 0]);
        for (n, part) in [(1u8, first), (2, second)] {
            let length = (2 + 14 + part.len()) as u16;
            jpeg.extend([0xff, 0xe2]);
            jpeg.extend(length.to_be_bytes());
            jpeg.extend(b"ICC_PROFILE\0");
            jpeg.extend([n, 2]);
            jpeg.extend(part);
        }
        jpeg.extend([0xff, 0xda, 0x00, 0x02]);
        assert_eq!(jpeg_icc(&jpeg).as_deref(), Some(&icc[..]));
        assert_eq!(jpeg_icc(&[0xff, 0xd8, 0xff, 0xda, 0, 2]), None);
    }
}
