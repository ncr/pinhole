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
    // HDR: tone mapped as it is decoded (toneMapToSrgb).
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

bool isHdrTransfer(const heif_color_profile_nclx *nclx) {
    return nclx && (nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_PQ
                     || nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_HLG);
}

double pqPeakOf(const heif_image_handle *handle) {
    heif_content_light_level light{};
    if (heif_image_handle_get_content_light_level(handle, &light) && light.max_content_light_level > 0)
        return light.max_content_light_level;
    heif_mastering_display_colour_volume volume{};
    heif_decoded_mastering_display_colour_volume decoded{};
    if (heif_image_handle_get_mastering_display_colour_volume(handle, &volume)
        && heif_mastering_display_colour_volume_decode(&volume, &decoded).code == heif_error_Ok
        && decoded.max_display_mastering_luminance > 0)
        return decoded.max_display_mastering_luminance;
    return 1000;
}

namespace {
constexpr double kPqM1 = 2610.0 / 16384, kPqM2 = 2523.0 / 4096 * 128;
constexpr double kPqC1 = 3424.0 / 4096, kPqC2 = 2413.0 / 4096 * 32, kPqC3 = 2392.0 / 4096 * 32;
constexpr double kHlgA = 0.17883277, kHlgB = 0.28466892, kHlgC = 0.55991073;

// HLG's inverse OETF: a code to scene light from 0 to 1.
double hlgToScene(double e) {
    return e <= 0.5 ? e * e / 3 : (std::exp((e - kHlgC) / kHlgA) + kHlgB) / 12;
}

double determinant(const Matrix3 &q) {
    return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0])
         + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
}

// The primaries' RGB to XYZ, from their chromaticities and white.
Matrix3 rgbToXyz(const heif_color_profile_nclx &n) {
    const double xy[3][2] = {{n.color_primary_red_x, n.color_primary_red_y},
                             {n.color_primary_green_x, n.color_primary_green_y},
                             {n.color_primary_blue_x, n.color_primary_blue_y}};
    Matrix3 p;
    for (int c = 0; c < 3; ++c) {
        p[0][c] = xy[c][0] / xy[c][1];
        p[1][c] = 1;
        p[2][c] = (1 - xy[c][0] - xy[c][1]) / xy[c][1];
    }
    const double wx = n.color_primary_white_x, wy = n.color_primary_white_y;
    const double w[3] = {wx / wy, 1, (1 - wx - wy) / wy};
    // Each primary scaled so that the three make the white (Cramer's rule).
    const double det = determinant(p);
    Matrix3 m;
    for (int c = 0; c < 3; ++c) {
        Matrix3 q = p;
        for (int r = 0; r < 3; ++r)
            q[r][c] = w[r];
        const double s = determinant(q) / det;
        for (int r = 0; r < 3; ++r)
            m[r][c] = p[r][c] * s;
    }
    return m;
}
} // namespace

double hlgSceneLight(double code) { return hlgToScene(code); }

const uint8_t *srgbEncodeTable() { return encodeTable(); }

double pqToNits(double code) {
    const double p = std::pow(std::clamp(code, 0.0, 1.0), 1 / kPqM2);
    return 10000 * std::pow(std::max(p - kPqC1, 0.0) / (kPqC2 - kPqC3 * p), 1 / kPqM1);
}

double nitsToPq(double nits) {
    const double y = std::pow(std::clamp(nits / 10000, 0.0, 1.0), kPqM1);
    return std::pow((kPqC1 + kPqC2 * y) / (1 + kPqC3 * y), kPqM2);
}

double eetfToSdr(double nits, double peakNits) {
    if (peakNits <= kSdrWhiteNits)
        return std::min(nits, kSdrWhiteNits) / kSdrWhiteNits;
    // In PQ's codes, the source's range from 0 to its peak, SDR white
    // within it: a Hermite spline from the knee to the peak.
    const double top = nitsToPq(peakNits);
    const double maxLum = nitsToPq(kSdrWhiteNits) / top;
    const double knee = 1.5 * maxLum - 0.5;
    const double e1 = std::min(nitsToPq(nits) / top, 1.0);
    double e2 = e1;
    if (e1 >= knee) {
        const double t = (e1 - knee) / (1 - knee), t2 = t * t, t3 = t2 * t;
        e2 = (2 * t3 - 3 * t2 + 1) * knee + (t3 - 2 * t2 + t) * (1 - knee) + (-2 * t3 + 3 * t2) * maxLum;
    }
    return std::min(pqToNits(e2 * top), kSdrWhiteNits) / kSdrWhiteNits;
}

