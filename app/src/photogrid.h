#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QTimer>

#include <array>
#include <atomic>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "marks.h"
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
//
// A cell is usually one photo, but a plugin can stack several into one card
// (a series): the grid then lays out cells, each showing the library
// position it stands for, and everything it says to the rest of Pinhole
// (indexAt, rectOf, activated, selection) is in library positions, a
// stacked photo's being its card's. A plugin can also hide photos (as
// Screenshots does): they have no cell at all.
// PINHOLE_INPUT_LOG=<file>: append a line about an input event there.
void logInput(const QString &line);

class PhotoGrid : public QQuickItem {
    Q_OBJECT
    // Cells: photos, a stack counting once.
    Q_PROPERTY(int count READ count NOTIFY cellsChanged)
    // Photos shown as one card, each a list of library positions: the card
    // stands where its first one is and shows it. A photo already in an
    // earlier stack stays there. Plugins' stacks, gathered (Main.qml).
    Q_PROPERTY(QVariantList stacks READ stacks WRITE setStacks NOTIFY stacksChanged)
    // Photos not in the grid at all, library positions: no cell, and out of
    // any stack. Plugins' hidden photos, gathered (Main.qml).
    Q_PROPERTY(QVariantList hidden READ hidden WRITE setHidden NOTIFY hiddenChanged)
    // How many of the library's photos are hidden now: with `count` 0, a
    // library that is not empty but shows nothing.
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY cellsChanged)
    // The kinds of mark there are (Plugins::markKinds), in their order left
    // to right in a cell's corner, each { id, icon, colour, field }: its
    // shape (marks.h: markShape), its colour (a theme's, lit as neon where
    // cells are small and unlit closer), and the library's field that
    // carries it ("video", "live", "favorite"; "stack" for a stack's card),
    // or none for one whose photos a plugin lists (setMarkBits). At most
    // 32, a bit of a word each.
    Q_PROPERTY(QVariantList markKinds READ markKinds WRITE setMarkKinds NOTIFY markKindsChanged)
    // The ids of those drawn: the others are turned off (MarkChoices).
    Q_PROPERTY(QStringList marksShown READ marksShown WRITE setMarksShown NOTIFY marksShownChanged)
    Q_PROPERTY(int columns READ columns NOTIFY columnsChanged)
    Q_PROPERTY(bool animating READ animating NOTIFY animatingChanged)
    Q_PROPERTY(bool statsVisible READ statsVisible WRITE setStatsVisible NOTIFY statsVisibleChanged)
    Q_PROPERTY(QString statsText READ statsText NOTIFY statsTextChanged)
    // How far down the library the view is, 0 at the oldest, 1 at the newest.
    Q_PROPERTY(qreal scrollFraction READ scrollFraction NOTIFY scrolled)
    // Within half a screen of the newest photos.
    Q_PROPERTY(bool atNewest READ atNewest NOTIFY scrolled)
    // The date for the window to show at the top left: far out the year at
    // the top of the view, then the month, then the days on screen, close
    // in the date at the top. While zooming, in the form of the level shown.
    Q_PROPERTY(QString dateLabel READ dateLabel NOTIFY dateLabelChanged)
    // A pinch or zoom step is moving the grid between levels (the date
    // label morphs meanwhile).
    Q_PROPERTY(bool zooming READ zooming NOTIFY zoomingChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    // The selection's border: the Omarchy theme's accent.
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
    // Select mode, entered on purpose (the Select button, Ctrl+A, a Ctrl or
    // Shift click): clicks tick photos instead of opening them. Leaving it
    // clears the selection.
    Q_PROPERTY(bool selecting READ selecting WRITE setSelecting NOTIFY selectingChanged)
    // The window's colour, showing between the cells.
    Q_PROPERTY(QColor background MEMBER m_background NOTIFY backgroundChanged)
    // The keyboard's cursor: a ring around one cell, while the keyboard is
    // in use (`cursorShown`, which the window sets). The arrows move it, the
    // view gliding to keep it in sight, and it is the photo Space, Enter
    // and Ctrl+C take then. Scrolled from by other means, it comes along to
    // the nearest cell in view.
    Q_PROPERTY(bool cursorShown READ cursorShown WRITE setCursorShown NOTIFY cursorShownChanged)
    // Select mode's Range: the next photo tapped ticks every one from the
    // last ticked to it, as a Shift+click does, for a touchpad that cannot
    // drag across them.
    Q_PROPERTY(bool rangeNext MEMBER m_rangeNext NOTIFY rangeNextChanged)
    Q_PROPERTY(QColor cursorColor MEMBER m_cursorColor NOTIFY cursorColorChanged)
    // Something over it has the window (a panel, a card, a menu, the
    // viewer, a plugin's screen): the pointer is not the grid's then, so
    // a wheel, a pinch or a click that something over it let through moves,
    // zooms, selects and opens nothing. A press or a swipe under way when
    // it comes ends there.
    Q_PROPERTY(bool covered READ covered WRITE setCovered NOTIFY coveredChanged)

public:
    explicit PhotoGrid(QQuickItem *parent = nullptr);
    ~PhotoGrid() override;

    void setSources(const Library *library, ThumbnailLoader *loader, FrameStats *stats);
    // The library was read again and its positions moved: `moved[old]` is
    // each photo's new position, or -1 if it is gone. Call right after the
    // swap; the view keeps the photos it showed, or stays at the newest end.
    // `textures` says where each photo's drawn thumbnail goes, as `moved`
    // unless a photo's thumbnail changed (an edit, a video's cover), whose
    // old one must not stay on screen; none means as `moved`.
    void libraryReloaded(const std::vector<int> &moved, const std::vector<int> *textures = nullptr);
    // Where the view is, for keeping it across a reload: the photo at the
    // top-left and how far from the top it is, and whether it is at the end.
    struct Anchor { int index; double offset; bool atEnd; };
    Anchor anchor() const;
    // Note where the view is, just before a reload.
    void rememberAnchor() { m_reloadAnchor = anchor(); }

    // Fingers resting on the touchpad stop a coast, as on a Mac.
    void setTouchpadHold(TouchpadHold *hold);

    int count() const;
    QVariantList stacks() const { return m_stacksGiven; }
    void setStacks(const QVariantList &stacks);
    QVariantList hidden() const { return m_hiddenGiven; }
    void setHidden(const QVariantList &hidden);
    int hiddenCount() const { return m_hiddenCount; }
    // Whether a photo has no cell, hidden by a plugin.
    Q_INVOKABLE bool isHidden(int index) const;
    static constexpr size_t kMaxMarkKinds = 32;
    QVariantList markKinds() const { return m_markKindsGiven; }
    void setMarkKinds(const QVariantList &kinds);
    QStringList marksShown() const { return m_marksShown; }
    void setMarksShown(const QStringList &ids);
    // The marks plugins list by photo: a word for each library position,
    // the bit of each kind (by its place in markKinds) that marks it. Empty
    // for none; read past its end as none.
    void setMarkBits(std::vector<quint32> bits);
    // The ids of the marks a photo's cell carries now, shown ones only, as
    // the grid draws them (on a cell big enough).
    Q_INVOKABLE QStringList marksOn(int index) const;
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
    // An arrow key, pressed or repeating while held: a row (fast, half a
    // screen), then while held a steady glide (fast, speeding up).
    Q_INVOKABLE void scrollByKey(int direction, bool fast);

    // The photo at a point in the grid, or -1; the photo under the pointer,
    // else the one in the middle of the view.
    Q_INVOKABLE int indexAt(qreal x, qreal y) const;
    Q_INVOKABLE int currentIndex() const;
    // Where a photo's cell is on screen (it may be off it), and bringing it
    // into view if it is not. A hidden photo's is the next shown one's (the
    // last's, past the end): where it would be.
    Q_INVOKABLE QRectF rectOf(int index) const;
    // The stack a photo is in, by its card's photo, or -1: the viewer's
    // strip marks a stack's photos.
    Q_INVOKABLE int stackOf(int index) const;
    Q_INVOKABLE void reveal(int index);

    // Selection, as in Photos: click, Ctrl-click to add or remove,
    // Shift-click for a range. A card ticks all of its photos.
    int selectionCount() const;
    bool selecting() const { return m_selecting; }
    void setSelecting(bool selecting);
    bool covered() const { return m_covered; }
    void setCovered(bool covered);
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &accent);
    Q_INVOKABLE QList<int> selection() const;
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();

    bool cursorShown() const { return m_cursorShown; }
    void setCursorShown(bool shown);
    // By photos across and rows down; with `extend`, in select mode, every
    // photo from where the range began to the cursor ticks. A key held
    // moves it on at a steady pace, a row or a photo at a time. The first
    // press after the pointer was used only shows it, where that was.
    Q_INVOKABLE void moveCursor(int dx, int dy, bool extend);
    // A screen's rows up or down.
    Q_INVOKABLE void cursorPages(int pages);
    // To the oldest and the newest photos, the view with it.
    Q_INVOKABLE void cursorToStart();
    Q_INVOKABLE void cursorToEnd();
    // Select mode: the cursor's photo ticked or not.
    Q_INVOKABLE void tickCursor();
    // On the photo the viewer closed on.
    Q_INVOKABLE void putCursorOn(int index);

    // How far down the library the view is (scrollFraction's).
    qreal scrollFraction() const;
    bool atNewest() const;
    bool zooming() const { return m_zoom.active; }
    // Glide to the newest photos: from far off, a jump to a screen and a
    // half before them and the rest eased, so the motion shows without
    // racing through the whole library.
    Q_INVOKABLE void glideToNewest();
    QString dateLabel() const { return m_dateLabel; }

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
    // The cell size drawn now, a pinch's or a settle's on its way (the
    // physics self-test).
    double zoomStride() const { return m_zoom.active ? m_zoom.stride : stride(); }
    // Where the view is, in pixels down the library.
    double scrollPosition() const { return m_scroll; }
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
    // While two levels cross-fade: frames since the last call, and how far
    // apart their column and row lines were at most (0: the cells coincide).
    std::tuple<int, double, double> takeCellOffset() {
        const auto r = std::make_tuple(m_fadeFrames, m_columnOffsetMax, m_rowOffsetMax);
        m_fadeFrames = 0;
        m_columnOffsetMax = m_rowOffsetMax = 0;
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
    void cursorShownChanged();
    void rangeNextChanged();
    void cursorColorChanged();
    void coveredChanged();
    void dateLabelChanged();
    void zoomingChanged();
    // The pointer moved far enough from a press on a photo: drag these out.
    void dragRequested(const QList<int> &indexes);
    void libraryChanged();
    void columnsChanged();
    void animatingChanged();
    void statsVisibleChanged();
    void statsTextChanged();
    void stacksChanged();
    void hiddenChanged();
    void markKindsChanged();
    void marksShownChanged();
    // The cells changed: the library's photos, or how they are stacked.
    void cellsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
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
    };
    std::vector<Layer> layers() const;
    QRectF cellRect(int index, const Layer &layer) const;
    // Cells and library positions: the photo a cell shows, the cell a photo
    // is in (its card's, when stacked; -1 when hidden), the cell at a point,
    // and a cell's photos (one, or a stack's).
    int libraryAt(int cell) const { return m_cellOf.empty() ? cell : m_cells[size_t(cell)]; }
    int cellOf(int index) const;
    // A photo's cell, or a hidden one's next shown; -1 with no cells.
    int nearestCell(int index) const;
    int cellAt(qreal x, qreal y) const;
    void appendPhotos(int cell, QList<int> &out) const;
    QList<int> photosOf(int cell) const;
    // Lay the cells out again from the stacks and hidden photos given,
    // positions mapped through `moved` (a reload's) first when given; the
    // selection and the photo at the top of the view stay with their photos.
    void rebuildCells(const std::vector<int> *moved = nullptr);
    // The stacks or the hidden photos given changed: the cells again, the
    // view staying on the photo at its top.
    void relayCells();
    double strideFor(int columns) const;
    double maxScroll(int columns) const;
    // Put the view at `y`, within the ends, stopping any motion.
    void setScroll(double y);
    void syncBounds();
    void updateAnimating();
    // Whether a level is far enough out for the marks to be neon,
    // and noting when the grid came to rest at such a level, from which
    // the tubes strike one by one (-1: not lit).
    bool neonAt(int columns) const;
    void updateNeon();
    double m_neonSince = -1; // s, on m_neonClock
    // Seconds of animation frames, the strike's clock.
    double m_neonClock = 0;
    void refreshStats();
    void invalidateSceneGraph();
    void touched(double timeMs, const char *how);

    // Zoom
    int levelForColumns(int columns) const;
    // The level whose own cell size is nearest `stride`, as a ratio.
    int nearestLevel(double stride) const;
    // Pin the photo under `at` (view coordinates) and start zooming around it.
    void beginZoom(QPointF at);
    // The cell size the zoom shows now, and where the pinned photo sits.
    void zoomTo(double stride, QPointF at);
    void wheelZoom(double notches, QPointF at);
    // The wheel still: on to the next level the way it turned.
    void endWheelZoom();
    // Settle on `level`: scale, position and fade animate there.
    void settleZoom(int level);
    // Content position of the pinned point in a grid `columns` wide.
    QPointF pinIn(int columns) const;

    const Library *m_library = nullptr;
    ThumbnailLoader *m_loader = nullptr;
    // Cell to library position, and back (-1: hidden); both empty while
    // every cell is one photo. Each stack's photos by its card's position,
    // the card's first.
    std::vector<int> m_cells;
    std::vector<int> m_cellOf;
    std::unordered_map<int, std::vector<int>> m_stackOf;
    QVariantList m_stacksGiven;
    std::vector<std::vector<int>> m_stackLists; // as given, through any reload since
    QVariantList m_hiddenGiven;
    std::vector<int> m_hiddenList; // as given, through any reload since
    int m_hiddenCount = 0;
    // The kinds of mark as given, and as drawn: each one's shape and
    // colour, in the atlas in this order. Counted up on every change of
    // shapes or colours, for the render thread to draw them again.
    struct MarkKind {
        QString id;
        MarkShape shape;
        QColor colour;
        bool operator==(const MarkKind &) const = default;
    };
    QVariantList m_markKindsGiven;
    QStringList m_marksShown;
    std::vector<MarkKind> m_markKinds;
    int m_markKindsVersion = 0;
    // Which kinds' bits a field of the library or a stack sets, and which
    // are drawn: worked out when the kinds change, not for each cell.
    quint32 m_videoMask = 0, m_liveMask = 0, m_favoriteMask = 0, m_stackMask = 0, m_shownMask = 0;
    void updateMarkMasks();
    std::vector<quint32> m_markBits;
    FrameStats *m_stats = nullptr;
    TouchpadHold *m_hold = nullptr;
    QPointF m_hover{-1, -1};
    std::set<int> m_selected;
    int m_selectionAnchor = -1;
    // The cursor's cell, -1 for none; whether it is moving the view now
    // (else it follows the view); and when a held key last moved it.
    int m_cursor = -1;
    bool m_rangeNext = false;
    bool m_cursorShown = false;
    bool m_cursorLeads = false;
    double m_cursorPressed = -1, m_cursorStepped = -1;
    QColor m_cursorColor{0xc0, 0xca, 0xf5};
    void revealCell(int cell);
    void keepCursorInView();
    // The cell the pointer is over, else the middle one in view.
    int pointedCell() const;
    QPointF m_pressAt;
    int m_pressIndex = -1;
    bool m_dragged = false;
    bool m_selecting = false;
    bool m_covered = false;
    // A drag across the photos in select mode (three fingers, with
    // libinput's three-finger drag): every photo from where it started to
    // where it is now ticks, on top of what was ticked before. Near the top
    // or bottom of the view the grid scrolls on.
    bool m_sweeping = false;
    std::set<int> m_sweepBase;
    QPointF m_sweepAt;
    void sweep();
    void endSweep();
    void updateDateLabel();
    QString m_dateLabel;
    // The press stopped the grid moving: its click opens nothing.
    bool m_pressStopped = false;
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
    // Ctrl+wheel notches not yet used; high-resolution wheels send parts.
    double m_zoomNotches = 0;

    // Layout and motion, GUI thread (read by the render thread while the
    // GUI thread is blocked in sync).
    int m_level = 2; // 7 columns, kDefaultLevel
    int m_columns = 8;
    double m_scroll = 0;

    ScrollPhysics m_physics;
    double m_lastKeyScroll = -1; // s, on m_clock
    double m_keyHeldSince = 0;
    double m_driveVelocity = 0; // the benchmark's, or a sweep's at an edge; px/s

    struct Zoom {
        bool active = false;   // pinching or settling
        bool settling = false;
        int index = 0;         // the pinned photo
        QPointF fraction;      // where in it the fingers are, 0..1
        double stride = 0;     // cell size shown now
        QPointF at;            // where the pinned point is shown now
        // Across, what every level grows about: the window's middle or an
        // edge, for the whole zoom.
        double focusX = 0;
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
        // Pinching: the level shown (the nearest to the cell size), the one
        // it took over from, and how far the cross-fade between them is.
        int shown = 0;
        int fadeFrom = -1;
        double fade = 1;
        double toScroll = 0;
        double t = 1;
    } m_zoom;
    // A horizontal wheel zooming as a pinch does: the log of the cell size
    // shown and the one its notches add up to, the level the turn began
    // at, and when the last notch came (s, on m_clock). Spent: this turn
    // has gone its one level, and its notches count for nothing until the
    // wheel rests.
    struct WheelZoom {
        bool active = false;
        bool spent = false;
        int from = 0;
        double current = 0, target = 0;
        double lastAt = -1;
    } m_wheelZoom;

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
    int m_fadeFrames = 0;
    double m_columnOffsetMax = 0;
    double m_rowOffsetMax = 0;
    std::atomic<int> m_unresolved{0};
    std::atomic<int> m_mediumNeeded{0};
    std::atomic<int> m_mediumShown{0};
    std::atomic<int> m_textureCount{0};
    std::atomic<qint64> m_textureBytes{0};

    std::unique_ptr<GridRenderState> m_render;
};
