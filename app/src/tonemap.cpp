#include "tonemap.h"

#include "colour.h"

#include <QImage>
#include <QMutex>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {

// What the frames of one stream are made SDR with: tables of its curve and
// of the EETF, made once for its curve and peak.
struct Tables {
    bool hlg = false;
    double peak = 0;
    // A non-linear value from 0 to 1, in kCurve steps, to linear light:
    // PQ's in nits, HLG's as scene light (0 to 1).
    static constexpr int kCurve = 16384;
    std::vector<float> curve;
    // The largest channel's nits, from 0 to the peak in kFactor steps, to
    // the factor that makes them linear SDR (1 is SDR white).
    static constexpr int kFactor = 4096;
    std::vector<float> factor;
    // HLG's OOTF: the scene's luminance to the power 0.2, in kCurve steps.
    std::vector<float> ootf;
};

std::shared_ptr<const Tables> tablesFor(bool hlg, double peak) {
    static QMutex lock;
    static std::shared_ptr<const Tables> last;
    QMutexLocker locked(&lock);
    if (last && last->hlg == hlg && last->peak == peak)
        return last;
    auto t = std::make_shared<Tables>();
    t->hlg = hlg;
    t->peak = peak;
    t->curve.resize(Tables::kCurve + 1);
    for (int i = 0; i <= Tables::kCurve; ++i) {
        const double e = double(i) / Tables::kCurve;
        t->curve[size_t(i)] = float(hlg ? hlgSceneLight(e) : pqToNits(e));
    }
    t->factor.resize(Tables::kFactor + 1);
    for (int i = 0; i <= Tables::kFactor; ++i) {
        const double nits = std::max(peak * i / Tables::kFactor, 1e-3);
        t->factor[size_t(i)] = float(eetfToSdr(nits, peak) / nits);
    }
    if (hlg) {
        t->ootf.resize(Tables::kCurve + 1);
        for (int i = 0; i <= Tables::kCurve; ++i)
            t->ootf[size_t(i)] = float(std::pow(double(i) / Tables::kCurve, 0.2));
    }
    last = t;
    return last;
}

inline float lookUp(const std::vector<float> &table, float x, int steps) {
    const float at = std::clamp(x, 0.f, 1.f) * float(steps);
    const int i = std::min(int(at), steps - 1);
    return table[size_t(i)] + (table[size_t(i + 1)] - table[size_t(i)]) * (at - float(i));
}

// The nearest of a table of fine steps: a 10-bit picture's values fall
// between two of 16,384 at most a sixteenth of a code apart.
inline float nearest(const float *table, float x, float steps) {
    return table[int(std::clamp(x, 0.f, 1.f) * steps + 0.5f)];
}

} // namespace

