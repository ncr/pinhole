#include "photogrid.h"

#include "framestats.h"
#include "library.h"
#include "thumbnailloader.h"
#include "touchpadhold.h"

#include <QDateTime>
#include <QFile>
#include <QInputDevice>
#include <QNativeGestureEvent>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSGVertexColorMaterial>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace {

// The zoom levels, as column counts. Neighbours are about 1.4x apart: close
// enough that a cross-fade between two of them reads as one motion.
constexpr std::array<int, 11> kLevels = {1, 2, 3, 4, 6, 8, 11, 15, 21, 29, 40};
constexpr int kDefaultLevel = 5;
// Seconds for a zoom to settle after the fingers lift, or for a zoom step.
constexpr double kSettleSeconds = 0.3;
// A pinch passing from one level to the next cross-fades in this long, as
// Photos on an iPhone does (3-4 frames at 30 fps in a recording of it),
// a little longer here for the light morph to show.
constexpr double kReflowSeconds = 0.15;
// How far toward the other level's places photos glide while two levels
// cross-fade: a light morph, not a full one.
constexpr double kMorph = 0.35;
// A pinch released while still moving goes on this long, in seconds, when
// picking the level it lands on: a quick pinch commits.
constexpr double kPinchProjection = 0.12;
// Texture data uploaded per frame, at most, and the time that may take.
// More and a frame could miss its vsync. Counted in bytes, not textures: a
// 1/8 thumbnail is 64 times cheaper than a full one.
const qint64 kUploadBytesPerFrame = qint64(qEnvironmentVariableIntValue("PINHOLE_UPLOAD_MB") > 0
                                               ? qEnvironmentVariableIntValue("PINHOLE_UPLOAD_MB")
                                               : 8)
                                    << 20;
constexpr qint64 kUploadNanos = 3'000'000;
// Cells this small (device pixels) are drawn from mosaic pages rather than
// their own thumbnails; see thumbnailloader.h. Pages kept on the GPU, at
// 4 MB each.
constexpr double kMosaicBelowPx = 100;
constexpr size_t kMaxPages = 64;

double easeOut(double t) { return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), 3); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
QPointF lerp(QPointF a, QPointF b, double t) { return a + (b - a) * t; }

// A hairline gap between cells, wider when cells are big.
double gapFor(int columns) { return columns <= 12 ? 2 : 1; }

// Texture coordinates of the centred square of a w x h image.
QRectF squareCrop(int w, int h) {
    if (w <= 0 || h <= 0)
        return {0, 0, 1, 1};
    if (w > h) {
        const double f = double(h) / w;
        return {(1 - f) / 2, 0, f, 1};
    }
    const double f = double(w) / h;
    return {0, (1 - f) / 2, 1, f};
}

} // namespace

// PINHOLE_INPUT_LOG=<file>: every scroll and gesture event as it arrives,
// for tuning scrolling against what the touchpad really sends.
void logInput(const QString &line) {
    static QFile *file = [] {
        const QString path = qEnvironmentVariable("PINHOLE_INPUT_LOG");
        if (path.isEmpty())
            return static_cast<QFile *>(nullptr);
        auto *f = new QFile(path);
        return f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text) ? f : (delete f, nullptr);
    }();
    if (!file)
        return;
    static QElapsedTimer clock = [] { QElapsedTimer t; t.start(); return t; }();
    file->write(QStringLiteral("%1 %2\n").arg(clock.nsecsElapsed() / 1e6, 10, 'f', 2).arg(line).toUtf8());
    file->flush();
}

// Everything the render thread owns.
struct GridRenderState {
    struct Entry {
        QSGTexture *texture[4] = {};
        QSize size[4];
        // The medium rendition, for cells bigger than the thumbnail.
        QSGTexture *medium = nullptr;
        QSize mediumSize;
        int mediumLevel = 4;
        quint64 lastUsed = 0;
    };
    std::unordered_map<int, Entry> cache;
    struct Page {
        QSGTexture *texture = nullptr;
        quint64 lastUsed = 0;
    };
    std::unordered_map<int, Page> pages;
    qint64 bytes = 0;
    int textures = 0;
    quint64 frame = 0;

    // Two drawing slots: the main layer, and during a zoom the one faded
    // over it. Each is one node per texture (atlas page), keyed by the
    // texture's comparison key, plus one for the colour placeholders.
    struct Slot {
        QSGOpacityNode *opacity = nullptr;
        QSGGeometryNode *placeholders = nullptr;
        std::unordered_map<qint64, QSGGeometryNode *> groups;
        // The cells' rounded corners: the background drawn over each
        // corner outside its arc, above the photos of the same layer.
        QSGGeometryNode *corners = nullptr;
    };
    Slot drawSlots[2];

    // Borders around selected photos, one node on top of everything.
    QSGGeometryNode *selection = nullptr;

    double lastScroll = 0;
    qint64 lastFrameAt = 0;

    void dropAll() {
        for (auto &[index, entry] : cache) {
            for (QSGTexture *t : entry.texture)
                delete t;
            delete entry.medium;
        }
        cache.clear();
        for (auto &[index, page] : pages)
            delete page.texture;
        pages.clear();
        bytes = 0;
        textures = 0;
    }
    ~GridRenderState() { dropAll(); }
};

PhotoGrid::PhotoGrid(QQuickItem *parent)
    : QQuickItem(parent), m_render(std::make_unique<GridRenderState>()) {
    if (const double gain = qEnvironmentVariable("PINHOLE_SCROLL_GAIN").toDouble(); gain > 0)
        m_physics.params().trackingGain = gain;
    m_level = kDefaultLevel;
    m_columns = kLevels[size_t(m_level)];
    setFlag(ItemHasContents);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setClip(true);
    m_clock.start();
    m_statsTimer.setInterval(250);
    connect(&m_statsTimer, &QTimer::timeout, this, &PhotoGrid::refreshStats);
    m_statsTimer.start();
}

PhotoGrid::~PhotoGrid() = default;

void PhotoGrid::setSources(const Library *library, ThumbnailLoader *loader, FrameStats *stats) {
    m_library = library;
    m_loader = loader;
    m_stats = stats;
    connect(loader, &ThumbnailLoader::ready, this, &QQuickItem::update, Qt::QueuedConnection);
    emit libraryChanged();
    scrollToEnd();
}

void PhotoGrid::setTouchpadHold(TouchpadHold *hold) {
    m_hold = hold;
    connect(hold, &TouchpadHold::began, this, [this](quint32 time, quint32 fingers) {
        logInput(QStringLiteral("hold begin time=%1 fingers=%2").arg(time).arg(fingers));
        touched(time, "hold");
    });
    connect(hold, &TouchpadHold::ended, this, [](quint32 time, bool cancelled) {
        logInput(QStringLiteral("hold end time=%1 cancelled=%2").arg(time).arg(cancelled));
    });
}

PhotoGrid::Anchor PhotoGrid::anchor() const {
    const int index = indexAt(0, 0);
    return {index, index >= 0 ? rectOf(index).top() : 0, m_scroll >= maxScroll(m_columns) - stride()};
}

void PhotoGrid::libraryReloaded(const std::vector<int> &moved) {
    const Anchor before = m_reloadAnchor;
    auto mapped = [&](int old) { return old >= 0 && old < int(moved.size()) ? moved[size_t(old)] : -1; };

    std::set<int> selected;
    for (int i : m_selected)
        if (const int n = mapped(i); n >= 0)
            selected.insert(n);
    const bool selectionChanged = selected != m_selected;
    m_selected = std::move(selected);
    m_selectionAnchor = mapped(m_selectionAnchor);
    m_zoom = {};
    m_pendingMoves = std::make_shared<std::vector<int>>(moved);
    emit libraryChanged();

    // At the newest end it stays there, so new photos come into view;
    // elsewhere the photo that was at the top stays where it was, or the
    // next one that survived.
    if (before.atEnd) {
        scrollToEnd();
    } else {
        int index = -1;
        for (int old = before.index; old >= 0 && old < int(moved.size()) && index < 0; ++old)
            index = mapped(old);
        if (index >= 0)
            setScroll(m_scroll + rectOf(index).top() - before.offset);
        else
            setScroll(m_scroll);
    }
    if (selectionChanged)
        emit this->selectionChanged();
    update();
}

void PhotoGrid::touched(double timeMs, const char *) {
    if (timeMs < m_coastFrom)
        return;
    m_physics.touch(timeMs);
    m_scroll = m_physics.position();
    updateAnimating();
    update();
}

void PhotoGrid::mouseDoubleClickEvent(QMouseEvent *event) {
    // The first click opened the photo already.
    event->accept();
}

void PhotoGrid::hoverMoveEvent(QHoverEvent *event) { m_hover = event->position(); }
void PhotoGrid::hoverLeaveEvent(QHoverEvent *) { m_hover = QPointF(-1, -1); }

int PhotoGrid::indexAt(qreal x, qreal y) const {
    if (x < 0 || y < 0 || x >= width() || y >= height() || count() == 0)
        return -1;
    const double s = stride();
    const int col = std::clamp(int(x / s), 0, m_columns - 1);
    const int row = int((y + m_scroll) / s);
    const int index = row * m_columns + col;
    return index < count() ? index : -1;
}

