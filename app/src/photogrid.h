#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QTimer>

#include <atomic>
#include <memory>
#include <set>

#include "scrollphysics.h"

class Library;
class ThumbnailLoader;
class FrameStats;
class TouchpadHold;
struct GridRenderState;

// The square photo grid. Everything is computed from the library position,
// so a jump to any of 100k items costs the same as scrolling one row:
//
// - Positions are arithmetic on (columns, scroll, visual scale), done in
//   screen space every frame.
// - Thumbnails live in the scene graph's texture atlases; all cells that
//   share an atlas page are one geometry node, so a full screen is a handful
//   of draw calls whatever the zoom.
// - A cell without its thumbnail yet shows the photo's average colour; all
//   of those together are one more draw call.
//
// Zoom works like Photos on an iPhone: there is a fixed set of column
// counts, and a pinch never reflows the grid under the fingers. The photo
// between the fingers stays there; the two levels either side of the
// current zoom are both drawn, scaled to it around that photo, and
// cross-faded by how far the pinch has gone between them. On release the
// grid settles on one level, scale, alignment and fade animating together.
// PINHOLE_INPUT_LOG=<file>: append a line about an input event there.
void logInput(const QString &line);

class PhotoGrid : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY libraryChanged)
    Q_PROPERTY(int columns READ columns NOTIFY columnsChanged)
    Q_PROPERTY(bool animating READ animating NOTIFY animatingChanged)
    Q_PROPERTY(bool statsVisible READ statsVisible WRITE setStatsVisible NOTIFY statsVisibleChanged)
    Q_PROPERTY(QString statsText READ statsText NOTIFY statsTextChanged)
    // How far down the library the view is, 0 at the oldest, 1 at the newest.
    Q_PROPERTY(qreal scrollFraction READ scrollFraction NOTIFY scrolled)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    // The selection's border: the Omarchy theme's accent.
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
    // Select mode, entered on purpose (the Select button, Ctrl+A, a Ctrl or
    // Shift click): clicks tick photos instead of opening them. Leaving it
    // clears the selection.
    Q_PROPERTY(bool selecting READ selecting WRITE setSelecting NOTIFY selectingChanged)
    // The window's colour, showing between the cells.
    Q_PROPERTY(QColor background MEMBER m_background NOTIFY backgroundChanged)

