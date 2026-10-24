#pragma once

#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QTimer>

#include <atomic>
#include <memory>

class Library;
class ThumbnailLoader;
class FrameStats;
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
// Zoom changes the column count in whole steps and scales smoothly between
// them; when the column count changes, cells glide from where they were.
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

    int count() const;
    int columns() const { return m_columns; }
    bool animating() const { return m_animating; }
    bool statsVisible() const { return m_statsVisible; }
    void setStatsVisible(bool visible);
    QString statsText() const { return m_statsText; }

    // One animation frame; `dt` in seconds. Driven by a FrameAnimation in
    // QML so it runs in step with vsync.
    Q_INVOKABLE void advance(qreal dt);
    // +1 shows one more column (zoom out), -1 one fewer.
    Q_INVOKABLE void zoomStep(int delta);
    Q_INVOKABLE void scrollToStart();
    Q_INVOKABLE void scrollToEnd();
    Q_INVOKABLE void scrollPages(qreal pages);

    // For the benchmark.
    void setDriveVelocity(double pixelsPerSecond);
    void fling(double pixelsPerSecond);
    void jumpToFraction(double fraction);
    void setEffectiveStride(double stride, QPointF anchor);
    void endContinuousZoom();
    double stride() const;
    double scrollRange() const { return maxScroll(m_columns); }
    int unresolvedVisible() const { return m_unresolved.load(); }
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
    bool event(QEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void releaseResources() override;

private:
    // Where everything is: enough to place any cell on screen.
    struct Layout {
        int columns = 8;
        double scroll = 0;  // content y at the top of the view, before scaling
        double scale = 1;   // visual zoom between column steps
        QPointF anchor;     // the fixed point of that zoom, in item coordinates
    };
    QRectF cellRect(int index, const Layout &layout) const;
    double strideFor(int columns) const;
    double maxScroll(int columns) const;
    void clampScroll();
    void relayout(int columns, double scale, QPointF anchor, bool glide);
    void updateAnimating();
    void refreshStats();
    void invalidateSceneGraph();

    const Library *m_library = nullptr;
    ThumbnailLoader *m_loader = nullptr;
    FrameStats *m_stats = nullptr;

    // Layout and motion, GUI thread (read by the render thread while the
    // GUI thread is blocked in sync).
    int m_columns = 8;
    double m_scroll = 0;
    double m_scale = 1;
    QPointF m_anchor;
    bool m_pinching = false;

    double m_velocity = 0;      // inertia, px/s
    double m_driveVelocity = 0; // benchmark, px/s
    double m_wheelRemaining = 0;
    double m_snapFrom = 1;      // visual scale settling back to 1
    double m_snapT = 1;
    Layout m_transitionFrom;    // where cells glide from after a column change
    double m_transitionT = 1;

    // Touchpad samples for the release velocity.
    struct WheelSample { qint64 at; double dy; };
    std::vector<WheelSample> m_wheelSamples;
    QElapsedTimer m_clock;

    bool m_animating = false;
    bool m_statsVisible = true;
    QString m_statsText;
    QTimer m_statsTimer;

    // Measured on the render thread.
    double m_speedRows = 0;
    std::atomic<int> m_unresolved{0};
    std::atomic<int> m_textureCount{0};
    std::atomic<qint64> m_textureBytes{0};

    std::unique_ptr<GridRenderState> m_render;
};