int PhotoGrid::currentIndex() const {
    if (m_hover.x() >= 0) {
        if (const int i = indexAt(m_hover.x(), m_hover.y()); i >= 0)
            return i;
    }
    const int middle = indexAt(width() / 2, height() / 2);
    return middle >= 0 ? middle : count() - 1;
}

QRectF PhotoGrid::rectOf(int index) const {
    return cellRect(index, Layer{m_columns, 1, QPointF(0, m_scroll), QPointF(0, 0), 1});
}

void PhotoGrid::reveal(int index) {
    const QRectF r = rectOf(index);
    if (r.top() >= 0 && r.bottom() <= height())
        return;
    // Into the middle of the view, as Photos does when a viewer closes on a
    // photo that has scrolled away.
    setScroll(m_scroll + r.center().y() - height() / 2);
}

void PhotoGrid::mousePressEvent(QMouseEvent *event) {
    // A click, or a tap with tap-to-click, is a touch too.
    m_pressStopped = m_physics.animating() || m_zoom.active;
    touched(double(event->timestamp()), "click");
    m_pressAt = event->position();
    m_pressIndex = indexAt(event->position().x(), event->position().y());
    m_dragged = false;
    event->accept();
    if (event->button() == Qt::RightButton) {
        const int index = m_pressIndex;
        m_pressIndex = -1;
        emit menuRequested(index, event->position());
    }
}

void PhotoGrid::mouseMoveEvent(QMouseEvent *event) {
    if (m_dragged || m_pressIndex < 0 || !(event->buttons() & Qt::LeftButton))
        return;
    if ((event->position() - m_pressAt).manhattanLength() < 6)
        return;
    m_dragged = true;
    // A photo in the selection drags the selection; any other, itself.
    emit dragRequested(m_selected.contains(m_pressIndex) ? selection() : QList<int>{m_pressIndex});
}

void PhotoGrid::mouseReleaseEvent(QMouseEvent *event) {
    if (m_dragged || event->button() != Qt::LeftButton)
        return;
    const int index = m_pressIndex;
    const auto mods = event->modifiers();
    if (index < 0)
        return; // between photos: nothing, a selection included
    // In select mode a click ticks a photo, Shift+click a range; a Ctrl or
    // Shift click outside it enters it. Otherwise a click opens.
    if (mods & (Qt::ControlModifier | Qt::ShiftModifier))
        setSelecting(true);
    if (m_selecting && !(mods & Qt::ShiftModifier)) {
        if (!m_selected.erase(index))
            m_selected.insert(index);
        m_selectionAnchor = index;
    } else if (mods & Qt::ShiftModifier) {
        const int anchor = m_selectionAnchor >= 0 ? m_selectionAnchor : index;
        for (int i = std::min(index, anchor); i <= std::max(index, anchor); ++i)
            m_selected.insert(i);
        m_selectionAnchor = index;
    } else {
        // A click or a tap opens the photo, as in Photos; one that only
        // stopped the grid coasting does not.
        if (!m_pressStopped)
            emit activated(index, rectOf(index));
        return;
    }
    emit selectionChanged();
    update();
}

void PhotoGrid::setAccent(const QColor &accent) {
    if (accent == m_accent)
        return;
    m_accent = accent;
    emit accentChanged();
    update();
}

QList<int> PhotoGrid::selection() const { return QList<int>(m_selected.begin(), m_selected.end()); }

void PhotoGrid::setSelecting(bool selecting) {
    if (selecting == m_selecting)
        return;
    m_selecting = selecting;
    if (!selecting)
        clearSelection();
    emit selectingChanged();
    update();
}

void PhotoGrid::selectAll() {
    setSelecting(true);
    m_selected.clear();
    for (int i = 0; i < count(); ++i)
        m_selected.insert(m_selected.end(), i);
    emit selectionChanged();
    update();
}

void PhotoGrid::clearSelection() {
    if (m_selected.empty())
        return;
    m_selected.clear();
    m_selectionAnchor = -1;
    emit selectionChanged();
    update();
}

int PhotoGrid::count() const { return m_library ? m_library->count() : 0; }

void PhotoGrid::setStatsVisible(bool visible) {
    if (visible == m_statsVisible)
        return;
    m_statsVisible = visible;
    emit statsVisibleChanged();
}

int PhotoGrid::levelCount() { return int(kLevels.size()); }
int PhotoGrid::columnsAt(int level) { return kLevels[size_t(std::clamp(level, 0, levelCount() - 1))]; }

// Layout

double PhotoGrid::strideFor(int columns) const { return width() / std::max(1, columns); }
double PhotoGrid::stride() const { return strideFor(m_columns); }

double PhotoGrid::maxScroll(int columns) const {
    const double s = strideFor(columns);
    const int rows = (count() + columns - 1) / std::max(1, columns);
    return std::max(0.0, rows * s - height());
}

void PhotoGrid::syncBounds() { m_physics.setBounds(0, maxScroll(m_columns), height()); }

void PhotoGrid::setScroll(double y) {
    syncBounds();
    m_scroll = std::clamp(y, 0.0, maxScroll(m_columns));
    m_physics.jumpTo(m_scroll);
    updateAnimating();
    update();
    notifyScrolled();
}

void PhotoGrid::notifyScrolled() {
    const qreal f = scrollFraction();
    if (std::abs(f - m_lastFraction) > 1e-6) {
        m_lastFraction = f;
        emit scrolled();
    }
}

qreal PhotoGrid::scrollFraction() const {
    const double max = maxScroll(m_columns);
    return max > 0 ? std::clamp(m_scroll / max, 0.0, 1.0) : 1.0;
}

void PhotoGrid::scrubTo(qreal fraction) { setScroll(std::clamp(fraction, 0.0, 1.0) * maxScroll(m_columns)); }

qint64 PhotoGrid::dateAt(qreal fraction) const {
    if (count() == 0)
        return 0;
    // The photo at the top of the view when scrolled there.
    const double y = std::clamp(fraction, 0.0, 1.0) * maxScroll(m_columns);
    const int index = std::clamp(int(y / stride()) * m_columns, 0, count() - 1);
    return m_library->at(index).date;
}

QVariantList PhotoGrid::yearMarks() const {
    QVariantList marks;
    const double max = maxScroll(m_columns);
    if (count() == 0 || max <= 0)
        return marks;
    // The library is oldest first, so each year's first photo is found by
    // halving: a few hundred dates looked at rather than every one, which
    // took 140 ms at 100,000 photos on every zoom step.
    const auto dateOf = [this](int i) { return m_library->at(i).date; };
    const int first = QDateTime::fromMSecsSinceEpoch(dateOf(0)).date().year();
    const int last = QDateTime::fromMSecsSinceEpoch(dateOf(count() - 1)).date().year();
    for (int y = first; y <= last; ++y) {
        const qint64 starts = QDateTime(QDate(y, 1, 1), QTime(0, 0)).toMSecsSinceEpoch();
        int lo = 0, hi = count(); // the first photo taken on or after `starts`
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (dateOf(mid) < starts)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == count() || (y > first && QDateTime::fromMSecsSinceEpoch(dateOf(lo)).date().year() != y))
            continue; // no photos that year
        const double top = (y == first ? 0 : lo / m_columns) * stride();
        marks.append(QVariantMap{{"year", y}, {"fraction", std::clamp(top / max, 0.0, 1.0)}});
    }
    return marks;
}

void PhotoGrid::setScrubbing(bool scrubbing) {
    m_scrubbing = scrubbing;
    update();
}

QRectF PhotoGrid::cellRect(int index, const Layer &l) const {
    auto place = [this, index](int columns, double scale, QPointF pin, QPointF at) {
        const double s = strideFor(columns);
        const double size = (s - gapFor(columns)) * scale;
        const double x = (index % columns) * s;
        const double y = (index / columns) * s;
        return QRectF(at.x() + (x - pin.x()) * scale, at.y() + (y - pin.y()) * scale, size, size);
    };
    const QRectF here = place(l.columns, l.scale, l.pin, l.at);
    if (l.morph <= 0)
        return here;
    const QRectF there = place(l.toColumns, l.toScale, l.toPin, l.toAt);
    const double m = l.morph;
    return {lerp(here.x(), there.x(), m), lerp(here.y(), there.y(), m), lerp(here.width(), there.width(), m),
            lerp(here.height(), there.height(), m)};
}

QPointF PhotoGrid::pinIn(int columns) const {
    const double s = strideFor(columns);
    return {(m_zoom.index % columns + m_zoom.fraction.x()) * s, (m_zoom.index / columns + m_zoom.fraction.y()) * s};
}

int PhotoGrid::levelForColumns(int columns) const {
    int best = 0;
    for (int i = 0; i < levelCount(); ++i)
        if (std::abs(kLevels[size_t(i)] - columns) < std::abs(kLevels[size_t(best)] - columns))
            best = i;
    return best;
}

