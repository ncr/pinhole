#include "colour.h"

#include <QColorSpace>
#include <QHash>
#include <QtConcurrent/QtConcurrentMap>

#include <libheif/heif.h>
#include <lcms2.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace {

// Linear light from 0 to 1, in this many steps, to 8-bit sRGB: fine enough
// that no code is missed where sRGB is steepest, near black.
constexpr int kSteps = 16384;

const uint8_t *encodeTable() {
    static const std::array<uint8_t, kSteps> table = [] {
        std::array<uint8_t, kSteps> t{};
        for (int i = 0; i < kSteps; ++i) {
            const double x = double(i) / (kSteps - 1);
            const double e = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
            t[size_t(i)] = uint8_t(std::lround(std::clamp(e, 0.0, 1.0) * 255));
        }
        return t;
    }();
    return table.data();
}

// How one profile's pixels become sRGB.
struct Conversion {
    enum class Kind { None, Matrix, Lcms } kind = Kind::None;
    // Matrix: each channel's code to linear light, then the profile's
    // primaries to sRGB's (both adapted to D50, as ICC keeps them).
    float linear[3][256];
    float matrix[9];
    // Lcms: for the bytes in the order of RGBA, and of BGRA.
    cmsHTRANSFORM rgba = nullptr;
    cmsHTRANSFORM bgra = nullptr;

    Conversion() = default;
    Conversion(const Conversion &) = delete;
    Conversion &operator=(const Conversion &) = delete;
    ~Conversion() {
        if (rgba)
            cmsDeleteTransform(rgba);
        if (bgra)
            cmsDeleteTransform(bgra);
    }
};

using Matrix3 = std::array<std::array<double, 3>, 3>;

// sRGB's primaries adapted to D50, as lcms2 has them in its sRGB profile,
// inverted: from PCS XYZ to linear sRGB.
const Matrix3 &fromXyz() {
    static const Matrix3 inverse = [] {
        cmsHPROFILE srgb = cmsCreate_sRGBProfile();
        const auto *r = static_cast<const cmsCIEXYZ *>(cmsReadTag(srgb, cmsSigRedColorantTag));
        const auto *g = static_cast<const cmsCIEXYZ *>(cmsReadTag(srgb, cmsSigGreenColorantTag));
        const auto *b = static_cast<const cmsCIEXYZ *>(cmsReadTag(srgb, cmsSigBlueColorantTag));
        const Matrix3 m = {{{r->X, g->X, b->X}, {r->Y, g->Y, b->Y}, {r->Z, g->Z, b->Z}}};
        cmsCloseProfile(srgb);
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        Matrix3 inv;
        inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
        inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
        inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
        inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
        inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
        inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
        inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
        inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
        inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
        return inv;
    }();
    return inverse;
}

// Three curves and a matrix: the tables, or false if it says too little.
bool matrixOf(cmsHPROFILE profile, Conversion &c) {
    const cmsTagSignature colorants[3] = {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag};
    const cmsTagSignature curves[3] = {cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
    const cmsCIEXYZ *xyz[3];
    for (int k = 0; k < 3; ++k) {
        xyz[k] = static_cast<const cmsCIEXYZ *>(cmsReadTag(profile, colorants[k]));
        auto *curve = static_cast<cmsToneCurve *>(cmsReadTag(profile, curves[k]));
        if (!xyz[k] || !curve)
            return false;
        for (int v = 0; v < 256; ++v)
            c.linear[k][v] = cmsEvalToneCurveFloat(curve, float(v) / 255);
    }
    const Matrix3 &to = fromXyz();
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            const double into[3] = {xyz[col]->X, xyz[col]->Y, xyz[col]->Z};
            c.matrix[row * 3 + col] = float(to[row][0] * into[0] + to[row][1] * into[1] + to[row][2] * into[2]);
        }
    return true;
}

// sRGB itself, to within a code: nothing to do.
bool isSrgb(const Conversion &c) {
    for (int k = 0; k < 9; ++k)
        if (std::abs(c.matrix[k] - (k % 4 == 0 ? 1.f : 0.f)) > 2e-3f)
            return false;
    const uint8_t *encode = encodeTable();
    for (int k = 0; k < 3; ++k)
        for (int v = 0; v < 256; ++v)
            if (std::abs(int(encode[int(std::clamp(c.linear[k][v], 0.f, 1.f) * (kSteps - 1) + 0.5f)]) - v) > 1)
                return false;
    return true;
}

