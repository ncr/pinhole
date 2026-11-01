#include "marks.h"

#include "theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QTextBoundaryFinder>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

double lerp(double a, double b, double t) { return a + (b - a) * t; }

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
// within the middle seven tenths so the shadow has room. As a `tube` (a
// neon sign's) the filled shapes are drawn as their outlines.
QImage shape(const MarkShape &mark, int px, bool tube = false) {
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(px, px);
    const QColor white(255, 255, 255);
    if (!mark.glyph.isEmpty()) {
        // A character of the theme's font, as big as Pinhole's own shapes
        // are (six tenths of the square at its longer side), in the middle;
        // filled, or as a tube its outline.
        QFont font(Theme::fontFamily());
        font.setPixelSize(100);
        QPainterPath path;
        path.addText(0, 0, font, mark.glyph);
        const QRectF bounds = path.boundingRect();
        if (bounds.isEmpty())
            return image;
        const double scale = 0.6 / std::max(bounds.width(), bounds.height());
        QTransform fit;
        fit.translate(0.5, 0.5);
        fit.scale(scale, scale);
        fit.translate(-bounds.center().x(), -bounds.center().y());
        if (tube) {
            p.setPen(QPen(white, 0.06, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(white);
        }
        p.drawPath(fit.map(path));
        return image;
    }
    if (mark.builtin == QLatin1String("video")) {
        // A play triangle, its corners rounded.
        QPainterPath path;
        path.moveTo(0.35, 0.25);
        path.lineTo(0.35, 0.75);
        path.lineTo(0.77, 0.5);
        path.closeSubpath();
        p.setPen(QPen(white, 0.08, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(tube ? QBrush(Qt::NoBrush) : QBrush(white));
        p.drawPath(path);
    } else if (mark.builtin == QLatin1String("live")) {
        // As the iPhone draws Live: a dot, a ring, and a ring of dots.
        p.setPen(Qt::NoPen);
        p.setBrush(white);
        p.drawEllipse(QPointF(0.5, 0.5), 0.085, 0.085);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(white, 0.065));
        p.drawEllipse(QPointF(0.5, 0.5), 0.2, 0.2);
        p.setPen(Qt::NoPen);
        p.setBrush(white);
        // As a tube fewer and bigger, or the glow runs them together.
        const int dots = tube ? 9 : 14;
        const double ring = tube ? 0.35 : 0.33, dot = tube ? 0.04 : 0.03;
        for (int i = 0; i < dots; ++i) {
            const double a = 2 * M_PI * i / dots;
            p.drawEllipse(QPointF(0.5 + ring * std::cos(a), 0.5 + ring * std::sin(a)), dot, dot);
        }
    } else if (mark.builtin == QLatin1String("heart")) {
        // A heart.
        QPainterPath path;
        path.moveTo(0.5, 0.79);
        path.cubicTo(0.3, 0.64, 0.17, 0.52, 0.17, 0.38);
        path.cubicTo(0.17, 0.27, 0.26, 0.2, 0.35, 0.2);
        path.cubicTo(0.42, 0.2, 0.47, 0.24, 0.5, 0.3);
        path.cubicTo(0.53, 0.24, 0.58, 0.2, 0.65, 0.2);
        path.cubicTo(0.74, 0.2, 0.83, 0.27, 0.83, 0.38);
        path.cubicTo(0.83, 0.52, 0.7, 0.64, 0.5, 0.79);
        if (tube) {
            p.setPen(QPen(white, 0.075, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(white);
        }
        p.drawPath(path);
    } else if (mark.builtin == QLatin1String("stack")) {
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
        p.setBrush(tube ? QColor(Qt::transparent) : QColor(255, 255, 255, 60));
        p.drawRoundedRect(front, 0.07, 0.07);
    }
    return image;
}

} // namespace

MarkShape markShape(const QString &icon) {
    if (icon == QLatin1String("video") || icon == QLatin1String("live") || icon == QLatin1String("stack"))
        return {icon, {}};
    if (icon == QLatin1String("heart") || icon == QLatin1String("favorite"))
        return {QStringLiteral("heart"), {}};
    // One character as people see one: a glyph of two code units (past the
    // first plane), or with a mark joined to it, is still one.
    if (icon.isEmpty() || icon.size() > 16 || icon.at(0).isSpace())
        return {};
    QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, icon);
    if (graphemes.toNextBoundary() != icon.size())
        return {};
    return {{}, icon};
}

QImage markImage(const MarkShape &mark, int px) {
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

QImage plainMark(const MarkShape &mark, const QColor &colour, int px) {
    px = std::max(8, px);
    QImage tube = shape(mark, px, true);
    {
        QPainter p(&tube);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        const auto light = [](int c) { return c + (255 - c) / 5; };
        p.fillRect(tube.rect(), QColor(light(colour.red()), light(colour.green()), light(colour.blue())));
    }
    // The shadow darker and wider than a white mark's: thin coloured lines
    // need more of it to stand off a photo.
    QImage shadow = blurredAlpha(shape(mark, px, true), std::max(1, px / 10));
    {
        QPainter p(&shadow);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.fillRect(shadow.rect(), QColor(0, 0, 0, 230));
    }
    QImage result(px, px, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter r(&result);
    r.drawImage(QPointF(0, std::max(1.0, px / 40.0)), shadow);
    r.drawImage(QPointF(0, 0), tube);
    return result;
}

NeonMark neonMark(const MarkShape &mark, const QColor &colour, int px, double lit) {
    px = std::max(8, px);
    const int side = int(std::lround(px * kNeonSpread)), at = (side - px) / 2;
    QImage tube(side, side, QImage::Format_ARGB32_Premultiplied);
    tube.fill(Qt::transparent);
    QPainter(&tube).drawImage(at, at, shape(mark, px, true));
    NeonMark neon;
    neon.core = tube;
    {
        QPainter p(&neon.core);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        // White-hot when lit, the colour's dark glass when not.
        const auto hot = [&](int c) { return int(lerp(c * 0.3, c + (255 - c) * 0.3, lit)); };
        p.fillRect(neon.core.rect(), QColor(hot(colour.red()), hot(colour.green()), hot(colour.blue()),
                                            int(255 * (0.55 + 0.45 * lit))));
    }
    // A tight bright glow hugging the tube and a wide soft one, and under
    // them a wider dark one.
    const QImage near = blurredAlpha(tube, std::max(1, px / 18));
    const QImage far = blurredAlpha(tube, std::max(2, px / 5));
    const QImage dark = blurredAlpha(tube, std::max(3, px / 4));
    neon.glow = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < side; ++y) {
        const auto *n = reinterpret_cast<const QRgb *>(near.constScanLine(y));
        const auto *f = reinterpret_cast<const QRgb *>(far.constScanLine(y));
        const auto *d = reinterpret_cast<const QRgb *>(dark.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(neon.glow.scanLine(y));
        for (int x = 0; x < side; ++x) {
            const double light = lit * std::min(1.0, 2.0 * qAlpha(n[x]) / 255.0 + 2.4 * qAlpha(f[x]) / 255.0);
            const double shade = std::min(1.0, 2.5 * qAlpha(d[x]) / 255.0) * (0.2 + 0.3 * lit);
            const double alpha = shade * (1 - light) + 0.45 * light;
            out[x] = qRgba(int(colour.red() * light), int(colour.green() * light), int(colour.blue() * light),
                           int(std::lround(alpha * 255)));
        }
    }
    return neon;
}

QImage MarkProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    // A glyph comes percent-encoded (encodeURIComponent), as any character
    // but a letter or a digit may.
    const MarkShape mark = markShape(QUrl::fromPercentEncoding(id.toUtf8()));
    const int px = requestedSize.isValid() ? std::max(requestedSize.width(), requestedSize.height()) : 32;
    QImage image(std::max(8, px), std::max(8, px), QImage::Format_ARGB32_Premultiplied);
    if (mark.valid())
        image = markImage(mark, px);
    else
        image.fill(Qt::transparent);
    if (size)
        *size = image.size();
    return image;
}