QImage toneMapToSrgb(const uchar *data, int width, int height, qsizetype stride, int bits, bool alpha,
                     const heif_color_profile_nclx &nclx, double peakNits) {
    if (bits < 1 || bits > 16)
        return {};
    QImage out(width, height, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
    if (out.isNull())
        return {};
    const bool hlg = nclx.transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_HLG;
    const int codes = 1 << bits;
    const double top = codes - 1;
    // Each code made linear: PQ's in nits, HLG's as scene light (0 to 1).
    std::vector<float> linear(static_cast<size_t>(codes));
    for (int c = 0; c < codes; ++c)
        linear[size_t(c)] = float(hlg ? hlgToScene(c / top) : pqToNits(c / top));
    const double peak = hlg ? kHlgPeakNits : peakNits;
    // The EETF over the largest channel's nits, as the factor that makes
    // them linear SDR, in a table from 0 to the peak.
    constexpr int kTable = 4096;
    std::vector<float> factor(kTable + 1);
    for (int i = 0; i <= kTable; ++i) {
        const double nits = std::max(peak * i / kTable, 1e-3);
        factor[size_t(i)] = float(eetfToSdr(nits, peak) / nits);
    }
    // To linear sRGB, by way of XYZ.
    const Matrix3 toXyz = rgbToXyz(nclx);
    static const double kToSrgb[3][3] = {{3.2404542, -1.5371385, -0.4985314},
                                         {-0.9692660, 1.8760108, 0.0415560},
                                         {0.0556434, -0.2040259, 1.0572252}};
    float m[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r * 3 + c] = float(kToSrgb[r][0] * toXyz[0][c] + kToSrgb[r][1] * toXyz[1][c] + kToSrgb[r][2] * toXyz[2][c]);
    const float luma[3] = {float(toXyz[1][0]), float(toXyz[1][1]), float(toXyz[1][2])};
    const uint8_t *encode = encodeTable();
    const int channels = alpha ? 4 : 3;
    const int mask = codes - 1;
    const float peakF = float(peak);
    auto rows = [&](int from, int to) {
        constexpr float steps = kSteps - 1;
        for (int y = from; y < to; ++y) {
            const auto *in = reinterpret_cast<const uint16_t *>(data + stride * y);
            uchar *o = out.scanLine(y);
            for (int x = 0; x < width; ++x, in += channels, o += 4) {
                float r = linear[size_t(in[0] & mask)], g = linear[size_t(in[1] & mask)], b = linear[size_t(in[2] & mask)];
                if (hlg) {
                    // The OOTF: display light from scene light, by the
                    // scene's luminance.
                    const float ys = luma[0] * r + luma[1] * g + luma[2] * b;
                    const float gain = ys > 0 ? float(kHlgPeakNits) * std::pow(ys, 0.2f) : 0.f;
                    r *= gain;
                    g *= gain;
                    b *= gain;
                }
                // Above the peak, SDR white (the EETF of the peak) at most.
                const float most = std::max({r, g, b, 0.f});
                const float at = std::min(most / peakF, 1.f) * kTable;
                const int i = std::min(int(at), kTable - 1);
                const float f = most > peakF ? 1 / most
                                             : factor[size_t(i)] + (factor[size_t(i + 1)] - factor[size_t(i)]) * (at - float(i));
                r *= f;
                g *= f;
                b *= f;
                for (int k = 0; k < 3; ++k) {
                    const float v = (m[k * 3] * r + m[k * 3 + 1] * g + m[k * 3 + 2] * b) * steps;
                    o[k] = encode[int(std::clamp(v, 0.f, steps) + 0.5f)];
                }
                o[3] = alpha ? uchar((in[3] & mask) * 255 / mask) : 255;
            }
        }
    };
    if (qsizetype(width) * height < (1 << 20)) {
        rows(0, height);
    } else {
        std::vector<int> starts;
        for (int y = 0; y < height; y += 64)
            starts.push_back(y);
        QtConcurrent::blockingMap(starts, [&](int from) { rows(from, std::min(height, from + 64)); });
    }
    out.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return out;
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