QVideoFrame toneMapFrame(const QVideoFrame &source, QList<QImage> *buffers) {
    const QVideoFrameFormat format = source.surfaceFormat();
    const auto transfer = format.colorTransfer();
    const bool hlg = transfer == QVideoFrameFormat::ColorTransfer_STD_B67;
    if (!hlg && transfer != QVideoFrameFormat::ColorTransfer_ST2084)
        return {};
    // How each plane's samples are laid out: 1 or 2 bytes, the bits used
    // (P010 keeps its ten in the high ones), and interleaved chroma.
    int bytes = 1, bits = 8, shift = 0;
    bool interleaved = false;
    switch (format.pixelFormat()) {
    case QVideoFrameFormat::Format_YUV420P:
        break;
    case QVideoFrameFormat::Format_NV12:
        interleaved = true;
        break;
    case QVideoFrameFormat::Format_YUV420P10:
        bytes = 2, bits = 10;
        break;
    case QVideoFrameFormat::Format_P010:
        bytes = 2, bits = 10, shift = 6, interleaved = true;
        break;
    case QVideoFrameFormat::Format_P016:
        bytes = 2, bits = 16, interleaved = true;
        break;
    default:
        return {};
    }
    QVideoFrame frame = source;
    if (!frame.map(QVideoFrame::ReadOnly))
        return {};
    const int width = frame.width(), height = frame.height();
    // Into a picture no frame shown holds any more, if there is one: a 4K
    // frame's 33 MB not asked of the system anew each time.
    // Taken out of the list while drawn into, so that nothing else holds
    // it then (writing to a picture held twice would copy it first).
    QImage out;
    if (buffers) {
        for (qsizetype i = 0; i < buffers->size(); ++i)
            if ((*buffers)[i].size() == QSize(width, height) && (*buffers)[i].isDetached()) {
                out = buffers->takeAt(i);
                break;
            }
    }
    if (out.isNull())
        out = QImage(width, height, QImage::Format_RGBX8888);
    if (out.isNull()) {
        frame.unmap();
        return {};
    }
    uchar *outBits = out.bits();
    const qsizetype outStride = out.bytesPerLine();
    // PQ's peak: as the stream says, else 1000 nits (Qt says 10000 for
    // none, and its FFmpeg backend multiplied a mastering display's peak,
    // already in nits, by 10000).
    double peak = kHlgPeakNits;
    if (!hlg) {
        peak = format.maxLuminance();
        if (peak > 10000)
            peak /= 10000;
        if (peak <= kSdrWhiteNits || peak >= 10000)
            peak = 1000;
    }
    const std::shared_ptr<const Tables> tables = tablesFor(hlg, peak);
    // Its codes to Y'CbCr: video range unless it says full.
    const float top = float((1 << bits) - 1);
    const float unit = float(1 << (bits - 8));
    const bool full = format.colorRange() == QVideoFrameFormat::ColorRange_Full;
    const float yOffset = full ? 0 : 16 * unit, yScale = full ? 1 / top : 1 / (219 * unit);
    const float cOffset = float(1 << (bits - 1)), cScale = full ? 1 / top : 1 / (224 * unit);
    // Y'CbCr to R'G'B' by its matrix, and its primaries to sRGB's.
    const auto space = format.colorSpace();
    const bool bt2020 = space == QVideoFrameFormat::ColorSpace_BT2020 || space == QVideoFrameFormat::ColorSpace_Undefined;
    const float kr = bt2020 ? 0.2627f : space == QVideoFrameFormat::ColorSpace_BT601 ? 0.299f : 0.2126f;
    const float kb = bt2020 ? 0.0593f : space == QVideoFrameFormat::ColorSpace_BT601 ? 0.114f : 0.0722f;
    const float kg = 1 - kr - kb;
    const float crR = 2 * (1 - kr), cbB = 2 * (1 - kb), cbG = -2 * (1 - kb) * kb / kg, crG = -2 * (1 - kr) * kr / kg;
    static const float kFrom2020[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
    static const float kSame[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float *m = bt2020 ? kFrom2020 : kSame;
    const uchar *planeY = frame.bits(0), *planeU = frame.bits(1), *planeV = interleaved ? nullptr : frame.bits(2);
    const qsizetype strideY = frame.bytesPerLine(0), strideU = frame.bytesPerLine(1), strideV = interleaved ? 0 : frame.bytesPerLine(2);
    const uint8_t *encode = srgbEncodeTable();
    const float peakF = float(peak);
    const int chromaWidth = (width + 1) / 2, chromaRows = (height + 1) / 2;
    const Tables &t = *tables;
    const float *curve = t.curve.data();
    const float *ootf = t.hlg ? t.ootf.data() : nullptr;
    constexpr float curveSteps = Tables::kCurve;
    auto sample = [bytes, shift](const uchar *row, int i) -> float {
        return bytes == 1 ? float(row[i]) : float(reinterpret_cast<const uint16_t *>(row)[i] >> shift);
    };
    auto rows = [&](int from, int to) {
        constexpr float steps = kSrgbSteps - 1;
        for (int y = from; y < to; ++y) {
            const uchar *rowY = planeY + strideY * y;
            // Chroma at half the size, sited left and between two rows
            // (as H.264 and HEVC have it): each pixel's between the two
            // samples nearest, as the window samples a texture.
            const float cy = std::max((float(y) - 0.5f) / 2, 0.f);
            const int y0 = std::min(int(cy), chromaRows - 1), y1 = std::min(y0 + 1, chromaRows - 1);
            const float wy = cy - float(y0);
            const uchar *u0 = planeU + strideU * y0, *u1 = planeU + strideU * y1;
            const uchar *v0 = interleaved ? nullptr : planeV + strideV * y0;
            const uchar *v1 = interleaved ? nullptr : planeV + strideV * y1;
            auto chroma = [&](const uchar *a, const uchar *b, int i, int j, float wx) {
                const float top = sample(a, i) + (sample(a, j) - sample(a, i)) * wx;
                const float bottom = sample(b, i) + (sample(b, j) - sample(b, i)) * wx;
                return top + (bottom - top) * wy;
            };
            uchar *o = outBits + outStride * y;
            for (int x = 0; x < width; ++x, o += 4) {
                const float yy = (sample(rowY, x) - yOffset) * yScale;
                const int i0 = x / 2, i1 = std::min(i0 + 1, chromaWidth - 1);
                const float wx = (x & 1) ? 0.5f : 0.f;
                const float cb = ((interleaved ? chroma(u0, u1, 2 * i0, 2 * i1, wx) : chroma(u0, u1, i0, i1, wx)) - cOffset) * cScale;
                const float cr = ((interleaved ? chroma(u0, u1, 2 * i0 + 1, 2 * i1 + 1, wx) : chroma(v0, v1, i0, i1, wx)) - cOffset) * cScale;
                float r = nearest(curve, yy + crR * cr, curveSteps);
                float g = nearest(curve, yy + cbG * cb + crG * cr, curveSteps);
                float b = nearest(curve, yy + cbB * cb, curveSteps);
                if (hlg) {
                    // The OOTF: display light from the scene's, by its
                    // luminance.
                    const float gain = float(kHlgPeakNits) * nearest(ootf, kr * r + kg * g + kb * b, curveSteps);
                    r *= gain;
                    g *= gain;
                    b *= gain;
                }
                // Above the peak, SDR white (the EETF of the peak) at most.
                const float most = std::max({r, g, b});
                const float f = most > peakF ? 1 / most : lookUp(t.factor, most / peakF, Tables::kFactor);
                r *= f;
                g *= f;
                b *= f;
                for (int k = 0; k < 3; ++k) {
                    const float v = (m[k * 3] * r + m[k * 3 + 1] * g + m[k * 3 + 2] * b) * steps;
                    o[k] = encode[int(std::clamp(v, 0.f, steps) + 0.5f)];
                }
                o[3] = 255;
            }
        }
    };
    std::vector<int> starts;
    for (int y = 0; y < height; y += 32)
        starts.push_back(y);
    QtConcurrent::blockingMap(starts, [&](int from) { rows(from, std::min(height, from + 32)); });
    frame.unmap();

    QVideoFrame made(out);
    // Kept for a frame to come, once this one is let go.
    if (buffers) {
        if (buffers->size() >= 4)
            buffers->removeFirst();
        buffers->append(out);
    }
    made.setStartTime(source.startTime());
    made.setEndTime(source.endTime());
    made.setRotation(source.rotation());
    made.setMirrored(source.mirrored());
    made.setStreamFrameRate(source.streamFrameRate());
    return made;
}