public:
    explicit PhotoGrid(QQuickItem *parent = nullptr);
    ~PhotoGrid() override;

    void setSources(const Library *library, ThumbnailLoader *loader, FrameStats *stats);
    // The library was read again and its positions moved: `moved[old]` is
    // each photo's new position, or -1 if it is gone. Call right after the
    // swap; the view keeps the photos it showed, or stays at the newest end.
    void libraryReloaded(const std::vector<int> &moved);
    // Where the view is, for keeping it across a reload: the photo at the
    // top-left and how far from the top it is, and whether it is at the end.
    struct Anchor { int index; double offset; bool atEnd; };
    Anchor anchor() const;
    // Note where the view is, just before a reload.
    void rememberAnchor() { m_reloadAnchor = anchor(); }

    // Fingers resting on the touchpad stop a coast, as on a Mac.
    void setTouchpadHold(TouchpadHold *hold);

    int count() const;
    int columns() const { return m_columns; }
    bool animating() const { return m_animating; }
    bool statsVisible() const { return m_statsVisible; }
    void setStatsVisible(bool visible);
    QString statsText() const { return m_statsText; }

    // One animation frame; `dt` in seconds. Driven by a FrameAnimation in
    // QML so it runs in step with vsync.
    Q_INVOKABLE void advance(qreal dt);
    // +1 shows the next level out (more columns), -1 the next level in.
    Q_INVOKABLE void zoomStep(int delta);
    Q_INVOKABLE void scrollToStart();
    Q_INVOKABLE void scrollToEnd();
    Q_INVOKABLE void scrollPages(qreal pages);

    // The photo at a point in the grid, or -1; the photo under the pointer,
    // else the one in the middle of the view.
    Q_INVOKABLE int indexAt(qreal x, qreal y) const;
    Q_INVOKABLE int currentIndex() const;
    // Where a photo's cell is on screen (it may be off it), and bringing it
    // into view if it is not.
    Q_INVOKABLE QRectF rectOf(int index) const;
    Q_INVOKABLE void reveal(int index);

    // Selection, as in Photos: click, Ctrl-click to add or remove,
    // Shift-click for a range.
    int selectionCount() const { return int(m_selected.size()); }
    bool selecting() const { return m_selecting; }
    void setSelecting(bool selecting);
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &accent);
    Q_INVOKABLE QList<int> selection() const;
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();

    // The date scrubber: jump anywhere, the capture date (ms) at a position,
    // where each year begins, and whether a drag is under way (the grid then
    // draws from mosaic pages, as when scrolling very fast).
    qreal scrollFraction() const;
    Q_INVOKABLE void scrubTo(qreal fraction);
    Q_INVOKABLE qint64 dateAt(qreal fraction) const;
    Q_INVOKABLE QVariantList yearMarks() const;
    Q_INVOKABLE void setScrubbing(bool scrubbing);

    // For the benchmark.
    void setDriveVelocity(double pixelsPerSecond);
    void fling(double pixelsPerSecond);
    void jumpToFraction(double fraction);
    // A pinch as the benchmark drives it: the cell size the fingers ask
    // for, around `anchor`; then the fingers lift.
    void setEffectiveStride(double stride, QPointF anchor);
    void endContinuousZoom();
    // Switch to the level nearest `columns` at once, keeping the position.
    void setColumnsNow(int columns);
    static int levelCount();
    static int columnsAt(int level);
    double stride() const;
    double scrollRange() const { return maxScroll(m_columns); }
    int unresolvedVisible() const { return m_unresolved.load(); }
    // Cells on screen that want a sharper rendition than the thumbnail, and
    // how many of those already show it.
    std::pair<int, int> mediumOnScreen() const { return {m_mediumNeeded.load(), m_mediumShown.load()}; }
    // The largest vertical move of the pinned photo between two frames
    // since the last call, in pixels: how a zoom is checked for jumps.
    double takePinJump() {
        const double j = m_pinJump;
        m_pinJump = 0;
        m_pinTracked = false; // the next frame starts a new measurement
        return j;
    }
    // Frames since the last call in which the base layer left part of the
    // window uncovered (an empty band at an edge), and the widest band.
    std::pair<int, double> takeUncovered() {
        const auto r = std::make_pair(m_uncoveredFrames, m_uncoveredMax);
        m_uncoveredFrames = 0;
        m_uncoveredMax = 0;
        return r;
    }
    double scrollSpeedRows() const { return m_speedRows; }

