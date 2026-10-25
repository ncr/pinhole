#pragma once

#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QTimer>

#include <atomic>
#include <memory>

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
class PhotoGrid : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY libraryChanged)
    Q_PROPERTY(int columns READ columns NOTIFY columnsChanged)
    Q_PROPERTY(bool animating READ animating NOTIFY animatingChanged)
    Q_PROPERTY(bool statsVisible READ statsVisible WRITE setStatsVisible NOTIFY statsVisibleChanged)
    Q_PROPERTY(QString statsText READ statsText NOTIFY statsTextChanged)

public:
    explicit PhotoGrid(QQuickItem *parent = nullptr);
    ~PhotoGrid() override;

    void setSources(const Library *library, ThumbnailLoader *loader, FrameStats *stats);
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
        double toScroll = 0;
        double t = 1;
    } m_zoom;

    QElapsedTimer m_clock;

    bool m_animating = false;
    bool m_statsVisible = true;
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