// What is drawn now: the grid at rest, or during a zoom the two levels
// around the current cell size, the stronger one first.
std::vector<PhotoGrid::Layer> PhotoGrid::layers() const {
    if (!m_zoom.active)
        return {Layer{m_columns, 1, QPointF(0, m_scroll), QPointF(0, 0), 1}};

    auto layerFor = [&](int level, double weight) {
        const int columns = kLevels[size_t(level)];
        const double scale = m_zoom.stride / strideFor(columns);
        const QPointF pin = pinIn(columns);
        // Vertically the pinned photo stays under the fingers. Across, every
        // level spans the window and grows or shrinks about the fingers'
        // x: photos left of them move left, those right of them right, and
        // the grid never slides. (Holding the pinned photo under the fingers
        // across as well cannot work: in each level it sits in another
        // column, so the grid slid one way at each change of level and back
        // as the zoom went on.) Scaled up, both edges stay outside the
        // view; scaled down (only the faded one on top), inside.
        const double fx = std::clamp(m_zoom.focusX, 0.0, width());
        const double left = fx * (1 - scale);
        // Down the window the same, near the library's ends: a level tall
        // enough to fill the window is kept filling it, rather than holding
        // the pinned photo's height and showing empty space past the first or
        // last row.
        const double tall = (count() + columns - 1) / columns * strideFor(columns) * scale;
        double top = m_zoom.at.y() - pin.y() * scale;
        top = tall >= height() ? std::clamp(top, height() - tall, 0.0) : 0.0;
        return Layer{columns, scale, pin, QPointF(left + pin.x() * scale, top + pin.y() * scale), weight};
    };
    // Two levels at one cell size: the denser is scaled up and always covers
    // the view, the coarser is scaled down and leaves gaps at the edges. So
    // the denser is drawn whole underneath and the coarser faded over it:
    // its share over the rest blends just like two half-transparent layers,
    // without the background ever showing through.
    // `a` is the level coming in, `aWeight` how far along; `b` going out.
    auto pair = [&](int a, double aWeight, int b) -> std::vector<Layer> {
        const bool aDenser = kLevels[size_t(a)] > kLevels[size_t(b)];
        const int dense = aDenser ? a : b, coarse = aDenser ? b : a;
        const double coarseWeight = aDenser ? 1 - aWeight : aWeight;
        if (coarseWeight >= 0.999)
            return {layerFor(coarse, 1)};
        if (coarseWeight <= 0.001)
            return {layerFor(dense, 1)};
        std::vector<Layer> both{layerFor(dense, 1), layerFor(coarse, coarseWeight)};
        // The light morph: the incoming level starts a little toward where
        // the outgoing one has each photo and settles into its own places;
        // the outgoing one leans a little toward the new places as it goes.
        Layer &in = aDenser ? both[0] : both[1];
        Layer &out = aDenser ? both[1] : both[0];
        auto toward = [](Layer &l, const Layer &other, double m) {
            l.morph = m;
            l.toColumns = other.columns;
            l.toScale = other.scale;
            l.toPin = other.pin;
            l.toAt = other.at;
        };
        const Layer inPlain = in, outPlain = out;
        toward(in, outPlain, (1 - aWeight) * kMorph);
        toward(out, inPlain, aWeight * kMorph);
        return both;
    };
    if (m_zoom.settling) {
        // The cross-fade done in the first part of the settle: briefly two
        // layouts at once, then one moving into place.
        const double w = lerp(m_zoom.fromWeight, 1, easeOut(std::min(1.0, m_zoom.t / 0.4)));
        if (m_zoom.partner == m_zoom.target)
            return {layerFor(m_zoom.target, 1)};
        return pair(m_zoom.target, w, m_zoom.partner);
    }

    // Pinching: one level, scaled; while the level changes, the old one
    // fading out over a tenth of a second (pinchLevel(), advance()).
    if (m_zoom.fade < 1 && m_zoom.fadeFrom >= 0 && m_zoom.fadeFrom != m_zoom.shown)
        return pair(m_zoom.shown, easeOut(m_zoom.fade), m_zoom.fadeFrom);
    return {layerFor(m_zoom.shown, 1)};
}

int PhotoGrid::pinchLevel(double stride) const {
    // The denser of the two levels whose cell sizes are either side of
    // `stride`: drawn scaled up, it always covers the window. Strides shrink
    // as the column count grows.
    const int last = levelCount() - 1;
    if (stride >= strideFor(kLevels.front()))
        return 0;
    int level = 1;
    while (level < last && stride < strideFor(kLevels[size_t(level)]))
        level += 1;
    return level;
}

void PhotoGrid::beginZoom(QPointF at) {
    m_physics.stop();
    m_driveVelocity = 0;
    // Find the photo under `at` in whatever is shown now (a settle that is
    // still running counts: the zoom continues from where it is).
    const std::vector<Layer> shown = layers();
    const Layer &l = shown.front();
    const double s = strideFor(l.columns);
    const QPointF content = l.pin + (at - l.at) / l.scale;
    const int col = std::clamp(int(std::floor(content.x() / s)), 0, l.columns - 1);
    const int row = std::max(0, int(std::floor(content.y() / s)));
    int index = row * l.columns + col;
    QPointF fraction(content.x() / s - col, content.y() / s - row);
    if (index >= count()) {
        // Past the last photo: pin the last one, at the same point in it.
        index = std::max(0, count() - 1);
    }
    fraction = QPointF(std::clamp(fraction.x(), 0.0, 1.0), std::clamp(fraction.y(), 0.0, 1.0));

    m_zoom.active = true;
    m_zoom.settling = false;
    m_zoom.index = index;
    m_zoom.fraction = fraction;
    m_zoom.stride = s * l.scale;
    m_zoom.startStride = m_zoom.stride;
    m_zoom.pinchScale = 1;
    m_zoom.samples.clear();
    // The pinned point where it is drawn now (not quite `at` when the
    // fingers are past the last photo), so taking over moves nothing.
    m_zoom.at = l.at + (pinIn(l.columns) - l.pin) * l.scale;
    m_zoom.pinStart = m_zoom.at;
    m_zoom.fingerStart = at;
    m_zoom.focusX = at.x();
    m_zoom.shown = levelForColumns(l.columns);
    m_zoom.fadeFrom = -1;
    m_zoom.fade = 1;
    m_pinTracked = false; // a new photo is pinned: start measuring afresh
    updateAnimating();
}

void PhotoGrid::zoomTo(double stride, QPointF at) {
    // Past the closest level the zoom gives a little, like a rubber band,
    // and springs back on release.
    const double finest = strideFor(kLevels.back());
    const double coarsest = strideFor(kLevels.front());
    // Zooming out stops at the last level: shrinking past it would leave
    // the window's edges empty.
    if (stride < finest)
        stride = finest;
    else if (stride > coarsest)
        stride = coarsest * std::pow(stride / coarsest, 0.3);
    m_zoom.stride = stride;
    // Past a level's own cell size the grid reflows to the next one, as in
    // Photos: one quick cross-fade, however slowly the fingers move, rather
    // than the two levels blended all the way between them.
    if (const int level = pinchLevel(stride); level != m_zoom.shown) {
        m_zoom.fadeFrom = m_zoom.shown;
        m_zoom.shown = level;
        m_zoom.fade = 0;
        updateAnimating();
    }
    // Moving fingers pan: the pinned point follows them.
    m_zoom.at = m_zoom.pinStart + (at - m_zoom.fingerStart);
    m_zoom.focusX = at.x();
    const double now = m_clock.nsecsElapsed() / 1e9;
    m_zoom.samples.emplace_back(now, std::log(stride));
    while (m_zoom.samples.size() > 2 && now - m_zoom.samples.front().first > 0.1)
        m_zoom.samples.erase(m_zoom.samples.begin());
    update();
}

void PhotoGrid::settleZoom(int level) {
    level = std::clamp(level, 0, levelCount() - 1);
    const std::vector<Layer> shown = layers();
    const int columns = kLevels[size_t(level)];
    // The partner is whichever other level is showing, fading out; the
    // target's share is its opacity if it is the faded one on top, or what
    // the faded one leaves if it is the base.
    int partner = level;
    double weight = 0;
    for (size_t k = 0; k < shown.size(); ++k) {
        if (shown[k].columns == columns)
            weight = k == 0 ? (shown.size() > 1 ? 1 - shown[1].opacity : 1) : shown[k].opacity;
        else
            partner = levelForColumns(shown[k].columns);
    }

    // The pinned photo keeps its height on screen; across it lands in its
    // own column. Near the ends the scroll limit has the last word.
    const QPointF pin = pinIn(columns);
    const double scroll = std::clamp(pin.y() - m_zoom.at.y(), 0.0, maxScroll(columns));

    m_zoom.settling = true;
    m_zoom.target = level;
    m_zoom.partner = partner;
    m_zoom.fromStride = m_zoom.stride;
    m_zoom.toStride = strideFor(columns);
    m_zoom.fromAt = m_zoom.at;
    m_zoom.toAt = QPointF(pin.x(), pin.y() - scroll);
    m_zoom.fromWeight = weight;
    m_zoom.toScroll = scroll;
    m_zoom.t = 0;
    updateAnimating();
    update();
}

