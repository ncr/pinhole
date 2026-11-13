#pragma once

#include <QQuickImageProvider>

#include <array>

// The small marks in a grid cell's bottom left corner that say what a cell
// is beyond a photo: a video, a Live Photo, a favourite, several photos as
// one card (a plugin's stack, such as a series), or whatever a plugin marks
// (PluginMark). Each can be turned off in the side panel (MarkChoices).
//
// A mark's shape: one of Pinhole's own drawn ones (`builtin`: "video",
// "live", "heart", "stack"), or one character of the theme's font, a Nerd
// Font glyph (`glyph`). Neither for an icon that is neither: not a mark.
struct MarkShape {
    QString builtin;
    QString glyph;
    bool valid() const { return !builtin.isEmpty() || !glyph.isEmpty(); }
    bool operator==(const MarkShape &) const = default;
};
// "video", "live", "heart" (or "favorite"), "stack", or one character (a
// single grapheme, an emoji's or a Nerd Font glyph's) as a glyph.
MarkShape markShape(const QString &icon);

// One mark, white over a thin dark outline so it reads on any photo, `px`
// pixels square (device pixels), the same in the grid at every level, in
// the side panel, and as the bar's panel draws a video's.
QImage markImage(const MarkShape &mark, int px);

// A video's length, as the grid writes it after the video mark where the
// cell has room (and as the bar's panel writes it under a video): "0:42",
// "12:05", "1:02:33", rounded to the second; empty for none.
QString clockText(double seconds);

// The characters a length is written in, "0123456789:", white over a thin
// dark outline as markImage() draws a mark, each in a cell `px` square of a
// strip twice `px` tall: their outlines in the top row, their cores in the
// bottom, so the grid draws a length's outlines first and none falls on a
// neighbour's core. As tall as the marks' shapes, about the middle of the
// cell, which the character's `advance` (a share of `px`, every digit as
// wide) is centred in. clockChar() is a character's cell, or -1.
constexpr int kClockChars = 11;
struct PlainClock {
    QImage strip;
    std::array<double, kClockChars> advance{};
};
PlainClock plainClock(int px);
int clockChar(QChar c);

// "image://mark/video" (live, heart, stack, or a glyph, its character
// percent-encoded) for QML, as the grid draws them.
class MarkProvider : public QQuickImageProvider {
public:
    MarkProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
