#pragma once

#include <QByteArray>
#include <QImage>

struct heif_color_profile_nclx;
struct heif_image_handle;

// Every photo in the window's colours (docs/hdr.md, H1). The window is
// sRGB; a photo's pixels are in whatever its profile says (an iPhone's
// Display P3, a camera's own), and are made sRGB as they are decoded, on
// the thread that decodes them, never as they are drawn. Relative
// colorimetric: what lies outside sRGB is clipped to its edge. Untagged
// pixels are sRGB already.
//
// lcms2 reads the profiles. The ones photos carry are nearly all three
// curves and a matrix, and those are converted here, a table look-up per
// channel each way and the matrix between: some three times quicker than
// QColorTransform or lcms2's own transform, which convert the rest.

// HEIC's colour description (nclx) as an ICC profile, for toSrgb: empty
// for sRGB, and for PQ and HLG, which are HDR (toneMapToSrgb).
QByteArray iccOfNclx(const heif_color_profile_nclx *nclx);

// HDR pictures in SDR (docs/hdr.md, H2). SDR white is 203 nits (BT.2408);
// an HLG picture is shown on a 1000-nit display with system gamma 1.2.
constexpr double kSdrWhiteNits = 203;
constexpr double kHlgPeakNits = 1000;

// Whether an nclx's curve is PQ or HLG.
bool isHdrTransfer(const heif_color_profile_nclx *nclx);
// A PQ picture's brightest, in nits: its content light level, else its
// mastering display's peak, else 1000.
double pqPeakOf(const heif_image_handle *handle);
// PQ or HLG pixels as libheif decodes them (16-bit RGB or RGBA, `bits`
// of each used), in sRGB: made linear (PQ's EOTF; HLG's inverse OETF and
// its OOTF at 1000 nits), tone mapped to SDR white with BT.2390's EETF on
// the largest of the three (keeping hue), from the nclx's primaries to
// sRGB's, clipped and encoded in 8 bits. `peakNits` is PQ's (pqPeakOf).
// In bands on Qt's thread pool when big.
QImage toneMapToSrgb(const uchar *data, int width, int height, qsizetype stride, int bits, bool alpha,
                     const heif_color_profile_nclx &nclx, double peakNits);
// BT.2390's EETF from a picture whose brightest is `peakNits` to SDR
// white: nits in, linear light out (1 is SDR white).
double eetfToSdr(double nits, double peakNits);
// HLG's inverse OETF: a code from 0 to 1 to scene light from 0 to 1.
double hlgSceneLight(double code);
// Linear light from 0 to 1, in kSrgbSteps steps, to 8-bit sRGB codes.
constexpr int kSrgbSteps = 16384;
const uint8_t *srgbEncodeTable();
// PQ's EOTF: a code from 0 to 1 to nits, and back.
double pqToNits(double code);
double nitsToPq(double nits);

// `image` made sRGB in place, from the ICC profile `icc`. Nothing is done
// for none, sRGB, a profile lcms2 cannot read or one not of RGB. RGB32,
// ARGB32, RGBX8888 and RGBA8888 are converted as they are; any other
// format is first made one of them (premultiplied, not). An image over a
// megapixel is converted in bands on Qt's thread pool. Any thread: a
// thread keeps the conversions it has made, one per profile.
void toSrgb(QImage &image, const QByteArray &icc);

// The same, from the colour space Qt's readers give an image (its ICC
// profile, as it was in the file).
void toSrgb(QImage &image);

// How the stage draws a photo's pixels as they are in the file (docs/hdr.md,
// H3): each channel's code made linear light by a table, then a matrix to
// linear sRGB, which keeps what lies outside sRGB (below 0, above 1). The
// same tables and matrix toSrgb converts by, so that the stage clipped to
// sRGB is the window's photo.
struct StageColour {
    float linear[3][256];
    float matrix[9];
};
// For the profile `icc`: none (sRGB), or three curves and a matrix, true.
// A profile of tables, or one not of RGB, false: the pixels are to be made
// sRGB first (toSrgb), and `colour` is sRGB's.
bool stageColourOf(const QByteArray &icc, StageColour &colour);