void PhotoGrid::zoomStep(int delta) {
    const QPointF at = isUnderMouse() ? mapFromGlobal(QCursor::pos()) : QPointF(width() / 2, height() / 2);
    const int from = m_zoom.active ? m_zoom.target : m_level;
    beginZoom(at);
    settleZoom(from + delta);
}

void PhotoGrid::setEffectiveStride(double stride, QPointF anchor) {
    if (!m_zoom.active || m_zoom.settling)
        beginZoom(anchor);
    zoomTo(stride, anchor);
}

void PhotoGrid::endContinuousZoom() {
    if (!m_zoom.active || m_zoom.settling)
        return;
    // Where the pinch was heading: its speed at release, carried on briefly.
    double logStride = std::log(m_zoom.stride);
    if (m_zoom.samples.size() >= 2) {
        const auto &[t0, l0] = m_zoom.samples.front();
        const auto &[t1, l1] = m_zoom.samples.back();
        if (t1 - t0 > 0.01)
            logStride += (l1 - l0) / (t1 - t0) * kPinchProjection;
    }
    // Land on the level nearest that, but never more than one level past
    // the two being cross-faded.
    const std::vector<Layer> shown = layers();
    int lo = levelCount() - 1, hi = 0;
    for (const Layer &l : shown) {
        lo = std::min(lo, levelForColumns(l.columns));
        hi = std::max(hi, levelForColumns(l.columns));
    }
    lo = std::max(0, lo - 1);
    hi = std::min(levelCount() - 1, hi + 1);
    int best = lo;
    for (int i = lo; i <= hi; ++i)
        if (std::abs(std::log(strideFor(kLevels[size_t(i)])) - logStride) <
            std::abs(std::log(strideFor(kLevels[size_t(best)])) - logStride))
            best = i;
    settleZoom(best);
}

void PhotoGrid::setColumnsNow(int columns) {
    const double fraction = maxScroll(m_columns) > 0 ? m_scroll / maxScroll(m_columns) : 1;
    m_zoom = {};
    m_level = levelForColumns(columns);
    m_columns = kLevels[size_t(m_level)];
    setScroll(fraction * maxScroll(m_columns));
    emit columnsChanged();
}

void PhotoGrid::scrollToStart() { setScroll(0); }

void PhotoGrid::scrollToEnd() { setScroll(maxScroll(m_columns)); }

void PhotoGrid::scrollPages(qreal pages) {
    syncBounds();
    m_physics.nudge(pages * height() * 0.9);
    updateAnimating();
}

void PhotoGrid::setDriveVelocity(double v) {
    m_driveVelocity = v;
    m_physics.jumpTo(m_scroll);
    updateAnimating();
}

void PhotoGrid::fling(double v) {
    syncBounds();
    m_physics.fling(v);
    updateAnimating();
}

void PhotoGrid::jumpToFraction(double f) { setScroll(std::clamp(f, 0.0, 1.0) * maxScroll(m_columns)); }

// Motion

void PhotoGrid::advance(qreal dt) {
    dt = std::clamp(dt, 0.0, 0.05);
    if (m_driveVelocity != 0) {
        setScroll(m_scroll + m_driveVelocity * dt);
    } else if (m_physics.animating()) {
        if (m_hold)
            m_hold->poll();
        m_physics.step(dt);
        m_scroll = m_physics.position();
    }
    if (m_zoom.active && !m_zoom.settling && m_zoom.fade < 1)
        m_zoom.fade = std::min(1.0, m_zoom.fade + dt / kReflowSeconds);
    if (m_zoom.active && m_zoom.settling) {
        m_zoom.t = std::min(1.0, m_zoom.t + dt / kSettleSeconds);
        const double e = easeOut(m_zoom.t);
        // Scale moves as a ratio, so zooming in and out feel the same speed.
        m_zoom.stride = std::exp(lerp(std::log(m_zoom.fromStride), std::log(m_zoom.toStride), e));
        m_zoom.at = lerp(m_zoom.fromAt, m_zoom.toAt, e);
        if (m_zoom.t >= 1) {
            const bool changed = m_level != m_zoom.target;
            m_zoom.active = false;
            m_zoom.settling = false;
            m_level = m_zoom.target;
            m_columns = kLevels[size_t(m_level)];
            setScroll(m_zoom.toScroll);
            if (changed)
                emit columnsChanged();
        }
    }
    updateAnimating();
    update();
    notifyScrolled();
}

void PhotoGrid::updateAnimating() {
    const bool animating = m_driveVelocity != 0 || m_physics.animating() ||
                           (m_zoom.active && (m_zoom.settling || m_zoom.fade < 1));
    if (animating != m_animating) {
        m_animating = animating;
        emit animatingChanged();
    }
}

void PhotoGrid::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (oldGeometry.width() > 0 && newGeometry.width() != oldGeometry.width()) {
        // Keep the same photos in view across a resize.
        const double ratio = newGeometry.width() / oldGeometry.width();
        m_scroll *= ratio;
    }
    setScroll(m_scroll);
}

void PhotoGrid::wheelEvent(QWheelEvent *event) {
    logInput(QStringLiteral("wheel phase=%1 pixel=%2,%3 angle=%4,%5 inverted=%6 device=%7 type=%8 ts=%9 mods=%10")
                 .arg(int(event->phase()))
                 .arg(event->pixelDelta().x()).arg(event->pixelDelta().y())
                 .arg(event->angleDelta().x()).arg(event->angleDelta().y())
                 .arg(event->inverted())
                 .arg(event->device() ? event->device()->name() : QStringLiteral("?"))
                 .arg(event->device() ? int(event->device()->type()) : -1)
                 .arg(event->timestamp())
                 .arg(int(event->modifiers())));
    if (event->modifiers() & Qt::ControlModifier) {
        const int notches = event->angleDelta().y() / 120;
        if (notches != 0) {
            const int from = m_zoom.active ? m_zoom.target : m_level;
            beginZoom(event->position());
            settleZoom(from - notches);
        }
        event->accept();
        return;
    }
    if (m_zoom.active) {
        // No scrolling while a zoom is in flight; it settles in a moment.
        event->accept();
        return;
    }

    // A touchpad reports begin, update and end phases; a mouse wheel none.
    // The begin and end come with no movement in them, and each arrives
    // twice, so they are recognised by phase, and repeats ignored.
    const double t = double(event->timestamp());
    // Zoomed out, the same swipe carries further: √(columns / 8), so 8
    // columns (where it was tuned) is unchanged and 40 is 2.2x. Measured
    // from Photos on an iPhone zoomed all the way out: a swipe there moves
    // about a screen, some 2,300 photos; here at 40 columns it moved half
    // a screen, a thousand.
    const double density = std::sqrt(std::max(1.0, m_columns / 8.0));
    const double dy = -event->pixelDelta().y() * density;
    switch (event->phase()) {
    case Qt::ScrollBegin:
        if (!m_physics.tracking()) {
            syncBounds();
            m_physics.begin(t);
        }
        break;
    case Qt::ScrollUpdate:
    case Qt::ScrollMomentum:
        syncBounds();
        m_physics.move(t, !event->pixelDelta().isNull() ? dy : -event->angleDelta().y() / 8.0 * density);
        break;
    case Qt::ScrollEnd:
        // Holds from before this lift (the fingers landing for this very
        // scroll) are handed out now, while nothing coasts for them to stop.
        if (m_hold)
            m_hold->poll();
        m_coastFrom = t;
        m_physics.end(t);
        break;
    case Qt::NoScrollPhase:
        syncBounds();
        if (!event->pixelDelta().isNull()) {
            // Smooth scrolling without phases: follow it directly.
            setScroll(m_scroll + dy);
        } else {
            // Mouse wheel: three quarters of a row per notch, eased.
            m_physics.nudge(-event->angleDelta().y() / 120.0 * stride() * 0.75);
        }
        break;
    }
    m_scroll = m_physics.position();
    updateAnimating();
    update();
    notifyScrolled();
    event->accept();
}

bool PhotoGrid::event(QEvent *event) {
    if (event->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(event);
        logInput(QStringLiteral("gesture type=%1 value=%2 delta=%3,%4 fingers=%5 pos=%6,%7")
                     .arg(int(g->gestureType()))
                     .arg(g->value())
                     .arg(g->delta().x()).arg(g->delta().y())
                     .arg(g->fingerCount())
                     .arg(g->position().x()).arg(g->position().y()));
        switch (g->gestureType()) {
        case Qt::BeginNativeGesture:
            beginZoom(g->position());
            break;
        case Qt::ZoomNativeGesture:
            // `value` is the change in scale since the last event; the scale
            // since the pinch began is their sum. Multiplying them instead
            // compounds, and a quick pinch zoomed 14 times over.
            if (!m_zoom.active || m_zoom.settling)
                beginZoom(g->position());
            m_zoom.pinchScale = std::clamp(m_zoom.pinchScale + g->value(), 0.02, 50.0);
            zoomTo(m_zoom.startStride * m_zoom.pinchScale, g->position());
            break;
        case Qt::EndNativeGesture:
            endContinuousZoom();
            break;
        default:
            break;
        }
        g->accept();
        return true;
    }
    return QQuickItem::event(event);
}

