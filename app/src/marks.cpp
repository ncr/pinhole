#include "marks.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// A box blur of an image's alpha, `radius` pixels each way, three passes
// across and down: near enough a Gaussian for a shadow this small.
QImage blurredAlpha(const QImage &source, int radius) {
    const int w = source.width(), h = source.height();
    std::vector<float> a(size_t(w) * h), b(a.size());
    for (int y = 0; y < h; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < w; ++x)
            a[size_t(y) * w + x] = qAlpha(line[x]);
    }
    auto pass = [&](bool across) {
        const int lines = across ? h : w, length = across ? w : h;
        for (int l = 0; l < lines; ++l) {
            for (int i = 0; i < length; ++i) {
                float sum = 0;
                int n = 0;
                for (int k = std::max(0, i - radius); k <= std::min(length - 1, i + radius); ++k, ++n)
                    sum += across ? a[size_t(l) * w + k] : a[size_t(k) * w + l];
                (across ? b[size_t(l) * w + i] : b[size_t(i) * w + l]) = sum / float(2 * radius + 1);
            }
        }
        a.swap(b);
    };
    for (int i = 0; i < 3; ++i) {
        pass(true);
        pass(false);
    }
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
            line[x] = qRgba(0, 0, 0, int(std::clamp(a[size_t(y) * w + x], 0.0f, 255.0f)));
    }
    return out;
}

// The mark itself, white, in a unit square scaled to `px`, its shapes kept
// within the middle seven tenths so the shadow has room.
QImage shape(Mark mark, int px) {
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(px, px);
    const QColor white(255, 255, 255);
    switch (mark) {
    case MarkVideo: {
        // A play triangle, its corners rounded.
        QPainterPath path;
        path.moveTo(0.35, 0.25);
        path.lineTo(0.35, 0.75);
        path.lineTo(0.77, 0.5);
        path.closeSubpath();
        p.setPen(QPen(white, 0.08, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(white);
        p.drawPath(path);
        break;
    }
    case MarkLive: {
        // As the iPhone draws Live: a dot, a ring, and a ring of dots.
        p.setPen(Qt::NoPen);
        p.setBrush(white);
        p.drawEllipse(QPointF(0.5, 0.5), 0.085, 0.085);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(white, 0.065));
        p.drawEllipse(QPointF(0.5, 0.5), 0.2, 0.2);
        p.setPen(Qt::NoPen);
        p.setBrush(white);
        constexpr int kDots = 14;
        for (int i = 0; i < kDots; ++i) {
            const double a = 2 * M_PI * i / kDots;
            p.drawEllipse(QPointF(0.5 + 0.33 * std::cos(a), 0.5 + 0.33 * std::sin(a)), 0.03, 0.03);
        }
        break;
    }
    case MarkFavorite: {
        // A heart.
        QPainterPath path;
        path.moveTo(0.5, 0.79);
        path.cubicTo(0.3, 0.64, 0.17, 0.52, 0.17, 0.38);
        path.cubicTo(0.17, 0.27, 0.26, 0.2, 0.35, 0.2);
        path.cubicTo(0.42, 0.2, 0.47, 0.24, 0.5, 0.3);
        path.cubicTo(0.53, 0.24, 0.58, 0.2, 0.65, 0.2);
        path.cubicTo(0.74, 0.2, 0.83, 0.27, 0.83, 0.38);
        path.cubicTo(0.83, 0.52, 0.7, 0.64, 0.5, 0.79);
        p.setPen(Qt::NoPen);
        p.setBrush(white);
        p.drawPath(path);
        break;
    }
    case MarkStack: {
        // Two cards, one behind the other: the one behind shows only at
        // its top and right.
        const QRectF front(0.19, 0.33, 0.44, 0.44), back = front.translated(0.15, -0.15);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(white, 0.065));
        p.drawRoundedRect(back, 0.07, 0.07);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        p.drawRoundedRect(front.adjusted(-0.08, -0.08, 0.08, 0.08), 0.1, 0.1);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(white, 0.065));
        p.setBrush(QColor(255, 255, 255, 60));
        p.drawRoundedRect(front, 0.07, 0.07);
        break;
    }
    }
    return image;
}

} // namespace

QImage markImage(Mark mark, int px) {
    px = std::max(8, px);
    const QImage white = shape(mark, px);
    // The shadow at a little over half, a little below, then the mark over it.
    QImage shadow = blurredAlpha(white, std::max(1, px / 14));
    {
        QPainter p(&shadow);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.fillRect(shadow.rect(), QColor(0, 0, 0, 150));
    }
    QImage result(px, px, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter r(&result);
    r.drawImage(QPointF(0, std::max(1.0, px / 32.0)), shadow);
    r.drawImage(QPointF(0, 0), white);
    return result;
}

QImage MarkProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    const Mark mark = id == QLatin1String("video") ? MarkVideo
                    : id == QLatin1String("live")  ? MarkLive
                    : id == QLatin1String("favorite") ? MarkFavorite
                                                       : MarkStack;
    const int px = requestedSize.isValid() ? std::max(requestedSize.width(), requestedSize.height()) : 32;
    const QImage image = markImage(mark, px);
    if (size)
        *size = image.size();
    return image;
}