signals:
    // A click or tap on a photo: open it.
    void activated(int index, QRectF rect);
    void scrolled();
    void selectionChanged();
    void accentChanged();
    void backgroundChanged();
    void selectingChanged();
    // The pointer moved far enough from a press on a photo: drag these out.
    void dragRequested(const QList<int> &indexes);
    // Right-click: the menu, at `pos`, for the photo there (-1 between
    // photos); for the selection when that photo is in it.
    void menuRequested(int index, QPointF pos);
    void libraryChanged();
    void columnsChanged();
    void animatingChanged();
    void statsVisibleChanged();
    void statsTextChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    bool event(QEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void releaseResources() override;

private:
    // One drawn grid: `columns` wide, scaled by `scale`, placed so content
    // point `pin` lands on view point `at`. At rest that is just the scroll
    // offset; during a zoom two of these are drawn, one faded over the other.
    struct Layer {
        int columns = 8;
        double scale = 1;
        QPointF pin;
        QPointF at;
        double opacity = 1;
        // While two levels cross-fade, each photo is drawn this share of the
        // way toward where the other level puts it: a light morph.
        double morph = 0;
        int toColumns = 0;
        double toScale = 1;
        QPointF toPin, toAt;
    };
    std::vector<Layer> layers() const;
    QRectF cellRect(int index, const Layer &layer) const;
    double strideFor(int columns) const;
    double maxScroll(int columns) const;
    // Put the view at `y`, within the ends, stopping any motion.
    void setScroll(double y);
    void syncBounds();
    void updateAnimating();
    void refreshStats();
    void invalidateSceneGraph();
    void touched(double timeMs, const char *how);

    // Zoom
    int levelForColumns(int columns) const;
    // The level a pinch at this cell size shows.
    int pinchLevel(double stride) const;
    // Pin the photo under `at` (view coordinates) and start zooming around it.
    void beginZoom(QPointF at);
    // The cell size the zoom shows now, and where the pinned photo sits.
    void zoomTo(double stride, QPointF at);
    // Settle on `level`: scale, position and fade animate there.
    void settleZoom(int level);
    // Content position of the pinned point in a grid `columns` wide.
    QPointF pinIn(int columns) const;

    const Library *m_library = nullptr;
    ThumbnailLoader *m_loader = nullptr;
    FrameStats *m_stats = nullptr;
    TouchpadHold *m_hold = nullptr;
    QPointF m_hover{-1, -1};
    std::set<int> m_selected;
    int m_selectionAnchor = -1;
    QPointF m_pressAt;
    int m_pressIndex = -1;
    bool m_dragged = false;
    bool m_selecting = false;
    // The press stopped the grid moving: its click opens nothing.
    bool m_pressStopped = false;
    bool m_scrubbing = false;
    QColor m_accent{0xff, 0xd6, 0x0a};
    // What shows between the cells, for their rounded corners.
    QColor m_background{0x0e, 0x0e, 0x10};
    Anchor m_reloadAnchor{-1, 0, true};
    // Position moves for the render thread to apply to its texture cache.
    std::shared_ptr<std::vector<int>> m_pendingMoves;
    qreal m_lastFraction = -1;
    void notifyScrolled();
    // When the current coast began; a touch from before it is old news.
    double m_coastFrom = 0;

    // Layout and motion, GUI thread (read by the render thread while the
    // GUI thread is blocked in sync).
    int m_level = 5;
    int m_columns = 8;
    double m_scroll = 0;

    ScrollPhysics m_physics;
    double m_driveVelocity = 0; // benchmark, px/s

    struct Zoom {
        bool active = false;   // pinching or settling
        bool settling = false;
        int index = 0;         // the pinned photo
        QPointF fraction;      // where in it the fingers are, 0..1
        double stride = 0;     // cell size shown now
        QPointF at;            // where the pinned point is shown now
        double focusX = 0;     // across, what the zoom grows about: the fingers
        QPointF pinStart, fingerStart; // both when the zoom began
        // A pinch: the cell size it began at and the scale since, which
        // `value` deltas add up to.
        double startStride = 0;
        double pinchScale = 1;
        std::vector<std::pair<double, double>> samples; // (seconds, log stride), for release speed
        // Settling: from what, to what.
        int target = 0;
        int partner = 0;       // the other level still fading out
        double fromStride = 0, toStride = 0;
        QPointF fromAt, toAt;
        double fromWeight = 0; // target level's share when settling began
        // Pinching: the one level shown (the denser of the two around the
        // cell size, scaled up so it covers the window), and the one it
        // replaced, fading out over a fixed time as in Photos.
        int shown = 0;
        int fadeFrom = -1;
        double fade = 1;
        double toScroll = 0;
        double t = 1;
    } m_zoom;

    QElapsedTimer m_clock;

    bool m_animating = false;
    bool m_statsVisible = false;
    QString m_statsText;
    QTimer m_statsTimer;

    // Measured on the render thread.
    double m_speedRows = 0;
    QPointF m_lastPinOnScreen;
    bool m_pinTracked = false;
    double m_pinJump = 0;
    int m_uncoveredFrames = 0;
    double m_uncoveredMax = 0;
    std::atomic<int> m_unresolved{0};
    std::atomic<int> m_mediumNeeded{0};
    std::atomic<int> m_mediumShown{0};
    std::atomic<int> m_textureCount{0};
    std::atomic<qint64> m_textureBytes{0};

    std::unique_ptr<GridRenderState> m_render;
};