std::unique_ptr<Conversion> conversionOf(const QByteArray &icc) {
    auto c = std::make_unique<Conversion>();
    cmsHPROFILE profile = cmsOpenProfileFromMem(icc.constData(), cmsUInt32Number(icc.size()));
    if (!profile)
        return c;
    if (cmsGetColorSpace(profile) == cmsSigRgbData) {
        if (cmsIsMatrixShaper(profile) && matrixOf(profile, *c)) {
            c->kind = isSrgb(*c) ? Conversion::Kind::None : Conversion::Kind::Matrix;
        } else {
            cmsHPROFILE srgb = cmsCreate_sRGBProfile();
            c->rgba = cmsCreateTransform(profile, TYPE_RGBA_8, srgb, TYPE_RGBA_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
            c->bgra = cmsCreateTransform(profile, TYPE_BGRA_8, srgb, TYPE_BGRA_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
            cmsCloseProfile(srgb);
            if (c->rgba && c->bgra)
                c->kind = Conversion::Kind::Lcms;
        }
    }
    cmsCloseProfile(profile);
    return c;
}

// The conversion for a profile, made once on each thread: Display P3 is
// nearly every one asked for. A few dozen kept at most.
const Conversion &conversionFor(const QByteArray &icc) {
    thread_local QHash<QByteArray, std::shared_ptr<const Conversion>> made;
    if (const auto found = made.constFind(icc); found != made.constEnd())
        return **found;
    if (made.size() >= 32)
        made.clear();
    return **made.insert(icc, conversionOf(icc));
}

// Rows [from, to) of 4-byte pixels, red at byte R and blue at byte B,
// green between, the fourth left as it is.
template <int R, int B>
void convertRows(const Conversion &c, uchar *bits, qsizetype stride, int width, int from, int to) {
    const uint8_t *encode = encodeTable();
    const float *lr = c.linear[0], *lg = c.linear[1], *lb = c.linear[2];
    const float *m = c.matrix;
    constexpr float top = kSteps - 1;
    for (int y = from; y < to; ++y) {
        uchar *p = bits + stride * y;
        int x = 0;
#if defined(__ARM_NEON)
        const float32x4_t zero = vdupq_n_f32(0), most = vdupq_n_f32(top);
        const float32x4_t m0 = vdupq_n_f32(m[0] * top), m1 = vdupq_n_f32(m[1] * top), m2 = vdupq_n_f32(m[2] * top);
        const float32x4_t m3 = vdupq_n_f32(m[3] * top), m4 = vdupq_n_f32(m[4] * top), m5 = vdupq_n_f32(m[5] * top);
        const float32x4_t m6 = vdupq_n_f32(m[6] * top), m7 = vdupq_n_f32(m[7] * top), m8 = vdupq_n_f32(m[8] * top);
        // Four at a time: the look-ups one by one, the matrix at once.
        for (; x + 4 <= width; x += 4, p += 16) {
            float32x4_t r = vdupq_n_f32(0), g = r, b = r;
            r = vld1q_lane_f32(lr + p[R], r, 0);
            r = vld1q_lane_f32(lr + p[R + 4], r, 1);
            r = vld1q_lane_f32(lr + p[R + 8], r, 2);
            r = vld1q_lane_f32(lr + p[R + 12], r, 3);
            g = vld1q_lane_f32(lg + p[1], g, 0);
            g = vld1q_lane_f32(lg + p[5], g, 1);
            g = vld1q_lane_f32(lg + p[9], g, 2);
            g = vld1q_lane_f32(lg + p[13], g, 3);
            b = vld1q_lane_f32(lb + p[B], b, 0);
            b = vld1q_lane_f32(lb + p[B + 4], b, 1);
            b = vld1q_lane_f32(lb + p[B + 8], b, 2);
            b = vld1q_lane_f32(lb + p[B + 12], b, 3);
            const uint32x4_t outR = vcvtnq_u32_f32(vminq_f32(vmaxq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(m0, r), m1, g), m2, b), zero), most));
            const uint32x4_t outG = vcvtnq_u32_f32(vminq_f32(vmaxq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(m3, r), m4, g), m5, b), zero), most));
            const uint32x4_t outB = vcvtnq_u32_f32(vminq_f32(vmaxq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(m6, r), m7, g), m8, b), zero), most));
            p[R] = encode[vgetq_lane_u32(outR, 0)];
            p[R + 4] = encode[vgetq_lane_u32(outR, 1)];
            p[R + 8] = encode[vgetq_lane_u32(outR, 2)];
            p[R + 12] = encode[vgetq_lane_u32(outR, 3)];
            p[1] = encode[vgetq_lane_u32(outG, 0)];
            p[5] = encode[vgetq_lane_u32(outG, 1)];
            p[9] = encode[vgetq_lane_u32(outG, 2)];
            p[13] = encode[vgetq_lane_u32(outG, 3)];
            p[B] = encode[vgetq_lane_u32(outB, 0)];
            p[B + 4] = encode[vgetq_lane_u32(outB, 1)];
            p[B + 8] = encode[vgetq_lane_u32(outB, 2)];
            p[B + 12] = encode[vgetq_lane_u32(outB, 3)];
        }
