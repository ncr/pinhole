#pragma once

#include <QByteArray>
#include <QImage>

struct heif_color_profile_nclx;

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
// for sRGB, and for PQ and HLG, which are HDR (H2).
QByteArray iccOfNclx(const heif_color_profile_nclx *nclx);

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