// Rendering

void PhotoGrid::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemSceneChange && value.window) {
        connect(value.window, &QQuickWindow::sceneGraphInvalidated, this, &PhotoGrid::invalidateSceneGraph,
                Qt::DirectConnection);
    }
    QQuickItem::itemChange(change, value);
}

void PhotoGrid::invalidateSceneGraph() {
    // Render thread, with the scene graph going away: its textures go too.
    m_render->dropAll();
    for (auto &slot : m_render->drawSlots)
        slot = {};
    m_render->selection = nullptr;
}

void PhotoGrid::releaseResources() {
    // The nodes go with the scene graph; textures are dropped on
    // invalidation, which follows on the render thread.
}

QSGNode *PhotoGrid::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    QElapsedTimer timer;
    timer.start();
    GridRenderState &rs = *m_render;
    rs.frame += 1;

    auto *root = old;
    if (!root) {
        root = new QSGNode;
        for (auto &slot : rs.drawSlots) {
            slot = {};
            slot.opacity = new QSGOpacityNode;
            slot.placeholders = new QSGGeometryNode;
            auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            slot.placeholders->setGeometry(geometry);
            slot.placeholders->setFlag(QSGNode::OwnsGeometry);
            slot.placeholders->setMaterial(new QSGVertexColorMaterial);
            slot.placeholders->setFlag(QSGNode::OwnsMaterial);
            slot.opacity->appendChildNode(slot.placeholders);
            slot.corners = new QSGGeometryNode;
            auto *cornerGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0);
            cornerGeometry->setDrawingMode(QSGGeometry::DrawTriangles);
            slot.corners->setGeometry(cornerGeometry);
            slot.corners->setFlag(QSGNode::OwnsGeometry);
            slot.corners->setMaterial(new QSGVertexColorMaterial);
            slot.corners->setFlag(QSGNode::OwnsMaterial);
            slot.opacity->appendChildNode(slot.corners);
            root->appendChildNode(slot.opacity);
        }
        rs.selection = new QSGGeometryNode;
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        rs.selection->setGeometry(geometry);
        rs.selection->setFlag(QSGNode::OwnsGeometry);
        rs.selection->setMaterial(new QSGVertexColorMaterial);
        rs.selection->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(rs.selection);
    }
    if (!m_library || !m_loader || width() <= 0 || count() == 0)
        return root;

    // The library was read again: textures follow their photos to their new
    // positions (no flash of colours), those of deleted photos go; mosaic
    // pages are rebuilt, since they hold positions, not photos.
    if (m_pendingMoves) {
        const std::vector<int> &moved = *m_pendingMoves;
        std::unordered_map<int, GridRenderState::Entry> cache;
        for (auto &[old, entry] : rs.cache) {
            const int now = old < int(moved.size()) ? moved[size_t(old)] : -1;
            if (now >= 0) {
                cache.emplace(now, entry);
                continue;
            }
            for (int lv = 0; lv < 4; ++lv) {
                if (entry.texture[lv]) {
                    rs.bytes -= qint64(entry.size[lv].width()) * entry.size[lv].height() * 4;
                    delete entry.texture[lv];
                    rs.textures -= 1;
                }
            }
            if (entry.medium) {
                rs.bytes -= qint64(entry.mediumSize.width()) * entry.mediumSize.height() * 4;
                delete entry.medium;
                rs.textures -= 1;
            }
        }
        rs.cache = std::move(cache);
        // Nodes may point at page textures: drop the nodes first.
        for (auto &slot : rs.drawSlots) {
            for (auto &[key, node] : slot.groups) {
                slot.opacity->removeChildNode(node);
                delete node;
            }
            slot.groups.clear();
        }
        for (auto &[page, p] : rs.pages) {
            rs.bytes -= qint64(kMosaicSide * kMosaicTile) * kMosaicSide * kMosaicTile * 4;
            delete p.texture;
        }
        rs.pages.clear();
        m_pendingMoves.reset();
    }

    const qreal dpr = window()->effectiveDevicePixelRatio();
    const std::vector<Layer> drawn = layers();
    const QRectF view(0, 0, width(), height());

    // Speed, in rows per second, decides how sharp a thumbnail is worth
    // decoding and how far ahead to look.
    const qint64 now = m_clock.nsecsElapsed();
    const double s = stride();
    const double rowsOnScreen = height() / s;
    if (rs.lastFrameAt > 0) {
        const double dt = std::max(1e-3, (now - rs.lastFrameAt) / 1e9);
        const double rows = (m_scroll - rs.lastScroll) / s;
        // A jump (scrubber, Home, End) is not motion: nothing is moving once
        // it lands, so it should not be decoded as if it were.
        // (A scrubber drag is a run of jumps, and it is motion.)
        const bool jumped = std::abs(rows) > rowsOnScreen && m_driveVelocity == 0 && !m_physics.animating() &&
                            !m_physics.tracking() && !m_scrubbing;
        m_speedRows = jumped ? 0 : m_speedRows * 0.7 + rows / dt * 0.3;
    }
    rs.lastFrameAt = now;
    rs.lastScroll = m_scroll;
    const double speed = std::abs(m_speedRows);
    // Faster than about three screens a second nobody sees detail, and the
    // decoders could not keep up anyway: coarse previews only.
    const bool scrubbing = m_scrubbing || speed > rowsOnScreen * 3;
    const bool fast = speed > rowsOnScreen * 0.75;
    // How far ahead to fetch, in rows: a screen and a half, or half a
    // second of travel when that is more. A mosaic page takes about 0.1 s
    // to build; at scrubbing speed a screen ahead is passed before it lands.
    const int lookahead = int(std::ceil(std::max(rowsOnScreen * 1.5, speed * 0.5)));

    // 1. What each layer shows: rows and columns in view.
    struct Range {
        int firstRow, lastRow, firstCol, lastCol;
    };
    std::vector<Range> ranges;
    for (const Layer &l : drawn) {
        const double ls = strideFor(l.columns);
        const QPointF topLeft = l.pin + (view.topLeft() - l.at) / l.scale;
        const QPointF bottomRight = l.pin + (view.bottomRight() - l.at) / l.scale;
        if (l.morph > 0) {
            // Photos glide in from where the other level had them: a few
            // rows beyond the view, and every column, may land inside it.
            const int extra = int(std::ceil((bottomRight.y() - topLeft.y()) / ls * 0.4)) + 1;
            ranges.push_back({std::max(0, int(std::floor(topLeft.y() / ls)) - extra),
                              int(std::floor(bottomRight.y() / ls)) + extra, 0, l.columns - 1});
            continue;
        }
        ranges.push_back({std::max(0, int(std::floor(topLeft.y() / ls))), int(std::floor(bottomRight.y() / ls)),
                          std::clamp(int(std::floor(topLeft.x() / ls)), 0, l.columns - 1),
                          std::clamp(int(std::floor(bottomRight.x() / ls)), 0, l.columns - 1)});
    }

    // 2. Turn decoded thumbnails into textures, within this frame's budget.
    // A thumbnail whose cell has meanwhile gone far off screen is dropped:
    // uploading it would only delay the ones that are still wanted, and
    // when scrolling fast that delay is what leaves cells empty.
    auto keptRange = [&](int lo, int hi) {
        for (size_t k = 0; k < drawn.size(); ++k) {
            const int margin = std::max(int(height() / strideFor(drawn[k].columns) * 3) + 2, k == 0 ? lookahead + 2 : 0);
            const int from = std::max(0, ranges[k].firstRow - margin) * drawn[k].columns;
            const int to = (ranges[k].lastRow + margin) * drawn[k].columns;
            if (hi >= from && lo < to)
                return true;
        }
        return false;
    };
    auto kept = [&](int index) { return keptRange(index, index); };
    int uploads = 0;
    qint64 uploadBytes = 0;
    QElapsedTimer uploadTimer;
    uploadTimer.start();
    while (uploadBytes < kUploadBytesPerFrame && uploadTimer.nsecsElapsed() < kUploadNanos) {
        auto ready = m_loader->takeReady(16);
        if (ready.empty())
            break;
        for (auto &d : ready) {
            if (isPageKey(d.key)) {
                const int page = keyPage(d.key);
                if (!keptRange(page * kMosaicCount, page * kMosaicCount + kMosaicCount - 1))
                    continue;
                auto &p = rs.pages[page];
                if (p.texture) {
                    delete p.texture;
                    rs.bytes -= qint64(d.image.width()) * d.image.height() * 4;
                }
                // A texture of its own, not in an atlas: it is a whole page.
                p.texture = window()->createTextureFromImage(d.image);
                p.texture->setFiltering(QSGTexture::Linear);
                p.lastUsed = rs.frame;
                const qint64 bytes = qint64(d.image.width()) * d.image.height() * 4;
                rs.bytes += bytes;
                uploads += 1;
                uploadBytes += bytes;
                continue;
            }
            const int index = keyIndex(d.key);
            if (!kept(index))
                continue;
            auto &entry = rs.cache[index];
            if (isMediumKey(d.key)) {
                if (entry.medium) {
                    rs.bytes -= qint64(entry.mediumSize.width()) * entry.mediumSize.height() * 4;
                    delete entry.medium;
                    rs.textures -= 1;
                }
                entry.medium = window()->createTextureFromImage(d.image, QQuickWindow::TextureCanUseAtlas);
                entry.medium->setFiltering(QSGTexture::Linear);
                entry.mediumSize = d.image.size();
                entry.mediumLevel = keyLevel(d.key);
                entry.lastUsed = rs.frame;
                const qint64 bytes = qint64(d.image.width()) * d.image.height() * 4;
                rs.bytes += bytes;
                rs.textures += 1;
                uploads += 1;
                uploadBytes += bytes;
                continue;
            }
            const int level = keyLevel(d.key);
            if (entry.texture[level]) {
                rs.bytes -= qint64(entry.size[level].width()) * entry.size[level].height() * 4;
                delete entry.texture[level];
                rs.textures -= 1;
            }
            entry.texture[level] = window()->createTextureFromImage(d.image, QQuickWindow::TextureCanUseAtlas);
            entry.texture[level]->setFiltering(QSGTexture::Linear);
            entry.size[level] = d.image.size();
            entry.lastUsed = rs.frame;
            const qint64 bytes = qint64(d.image.width()) * d.image.height() * 4;
            rs.bytes += bytes;
            rs.textures += 1;
            uploads += 1;
            uploadBytes += bytes;
        }
    }
    const qint64 uploadNanos = uploadTimer.nsecsElapsed();

    auto desiredLevel = [&](const LibraryItem &item, double cellDevicePx) {
        const int shortEdge = std::max(1, int(std::min(item.thumbW, item.thumbH)));
        const double need = scrubbing ? cellDevicePx * 0.25 : fast ? cellDevicePx * 0.5 : cellDevicePx * 0.85;
        int level = 0;
        while (level < 3 && (shortEdge >> (level + 1)) >= need)
            level += 1;
        return level;
    };

    // 3. Build each layer's geometry: one list of quads per texture, one for
    // colours. The main layer's wants go first.
    struct Quad { QRectF rect; QRectF uv; };
    struct Built {
        std::unordered_map<qint64, std::pair<QSGTexture *, std::vector<Quad>>> byTexture;
        std::vector<std::pair<QRectF, quint32>> colours;
    };
    std::vector<Built> built(drawn.size());
    std::vector<quint64> wanted;
    // Medium renditions come after every thumbnail: they sharpen what is
    // already there.
    std::vector<quint64> wantedMedium;
    const bool settled = !m_zoom.active && !fast && !scrubbing;
    int mediumNeeded = 0, mediumShown = 0;
    int unresolved = 0;
    for (size_t k = 0; k < drawn.size(); ++k) {
        const Layer &l = drawn[k];
        const Range &r = ranges[k];
        const double cellDevicePx = (strideFor(l.columns) - gapFor(l.columns)) * l.scale * dpr;
        // Small cells, and anything flying past too fast to see detail.
        const bool mosaic = cellDevicePx < kMosaicBelowPx || scrubbing;
        int lastWantedPage = -1;
        for (int row = r.firstRow; row <= r.lastRow; ++row) {
            for (int col = r.firstCol; col <= r.lastCol; ++col) {
                const int i = row * l.columns + col;
                if (i >= count())
                    break;
                const QRectF rect = cellRect(i, l);
                if (!rect.intersects(view))
                    continue;
                const LibraryItem &item = m_library->at(i);
                if (mosaic) {
                    const int page = i / kMosaicCount;
                    auto found = rs.pages.find(page);
                    if (found != rs.pages.end()) {
                        found->second.lastUsed = rs.frame;
                        QSGTexture *texture = found->second.texture;
                        // The tile, half a texel in from its edges so linear
                        // filtering never reaches into the neighbours.
                        const int j = i % kMosaicCount;
                        const double side = kMosaicSide * kMosaicTile;
                        const double inset = 0.5 / side;
                        const QRectF uv((j % kMosaicSide) * kMosaicTile / side + inset,
                                        (j / kMosaicSide) * kMosaicTile / side + inset,
                                        kMosaicTile / side - 2 * inset, kMosaicTile / side - 2 * inset);
                        auto &group = built[k].byTexture[texture->comparisonKey()];
                        group.first = texture;
                        group.second.push_back({rect, uv});
                        continue;
                    }
                    if (page != lastWantedPage) {
                        wanted.push_back(pageKey(page));
                        lastWantedPage = page;
                    }
                    if (k == 0)
                        unresolved += 1;
                    // Until the page arrives: any thumbnail already here, or
                    // the colour.
                    QSGTexture *texture = nullptr;
                    if (auto any = rs.cache.find(i); any != rs.cache.end())
                        for (int lv = 3; lv >= 0 && !texture; --lv)
                            texture = any->second.texture[lv];
                    if (texture) {
                        const QRectF crop = squareCrop(int(item.thumbW), int(item.thumbH));
                        const QRectF sub = texture->normalizedTextureSubRect();
                        auto &group = built[k].byTexture[texture->comparisonKey()];
                        group.first = texture;
                        group.second.push_back({rect, QRectF(sub.x() + crop.x() * sub.width(),
                                                             sub.y() + crop.y() * sub.height(),
                                                             crop.width() * sub.width(), crop.height() * sub.height())});
                    } else {
                        built[k].colours.emplace_back(rect, item.avgColor);
                    }
                    continue;
                }
                const int want = desiredLevel(item, cellDevicePx);
                QSGTexture *texture = nullptr;
                int have = -1;
                auto found = rs.cache.find(i);

                // Bigger than the thumbnail can fill without blur: the
                // medium rendition (2048 px on the long side, same shape),
                // at the smallest decode level that still covers the cell.
                const int shortEdge = std::max(1, int(std::min(item.thumbW, item.thumbH)));
                const double need = cellDevicePx * 0.85;
                if (need > shortEdge * 1.35) {
                    if (k == 0)
                        mediumNeeded += 1;
                    const int longEdge = std::max(1, int(std::max(item.thumbW, item.thumbH)));
                    const int mediumShort = 2048 * shortEdge / longEdge;
                    int level = 0;
                    while (level < 3 && (mediumShort >> (level + 1)) >= need)
                        level += 1;
                    QSGTexture *medium = found != rs.cache.end() ? found->second.medium : nullptr;
                    if (settled && k == 0 && (!medium || found->second.mediumLevel > level))
                        wantedMedium.push_back(mediumKey(i, level));
                    if (medium) {
                        if (k == 0)
                            mediumShown += 1;
                        found->second.lastUsed = rs.frame;
                        const QRectF crop = squareCrop(int(item.thumbW), int(item.thumbH));
                        const QRectF sub = medium->normalizedTextureSubRect();
                        auto &group = built[k].byTexture[medium->comparisonKey()];
                        group.first = medium;
                        group.second.push_back({rect, QRectF(sub.x() + crop.x() * sub.width(),
                                                             sub.y() + crop.y() * sub.height(),
                                                             crop.width() * sub.width(), crop.height() * sub.height())});
                        continue;
                    }
                }
                if (found != rs.cache.end()) {
                    auto &entry = found->second;
                    // The wanted level, else anything sharper, else anything.
                    for (int lv = want; lv >= 0 && !texture; --lv)
                        if (entry.texture[lv]) { texture = entry.texture[lv]; have = lv; }
                    for (int lv = want + 1; lv < 4 && !texture; ++lv)
                        if (entry.texture[lv]) { texture = entry.texture[lv]; have = lv; }
                    entry.lastUsed = rs.frame;
                }
                if (have > want || have < 0) {
                    if (k == 0)
                        unresolved += 1;
                    wanted.push_back(thumbKey(i, want));
                }
                if (texture) {
                    const QRectF crop = squareCrop(int(item.thumbW), int(item.thumbH));
                    const QRectF sub = texture->normalizedTextureSubRect();
                    const QRectF uv(sub.x() + crop.x() * sub.width(), sub.y() + crop.y() * sub.height(),
                                    crop.width() * sub.width(), crop.height() * sub.height());
                    auto &group = built[k].byTexture[texture->comparisonKey()];
                    group.first = texture;
                    group.second.push_back({rect, uv});
                } else {
                    built[k].colours.emplace_back(rect, item.avgColor);
                }
            }
        }
    }

    // 4. At rest, look ahead in the direction of travel, so rows arrive
    // decoded.
    const bool restMosaic = (s - gapFor(m_columns)) * dpr < kMosaicBelowPx || scrubbing;
    if (!m_zoom.active && restMosaic) {
        // Pages ahead in the direction of travel, and one behind.
        const Range &r = ranges.front();
        const int ahead = lookahead;
        const bool down = m_speedRows >= 0;
        const int fromRow = down ? r.lastRow + 1 : std::max(0, r.firstRow - ahead);
        const int toRow = down ? r.lastRow + ahead : r.firstRow - 1;
        const int lastPage = (count() - 1) / kMosaicCount;
        auto wantPages = [&](int a, int b) {
            for (int p = std::max(0, a); p <= std::min(lastPage, b); ++p)
                if (!rs.pages.contains(p))
                    wanted.push_back(pageKey(p));
                else
                    rs.pages[p].lastUsed = rs.frame;
        };
        wantPages(fromRow * m_columns / kMosaicCount, (toRow + 1) * m_columns / kMosaicCount);
        if (down)
            wantPages(r.firstRow * m_columns / kMosaicCount - 1, r.firstRow * m_columns / kMosaicCount - 1);
        else
            wantPages((r.lastRow + 1) * m_columns / kMosaicCount + 1, (r.lastRow + 1) * m_columns / kMosaicCount + 1);
    }
    if (!m_zoom.active && !scrubbing && !restMosaic) {
        const Range &r = ranges.front();
        const double cellDevicePx = (s - gapFor(m_columns)) * dpr;
        const int rowsAhead = int(std::ceil(rowsOnScreen * (fast ? 2.5 : 1.0)));
        const int rowsBehind = int(std::ceil(rowsOnScreen * 0.5));
        const bool down = m_speedRows >= 0;
        auto prefetchRows = [&](int fromRow, int toRow) {
            for (int row = fromRow; row <= toRow; ++row) {
                for (int c = 0; c < m_columns; ++c) {
                    const int i = row * m_columns + c;
                    if (i < 0 || i >= count())
                        continue;
                    const int want = desiredLevel(m_library->at(i), cellDevicePx);
                    auto found = rs.cache.find(i);
                    if (found != rs.cache.end()) {
                        found->second.lastUsed = rs.frame;
                        bool good = false;
                        for (int lv = want; lv >= 0; --lv)
                            good = good || found->second.texture[lv];
                        if (good)
                            continue;
                    }
                    wanted.push_back(thumbKey(i, want));
                }
            }
        };
        if (down) {
            prefetchRows(r.lastRow + 1, r.lastRow + rowsAhead);
            prefetchRows(r.firstRow - rowsBehind, r.firstRow - 1);
        } else {
            prefetchRows(r.firstRow - rowsAhead, r.firstRow - 1);
            prefetchRows(r.lastRow + 1, r.lastRow + rowsBehind);
        }
    }
    wanted.insert(wanted.end(), wantedMedium.begin(), wantedMedium.end());
    m_loader->setWanted(wanted);

    // Does the base layer cover the window, across and (when the library
    // is tall enough to) down?
    {
        const Layer &l = drawn.front();
        const double left = l.at.x() - l.pin.x() * l.scale;
        const double right = left + width() * l.scale;
        double band = std::max(left, width() - right);
        const double tall = (count() + l.columns - 1) / l.columns * strideFor(l.columns) * l.scale;
        if (tall >= height()) {
            const double top = l.at.y() - l.pin.y() * l.scale;
            band = std::max({band, top, height() - (top + tall)});
        }
        if (band > 0.5) {
            m_uncoveredFrames += 1;
            m_uncoveredMax = std::max(m_uncoveredMax, band);
        }
    }

    // Where the pinned photo (the last zoom's) is drawn, frame to frame.
    if (m_zoom.startStride > 0) {
        const Layer &l = drawn.front();
        const QPointF p = l.at + (pinIn(l.columns) - l.pin) * l.scale;
        // Height only: across, the photo sits in a different column in each
        // of the two levels being blended, and cross-fades between them.
        if (m_pinTracked)
            m_pinJump = std::max(m_pinJump, std::abs(p.y() - m_lastPinOnScreen.y()));
        m_lastPinOnScreen = p;
        m_pinTracked = true;
    }

    // Rounded corners, a little, as in Photos: over each corner of a cell,
    // the background outside a quarter circle. The radius grows with the
    // cell up to 5 px; on cells too small to see it, none.
    const QColor bg = m_background;
    const uchar bgr = uchar(bg.red()), bgg = uchar(bg.green()), bgb = uchar(bg.blue());
    auto writeCorners = [&](QSGGeometryNode *node, const Built &b) {
        std::vector<QRectF> cells;
        cells.reserve(b.colours.size());
        for (const auto &[r, c] : b.colours)
            cells.push_back(r);
        for (const auto &[key, group] : b.byTexture)
            for (const Quad &q : group.second)
                cells.push_back(q.rect);
        const double side = cells.empty() ? 0 : cells.front().width();
        const double radius = std::min(5.0, side * 0.04);
        QSGGeometry *g = node->geometry();
        if (radius < 1.5) {
            g->allocate(0);
            node->markDirty(QSGNode::DirtyGeometry);
            return;
        }
        const int segments = radius >= 3 ? 4 : 2;
        g->allocate(int(cells.size()) * 4 * segments * 3);
        auto *v = g->vertexDataAsColoredPoint2D();
        static constexpr double kQuarter = M_PI / 2;
        for (const QRectF &r : cells) {
            // Each corner, and the centre of its arc.
            const QPointF corners[4] = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
            const QPointF centres[4] = {r.topLeft() + QPointF(radius, radius), r.topRight() + QPointF(-radius, radius),
                                        r.bottomRight() + QPointF(-radius, -radius),
                                        r.bottomLeft() + QPointF(radius, -radius)};
            for (int c = 0; c < 4; ++c) {
                const double start = M_PI + c * kQuarter; // top left's arc runs from 180° to 270°
                for (int i = 0; i < segments; ++i) {
                    const double a0 = start + kQuarter * i / segments, a1 = start + kQuarter * (i + 1) / segments;
                    v[0].set(float(corners[c].x()), float(corners[c].y()), bgr, bgg, bgb, 255);
                    v[1].set(float(centres[c].x() + radius * std::cos(a0)), float(centres[c].y() + radius * std::sin(a0)),
                             bgr, bgg, bgb, 255);
                    v[2].set(float(centres[c].x() + radius * std::cos(a1)), float(centres[c].y() + radius * std::sin(a1)),
                             bgr, bgg, bgb, 255);
                    v += 3;
                }
            }
        }
        node->markDirty(QSGNode::DirtyGeometry);
    };

    // 5. Write the geometry into the two slots.
    int batches = 0;
    for (int k = 0; k < 2; ++k) {
        GridRenderState::Slot &slot = rs.drawSlots[k];
        static const Built empty;
        const Built &b = size_t(k) < built.size() ? built[size_t(k)] : empty;
        const double opacity = size_t(k) < drawn.size() ? drawn[size_t(k)].opacity : 0;
        if (slot.opacity->opacity() != opacity)
            slot.opacity->setOpacity(opacity);

        QSGGeometry *g = slot.placeholders->geometry();
        g->allocate(int(b.colours.size() * 6));
        auto *v = g->vertexDataAsColoredPoint2D();
        for (const auto &[r, c] : b.colours) {
            const uchar red = uchar(c >> 16), green = uchar(c >> 8), blue = uchar(c);
            const float x0 = float(r.left()), y0 = float(r.top()), x1 = float(r.right()), y1 = float(r.bottom());
            v[0].set(x0, y0, red, green, blue, 255);
            v[1].set(x1, y0, red, green, blue, 255);
            v[2].set(x0, y1, red, green, blue, 255);
            v[3].set(x1, y0, red, green, blue, 255);
            v[4].set(x1, y1, red, green, blue, 255);
            v[5].set(x0, y1, red, green, blue, 255);
            v += 6;
        }
        slot.placeholders->markDirty(QSGNode::DirtyGeometry);

        for (auto it = slot.groups.begin(); it != slot.groups.end();) {
            if (!b.byTexture.contains(it->first)) {
                // Nothing on this page this frame. The node goes, so it never
                // points at a texture that may be evicted below.
                slot.opacity->removeChildNode(it->second);
                delete it->second;
                it = slot.groups.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto &[key, group] : b.byTexture) {
            QSGGeometryNode *&node = slot.groups[key];
            if (!node) {
                node = new QSGGeometryNode;
                auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0);
                geometry->setDrawingMode(QSGGeometry::DrawTriangles);
                node->setGeometry(geometry);
                node->setFlag(QSGNode::OwnsGeometry);
                // Not the opaque material: the faded layer needs opacity.
                auto *material = new QSGTextureMaterial;
                material->setFiltering(QSGTexture::Linear);
                node->setMaterial(material);
                node->setFlag(QSGNode::OwnsMaterial);
                // Under the corners, which round it.
                slot.opacity->insertChildNodeBefore(node, slot.corners);
            }
            auto *material = static_cast<QSGTextureMaterial *>(node->material());
            // Any texture on the page binds the page; this one is in use this
            // frame, so it cannot be evicted before the frame is drawn.
            material->setTexture(group.first);
            QSGGeometry *geometry = node->geometry();
            geometry->allocate(int(group.second.size() * 6));
            auto *tv = geometry->vertexDataAsTexturedPoint2D();
            for (const Quad &q : group.second) {
                const float x0 = float(q.rect.left()), y0 = float(q.rect.top());
                const float x1 = float(q.rect.right()), y1 = float(q.rect.bottom());
                const float u0 = float(q.uv.left()), v0 = float(q.uv.top());
                const float u1 = float(q.uv.right()), v1 = float(q.uv.bottom());
                tv[0].set(x0, y0, u0, v0);
                tv[1].set(x1, y0, u1, v0);
                tv[2].set(x0, y1, u0, v1);
                tv[3].set(x1, y0, u1, v0);
                tv[4].set(x1, y1, u1, v1);
                tv[5].set(x0, y1, u0, v1);
                tv += 6;
            }
            node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
        }
        writeCorners(slot.corners, b);
        if (opacity > 0)
            batches += int(b.byTexture.size()) + 2;
    }

    // The selection: a border inside each selected cell of the main layer.
    {
        std::vector<QRectF> frames;
        if (!m_selected.empty()) {
            const Layer &l = drawn.front();
            const Range &r = ranges.front();
            const int from = r.firstRow * l.columns, to = std::min(count(), (r.lastRow + 1) * l.columns);
            for (auto it = m_selected.lower_bound(from); it != m_selected.end() && *it < to; ++it)
                frames.push_back(cellRect(*it, l));
        }
        // Each: a border, and a check badge in the bottom right corner as
        // on an iPhone: a white ring, the accent inside, a white tick.
        constexpr int kSegments = 16;
        constexpr int kPerCell = 24 + 2 * kSegments * 3 + 12;
        QSGGeometry *g = rs.selection->geometry();
        g->allocate(int(frames.size() * kPerCell));
        auto *v = g->vertexDataAsColoredPoint2D();
        const float w = 3;
        const uchar r = uchar(m_accent.red()), gr = uchar(m_accent.green()), b = uchar(m_accent.blue());
        auto quad = [&](float x0, float y0, float x1, float y1) {
            v[0].set(x0, y0, r, gr, b, 255);
            v[1].set(x1, y0, r, gr, b, 255);
            v[2].set(x0, y1, r, gr, b, 255);
            v[3].set(x1, y0, r, gr, b, 255);
            v[4].set(x1, y1, r, gr, b, 255);
            v[5].set(x0, y1, r, gr, b, 255);
            v += 6;
        };
        auto disc = [&](float cx, float cy, float radius, uchar dr, uchar dg, uchar db) {
            for (int i = 0; i < kSegments; ++i) {
                const double a0 = 2 * M_PI * i / kSegments, a1 = 2 * M_PI * (i + 1) / kSegments;
                v[0].set(cx, cy, dr, dg, db, 255);
                v[1].set(cx + radius * float(std::cos(a0)), cy + radius * float(std::sin(a0)), dr, dg, db, 255);
                v[2].set(cx + radius * float(std::cos(a1)), cy + radius * float(std::sin(a1)), dr, dg, db, 255);
                v += 3;
            }
        };
        // A stroke from p to q, `t` thick, in white.
        auto stroke = [&](QPointF p, QPointF q, double t) {
            const QPointF d = q - p;
            const double len = std::max(1e-6, std::hypot(d.x(), d.y()));
            const QPointF n(-d.y() / len * t / 2, d.x() / len * t / 2);
            const QPointF a = p + n, b2 = p - n, c = q + n, e = q - n;
            v[0].set(float(a.x()), float(a.y()), 255, 255, 255, 255);
            v[1].set(float(b2.x()), float(b2.y()), 255, 255, 255, 255);
            v[2].set(float(c.x()), float(c.y()), 255, 255, 255, 255);
            v[3].set(float(b2.x()), float(b2.y()), 255, 255, 255, 255);
            v[4].set(float(e.x()), float(e.y()), 255, 255, 255, 255);
            v[5].set(float(c.x()), float(c.y()), 255, 255, 255, 255);
            v += 6;
        };
        for (const QRectF &f : frames) {
            const float x0 = float(f.left()), y0 = float(f.top()), x1 = float(f.right()), y1 = float(f.bottom());
            quad(x0, y0, x1, y0 + w);
            quad(x0, y1 - w, x1, y1);
            quad(x0, y0 + w, x0 + w, y1 - w);
            quad(x1 - w, y0 + w, x1, y1 - w);
            const double radius = std::clamp(f.width() * 0.1, 6.0, 11.0);
            const double cx = f.right() - radius - 6, cy = f.bottom() - radius - 6;
            disc(float(cx), float(cy), float(radius + 1.5), 255, 255, 255);
            disc(float(cx), float(cy), float(radius), r, gr, b);
            const double t = std::max(1.5, radius * 0.22);
            stroke({cx - radius * 0.45, cy + radius * 0.02}, {cx - radius * 0.1, cy + radius * 0.38}, t);
            stroke({cx - radius * 0.1, cy + radius * 0.38}, {cx + radius * 0.48, cy - radius * 0.35}, t);
        }
        rs.selection->markDirty(QSGNode::DirtyGeometry);
    }

    // 6. Keep texture memory under budget: drop what has not been on or
    // near the screen for longest.
    // Freeing textures costs real time (859 at once took 13 ms), so it is
    // spread out: past nine tenths of the budget, a few dozen of the least
    // recently used go each frame, found without a full sort.
    static const qint64 budget = qEnvironmentVariableIntValue("PINHOLE_TEXTURE_MB") > 0
                                     ? qint64(qEnvironmentVariableIntValue("PINHOLE_TEXTURE_MB")) << 20
                                     : qint64(1024) << 20;
    constexpr size_t kEvictionsPerFrame = 48;
    int evicted = 0;
    QElapsedTimer evictTimer;
    evictTimer.start();
    if (rs.bytes > budget * 9 / 10) {
        std::vector<std::pair<quint64, int>> candidates;
        for (const auto &[index, entry] : rs.cache)
            if (entry.lastUsed + 2 < rs.frame)
                candidates.emplace_back(entry.lastUsed, index);
        const size_t n = std::min(kEvictionsPerFrame, candidates.size());
        std::nth_element(candidates.begin(), candidates.begin() + long(n), candidates.end());
        candidates.resize(n);
        for (const auto &[used, index] : candidates) {
            evicted += 1;
            auto &entry = rs.cache[index];
            for (int lv = 0; lv < 4; ++lv) {
                if (entry.texture[lv]) {
                    rs.bytes -= qint64(entry.size[lv].width()) * entry.size[lv].height() * 4;
                    delete entry.texture[lv];
                    rs.textures -= 1;
                }
            }
            if (entry.medium) {
                rs.bytes -= qint64(entry.mediumSize.width()) * entry.mediumSize.height() * 4;
                delete entry.medium;
                rs.textures -= 1;
            }
            rs.cache.erase(index);
        }
    }

    // Pages: keep the most recently used, freeing a few a frame.
    if (rs.pages.size() > kMaxPages) {
        std::vector<std::pair<quint64, int>> candidates;
        for (const auto &[page, p] : rs.pages)
            if (p.lastUsed + 2 < rs.frame)
                candidates.emplace_back(p.lastUsed, page);
        const size_t n = std::min<size_t>({4, candidates.size(), rs.pages.size() - kMaxPages});
        std::nth_element(candidates.begin(), candidates.begin() + long(n), candidates.end());
        for (size_t i = 0; i < n; ++i) {
            auto &p = rs.pages[candidates[i].second];
            rs.bytes -= qint64(kMosaicSide * kMosaicTile) * kMosaicSide * kMosaicTile * 4;
            delete p.texture;
            rs.pages.erase(candidates[i].second);
            evicted += 1;
        }
    }

    m_unresolved = unresolved;
    m_mediumNeeded = mediumNeeded;
    m_mediumShown = mediumShown;
    m_textureCount = rs.textures;
    m_textureBytes = rs.bytes;
    if (m_stats) {
        int cells = 0, placeholders = 0;
        for (const Built &b : built) {
            placeholders += int(b.colours.size());
            cells += int(b.colours.size());
            for (const auto &[key, group] : b.byTexture)
                cells += int(group.second.size());
        }
        m_stats->frameBuilt(timer.nsecsElapsed(), cells, placeholders, uploads, batches, evicted, uploadNanos,
                            evictTimer.nsecsElapsed());
    }
    // Anything uploaded may have left more decoded thumbnails waiting behind
    // the frame's budget, and the loader only signals once until they are
    // taken: come back next frame.
    if (uploads > 0)
        QMetaObject::invokeMethod(this, &QQuickItem::update, Qt::QueuedConnection);
    return root;
}

void PhotoGrid::refreshStats() {
    if (!m_statsVisible || !m_stats || !m_loader)
        return;
    const double decodeMs = m_loader->decodedCount() ? m_loader->decodeNanos() / 1e6 / m_loader->decodedCount() : 0;
    m_statsText = m_stats->overlayText() +
                  QStringLiteral("\n%1 columns  %2 rows/s  decode %3 ms avg on %4 threads, %5 queued\n"
                                 "%6 textures, %7 MB  %8 cells waiting")
                      .arg(m_columns)
                      .arg(m_speedRows, 0, 'f', 0)
                      .arg(decodeMs, 0, 'f', 2)
                      .arg(m_loader->threads())
                      .arg(m_loader->queued())
                      .arg(m_textureCount.load())
                      .arg(m_textureBytes.load() >> 20)
                      .arg(m_unresolved.load());
    emit statsTextChanged();
}