#endif
        for (; x < width; ++x, p += 4) {
            const float r = lr[p[R]], g = lg[p[1]], b = lb[p[B]];
            const auto out = [&](int row) {
                const float v = (m[row * 3] * r + m[row * 3 + 1] * g + m[row * 3 + 2] * b) * top;
                return encode[int(std::clamp(v, 0.f, top) + 0.5f)];
            };
            const uint8_t outR = out(0), outG = out(1), outB = out(2);
            p[R] = outR;
            p[1] = outG;
            p[B] = outB;
        }
    }
}

void convert(const Conversion &c, QImage &image) {
    const bool bgra = image.format() == QImage::Format_RGB32 || image.format() == QImage::Format_ARGB32;
    uchar *bits = image.bits();
    const qsizetype stride = image.bytesPerLine();
    const int width = image.width();
    auto rows = [&](int from, int to) {
        if (c.kind == Conversion::Kind::Lcms)
            cmsDoTransformLineStride(bgra ? c.bgra : c.rgba, bits + stride * from, bits + stride * from, cmsUInt32Number(width),
                                     cmsUInt32Number(to - from), cmsUInt32Number(stride), cmsUInt32Number(stride), 0, 0);
        else if (bgra)
            convertRows<2, 0>(c, bits, stride, width, from, to);
        else
            convertRows<0, 2>(c, bits, stride, width, from, to);
    };
    // Big ones (the viewer's full size) on every core.
    const int height = image.height();
    if (qsizetype(width) * height < (1 << 20)) {
        rows(0, height);
        return;
    }
    // In bands of 64 rows, small enough that the slower cores take fewer.
    std::vector<int> starts;
    for (int y = 0; y < height; y += 64)
        starts.push_back(y);
    QtConcurrent::blockingMap(starts, [&](int from) { rows(from, std::min(height, from + 64)); });
}

} // namespace

QByteArray iccOfNclx(const heif_color_profile_nclx *nclx) {
    if (!nclx)
        return {};
    cmsToneCurve *curve = nullptr;
    bool srgbCurve = false;
    switch (nclx->transfer_characteristics) {
    case heif_transfer_characteristic_linear:
        curve = cmsBuildGamma(nullptr, 1.0);
        break;
    case heif_transfer_characteristic_ITU_R_BT_470_6_System_M:
        curve = cmsBuildGamma(nullptr, 2.2);
        break;
    case heif_transfer_characteristic_ITU_R_BT_470_6_System_B_G:
        curve = cmsBuildGamma(nullptr, 2.8);
        break;
    // HDR: tone mapped in H2, as it is until then.
    case heif_transfer_characteristic_ITU_R_BT_2100_0_PQ:
    case heif_transfer_characteristic_SMPTE_ST_428_1:
    case heif_transfer_characteristic_ITU_R_BT_2100_0_HLG:
        return {};
    default: {
        // sRGB's, and BT.709's and its kin too: an iPhone's edit says BT.709
        // (1) and holds the very codes of its Display P3 medium, which has
        // sRGB's curve (docs/hdr.md).
        const double parameters[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
        curve = cmsBuildParametricToneCurve(nullptr, 4, parameters);
        srgbCurve = true;
    }
    }
    const bool srgbPrimaries = nclx->color_primaries == heif_color_primaries_ITU_R_BT_709_5
                            || nclx->color_primaries == heif_color_primaries_unspecified;
    QByteArray icc;
    if (curve && !(srgbCurve && srgbPrimaries)) {
        const cmsCIExyY white{nclx->color_primary_white_x, nclx->color_primary_white_y, 1.0};
        const cmsCIExyYTRIPLE primaries{{nclx->color_primary_red_x, nclx->color_primary_red_y, 1.0},
                                        {nclx->color_primary_green_x, nclx->color_primary_green_y, 1.0},
                                        {nclx->color_primary_blue_x, nclx->color_primary_blue_y, 1.0}};
        cmsToneCurve *curves[3] = {curve, curve, curve};
        if (cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves)) {
            cmsUInt32Number size = 0;
            if (cmsSaveProfileToMem(profile, nullptr, &size) && size > 0) {
                icc.resize(qsizetype(size));
                if (!cmsSaveProfileToMem(profile, icc.data(), &size))
                    icc.clear();
            }
            cmsCloseProfile(profile);
        }
    }
    if (curve)
        cmsFreeToneCurve(curve);
    return icc;
}

void toSrgb(QImage &image, const QByteArray &icc) {
    if (image.isNull() || icc.isEmpty())
        return;
    const Conversion &c = conversionFor(icc);
    if (c.kind == Conversion::Kind::None)
        return;
    switch (image.format()) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
        break;
    case QImage::Format_ARGB32_Premultiplied:
        image.convertTo(QImage::Format_ARGB32);
        break;
    default:
        image.convertTo(image.hasAlphaChannel() ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
    }
    convert(c, image);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
}

void toSrgb(QImage &image) {
    const QColorSpace space = image.colorSpace();
    if (space.isValid() && space != QColorSpace(QColorSpace::SRgb))
        toSrgb(image, space.iccProfile());
}
