use std::{path::Path, fs};
use zune_core::{bytestream::ZCursor, options::DecoderOptions};

fn decode(path: &Path) -> (usize, usize, Vec<u8>) {
    let bytes = fs::read(path).unwrap();
    let mut decoder = zune_jpeg::JpegDecoder::new_with_options(ZCursor::new(&bytes), DecoderOptions::default());
    let pixels = decoder.decode().unwrap();
    let (width, height) = decoder.dimensions().unwrap();
    (width, height, pixels)
}

fn main() {
    simd_packing();
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let (width, height, pixels) = decode(&root.join("gray-ramp.jpg"));
    let channels = pixels.len() / (width * height);
    let worst = pixels.iter().enumerate().map(|(i, &actual)| {
        let x = (i / channels) % width;
        (actual as i32 - (x * 255 / (width - 1)) as i32).abs()
    }).max().unwrap();
    assert!(worst <= 2, "JPEG ramp: max error {worst}");
    println!("JPEG ramp {width}x{height}x{channels}: max error {worst}");
    if let Some(destination) = std::env::args_os().nth(1) {
        let destination = Path::new(&destination);
        fs::create_dir_all(destination).unwrap();
        let mut fixtures: Vec<_> = fs::read_dir(root.join("fixtures")).unwrap().map(|p| p.unwrap().path()).collect();
        fixtures.push(root.join("gray-ramp.jpg"));
        fixtures.sort();
        for fixture in fixtures {
            let (w, h, bytes) = decode(&fixture);
            let name = fixture.file_stem().unwrap().to_string_lossy();
            fs::write(destination.join(format!("{name}-{w}x{h}-{}.raw", bytes.len() / (w*h))), bytes).unwrap();
        }
    }
}

