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

// One mark, white with a soft shadow so it reads on any photo, `px` pixels
// square (device pixels), as the side panel shows it.
QImage markImage(const MarkShape &mark, int px);

// The same mark as its neon tube (neonMark()) in `colour` but unlit, for
// cells seen closer, where a neon sign would be too much and a small white
// mark is lost on a big photo: the tube's shape in the colour, a little
// lighter, over a soft dark shadow so it reads on bright photos too, `px`
// pixels square.
QImage plainMark(const MarkShape &mark, const QColor &colour, int px);

// A video's length, as the grid writes it after the video mark on cells
// seen closer (and as the bar's panel writes it under a video): "0:42",
// "12:05", "1:02:33", rounded to the second; empty for none.
QString clockText(double seconds);

// The characters a length is written in, "0123456789:", in `colour` over a
// soft dark shadow as plainMark() draws a mark, each in a cell `px` square of a strip twice `px` tall: their
// shadows in the top row, their cores in the bottom, so the grid draws a
// length's shadows first and none falls on a neighbour's core. As tall as
// the plain marks' shapes, about the middle of the cell, which the
// character's `advance` (a share of `px`, every digit as wide) is centred
// in. clockChar() is a character's cell, or -1.
constexpr int kClockChars = 11;
struct PlainClock {
    QImage strip;
    std::array<double, kClockChars> advance{};
};
PlainClock plainClock(const QColor &colour, int px);
int clockChar(QChar c);

// The same mark lit like a neon tube, for cells seen from afar, where a
// white mark is lost among the photos: a white-hot core in a tint of
// `colour`, and the light it throws, which partly adds to what is under it
// (premultiplied, its colour stronger than its alpha) and darkens the rest a
// little so it shows on bright photos too. Both kNeonSpread times the
// mark's `px`, the mark in their middle. The grid draws every glow, then
// every core, so no light falls on a neighbour's core. `lit` from 0, the
// tube dark, to 1, as it strikes and stays on.
constexpr double kNeonSpread = 2;
struct NeonMark {
    QImage glow, core;
};
NeonMark neonMark(const MarkShape &mark, const QColor &colour, int px, double lit = 1);

// "image://mark/video" (live, heart, stack, or a glyph, its character
// percent-encoded) for QML, as the grid draws them.
class MarkProvider : public QQuickImageProvider {
public:
    MarkProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
