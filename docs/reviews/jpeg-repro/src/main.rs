use zune_core::{bytestream::ZCursor, options::DecoderOptions};

fn main() {
    // Synthetic 96×64 greyscale JPEG: each row is the same 0..255 ramp.
    let bytes = std::fs::read(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("gray-ramp.jpg")).unwrap();
    let mut failed = false;
    for simd in [true, false] {
        let options = DecoderOptions::default().set_use_unsafe(simd);
        let mut decoder = zune_jpeg::JpegDecoder::new_with_options(ZCursor::new(&bytes), options);
        let pixels = decoder.decode().unwrap();
        let (width, height) = decoder.dimensions().unwrap();
        let channels = pixels.len() / (width * height);
        let mut worst = 0;
        for (i, &actual) in pixels.iter().enumerate() {
            let x = (i / channels) % width;
            let expected = (x * 255 / (width - 1)) as i32;
            worst = worst.max((actual as i32 - expected).abs());
        }
        println!("simd={simd}: {width}x{height}x{channels}, max error={worst}, first pixels={:?}", &pixels[..24]);
        failed |= worst > 2;
    }
    if failed { std::process::exit(1); }
}
