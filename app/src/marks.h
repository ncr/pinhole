#pragma once

#include <QQuickImageProvider>

// The small marks in a grid cell's bottom left corner that say what a cell
// is beyond a photo: a video, a Live Photo, a favourite, several photos as
// one card (a plugin's stack, such as a series). Each can be turned off in
// the side panel; PhotoGrid::marks holds the ones shown.
enum Mark : int {
    MarkVideo = 1,
    MarkLive = 2,
    MarkFavorite = 4,
    MarkStack = 8,
};
constexpr int kMarkKinds = 4;
constexpr int kAllMarks = MarkVideo | MarkLive | MarkFavorite | MarkStack;

// One mark, white with a soft shadow so it reads on any photo, `px` pixels
// square (device pixels). The grid and the side panel draw the same ones.
QImage markImage(Mark mark, int px);

// The same mark lit like a neon tube, for cells seen from afar, where a
// white mark is lost among the photos: a white-hot core in a tint of
// `colour`, and the light it throws, which partly adds to what is under it
// (premultiplied, its colour stronger than its alpha) and darkens the rest a
// little so it shows on bright photos too. Both kNeonSpread times the
// mark's `px`, the mark in their middle. The grid draws every glow, then
// every core, so no light falls on a neighbour's core.
constexpr double kNeonSpread = 2;
struct NeonMark {
    QImage glow, core;
};
NeonMark neonMark(Mark mark, const QColor &colour, int px);

// "image://mark/video" (live, favorite, stack) for QML, as the grid draws
// them.
class MarkProvider : public QQuickImageProvider {
public:
    MarkProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