// Direct regression for the compiler primitive, independent of zune-jpeg.
#[cfg(target_arch = "x86_64")]
fn simd_packing() {
    use std::arch::x86_64::*;
    unsafe {
        let a: [i32; 4] = [-50000, -32768, -1, 32767];
        let b: [i32; 4] = [32768, 65535, 1, 12345];
        let aa = _mm_loadu_si128(a.as_ptr().cast());
        let bb = _mm_loadu_si128(b.as_ptr().cast());
        let signed: [i16; 8] = std::mem::transmute(_mm_packs_epi32(aa, bb));
        assert_eq!(signed, [-32768, -32768, -1, 32767, 32767, 32767, 1, 12345]);
        let a: [i16; 8] = [-300, -128, -1, 0, 1, 127, 128, 300];
        let b: [i16; 8] = [255, 256, 19, 20, 21, 22, 23, 24];
        let aa = _mm_loadu_si128(a.as_ptr().cast());
        let bb = _mm_loadu_si128(b.as_ptr().cast());
        let signed: [i8; 16] = std::mem::transmute(_mm_packs_epi16(aa, bb));
        assert_eq!(signed, [-128, -128, -1, 0, 1, 127, 127, 127, 127, 127, 19, 20, 21, 22, 23, 24]);
        let unsigned: [u8; 16] = std::mem::transmute(_mm_packus_epi16(aa, bb));
        assert_eq!(unsigned, [0, 0, 0, 0, 1, 127, 128, 255, 255, 255, 19, 20, 21, 22, 23, 24]);
        if is_x86_feature_detected!("sse4.1") { packing_sse41(); }
        if is_x86_feature_detected!("avx2") { packing_avx2(); }
        if is_x86_feature_detected!("avx512bw") { packing_avx512(); }
    }
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable="sse4.1")]
unsafe fn packing_sse41() {
    use std::arch::x86_64::*;
    let a: [i32; 4] = [-99999, -1, 0, 32768];
    let b: [i32; 4] = [65535, 65536, 17, 23];
    let result: [u16; 8] = std::mem::transmute(_mm_packus_epi32(
        _mm_loadu_si128(a.as_ptr().cast()), _mm_loadu_si128(b.as_ptr().cast())));
    assert_eq!(result, [0, 0, 0, 32768, 65535, 65535, 17, 23]);
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable="avx2")]
unsafe fn packing_avx2() {
    use std::arch::x86_64::*;
    let a: [i32; 8] = [-99999, -32768, 1, 32767, 32768, 65535, -1, 25];
    let b: [i32; 8] = [10, 20, 30, 40, 50, 60, 70, 80];
    let aa = _mm256_loadu_si256(a.as_ptr().cast());
    let bb = _mm256_loadu_si256(b.as_ptr().cast());
    let signed: [i16; 16] = std::mem::transmute(_mm256_packs_epi32(aa, bb));
    assert_eq!(signed, [-32768, -32768, 1, 32767, 10, 20, 30, 40, 32767, 32767, -1, 25, 50, 60, 70, 80]);
    let unsigned: [u16; 16] = std::mem::transmute(_mm256_packus_epi32(aa, bb));
    assert_eq!(unsigned, [0, 0, 1, 32767, 10, 20, 30, 40, 32768, 65535, 0, 25, 50, 60, 70, 80]);
    let a: [i16; 16] = [-999, -128, -1, 0, 1, 127, 128, 999, 10,20,30,40,50,60,70,80];
    let b: [i16; 16] = [11,22,33,44,55,66,77,88, -999,-128,-1,0,1,127,128,999];
    let aa = _mm256_loadu_si256(a.as_ptr().cast());
    let bb = _mm256_loadu_si256(b.as_ptr().cast());
    let signed: [i8; 32] = std::mem::transmute(_mm256_packs_epi16(aa, bb));
    assert_eq!(signed, [-128,-128,-1,0,1,127,127,127,11,22,33,44,55,66,77,88,10,20,30,40,50,60,70,80,-128,-128,-1,0,1,127,127,127]);
    let unsigned: [u8; 32] = std::mem::transmute(_mm256_packus_epi16(aa, bb));
    assert_eq!(unsigned, [0,0,0,0,1,127,128,255,11,22,33,44,55,66,77,88,10,20,30,40,50,60,70,80,0,0,0,0,1,127,128,255]);
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable="avx512bw")]
unsafe fn packing_avx512() {
    use std::arch::x86_64::*;
    let a: [i32; 16] = std::array::from_fn(|i| (i as i32 - 8) * 10000);
    let b: [i32; 16] = std::array::from_fn(|i| 99999 - i as i32 * 10000);
    let aa = _mm512_loadu_si512(a.as_ptr().cast());
    let bb = _mm512_loadu_si512(b.as_ptr().cast());
    let signed: [i16; 32] = std::mem::transmute(_mm512_packs_epi32(aa, bb));
    let unsigned: [u16; 32] = std::mem::transmute(_mm512_packus_epi32(aa, bb));
    for lane in 0..4 {
        for input in 0..2 {
            for element in 0..4 {
                let source = if input == 0 { a[lane*4+element] } else { b[lane*4+element] };
                assert_eq!(signed[lane*8+input*4+element], source.clamp(-32768, 32767) as i16);
                assert_eq!(unsigned[lane*8+input*4+element], source.clamp(0, 65535) as u16);
            }
        }
    }
    let a: [i16; 32] = std::array::from_fn(|i| (i as i16 - 16) * 30);
    let b: [i16; 32] = std::array::from_fn(|i| 999 - i as i16 * 50);
    let aa = _mm512_loadu_si512(a.as_ptr().cast());
    let bb = _mm512_loadu_si512(b.as_ptr().cast());
    let signed: [i8; 64] = std::mem::transmute(_mm512_packs_epi16(aa, bb));
    let unsigned: [u8; 64] = std::mem::transmute(_mm512_packus_epi16(aa, bb));
    for lane in 0..4 {
        for input in 0..2 {
            for element in 0..8 {
                let source = if input == 0 { a[lane*8+element] } else { b[lane*8+element] };
                assert_eq!(signed[lane*16+input*8+element], source.clamp(-128, 127) as i8);
                assert_eq!(unsigned[lane*16+input*8+element], source.clamp(0, 255) as u8);
            }
        }
    }
}
#[cfg(not(target_arch = "x86_64"))]
fn simd_packing() {}
