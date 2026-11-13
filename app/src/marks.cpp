#include "marks.h"

#include "theme.h"

#include <QFontMetricsF>
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
// within the middle seven tenths so the outline has room.
QImage shape(const MarkShape &mark, int px) {
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(px, px);
    const QColor white(255, 255, 255);
    if (!mark.glyph.isEmpty()) {
        // A character of the theme's font, as big as Pinhole's own shapes
        // are (six tenths of the square at its longer side), in the middle.
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
        p.setPen(Qt::NoPen);
        p.setBrush(white);
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
        p.setBrush(white);
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
        // Few and big enough that the outline does not run them together.
        const int dots = 9;
        const double ring = 0.35, dot = 0.04;
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
        p.setPen(Qt::NoPen);
        p.setBrush(white);
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
        p.setBrush(QColor(255, 255, 255, 60));
        p.drawRoundedRect(front, 0.07, 0.07);
    }
    return image;
}

// White over a thin dark outline, so it reads on any photo, light or dark.
QImage outlined(const QImage &white, int px) {
    QImage outline = blurredAlpha(white, std::max(1, px / 28));
    for (int y = 0; y < outline.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(outline.scanLine(y));
        for (int x = 0; x < outline.width(); ++x)
            line[x] = qRgba(0, 0, 0, std::min(190, qAlpha(line[x]) * 3));
    }
    QPainter(&outline).drawImage(0, 0, white);
    return outline;
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
    return outlined(shape(mark, px), px);
}

QString clockText(double seconds) {
    if (!(seconds > 0))
        return {};
    const qint64 s = std::llround(seconds), m = s / 60, h = m / 60;
    const QString ss = QStringLiteral("%1").arg(s % 60, 2, 10, QLatin1Char('0'));
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m % 60, 2, 10, QLatin1Char('0')).arg(ss);
    return QStringLiteral("%1:%2").arg(m).arg(ss);
}

int clockChar(QChar c) {
    if (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
        return c.unicode() - '0';
    return c == QLatin1Char(':') ? 10 : -1;
}

PlainClock plainClock(int px) {
    px = std::max(8, px);
    PlainClock clock;
    clock.strip = QImage(px * kClockChars, px * 2, QImage::Format_ARGB32_Premultiplied);
    clock.strip.fill(Qt::transparent);
    // Laid out at a size of 100 and scaled into the unit square, the
    // digits as tall as the marks' shapes are.
    QFont font(Theme::fontFamily());
    font.setPixelSize(100);
    font.setWeight(QFont::DemiBold);
    const QFontMetricsF metrics(font);
    double digitHeight = 0, digitAdvance = 0;
    for (char d = '0'; d <= '9'; ++d) {
        QPainterPath path;
        path.addText(0, 0, font, QString(QLatin1Char(d)));
        digitHeight = std::max(digitHeight, -path.boundingRect().top());
        digitAdvance = std::max(digitAdvance, metrics.horizontalAdvance(QLatin1Char(d)));
    }
    if (digitHeight <= 0)
        return clock;
    const double scale = 0.42 / digitHeight, baseline = 0.5 + 0.21;
    QPainter strip(&clock.strip);
    for (int i = 0; i < kClockChars; ++i) {
        const QChar c = i < 10 ? QChar(QLatin1Char(char('0' + i))) : QChar(QLatin1Char(':'));
        const double own = metrics.horizontalAdvance(c);
        clock.advance[size_t(i)] = (i < 10 ? digitAdvance : own) * scale;
        QPainterPath path;
        path.addText(0, 0, font, QString(c));
        QImage core(px, px, QImage::Format_ARGB32_Premultiplied);
        core.fill(Qt::transparent);
        {
            QPainter p(&core);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(px, px);
            p.translate(0.5 - own * scale / 2, baseline);
            p.scale(scale, scale);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::white);
            p.drawPath(path);
        }
        // The outline alone, the core cut out of it.
        QImage outline = outlined(core, px);
        {
            QPainter p(&outline);
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            p.drawImage(0, 0, core);
        }
        strip.drawImage(i * px, 0, outline);
        strip.drawImage(i * px, px, core);
    }
    return clock;
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
