#include "photogrid.h"

#include "framestats.h"
#include "library.h"
#include "marks.h"
#include "thumbnailloader.h"
#include "touchpadhold.h"

#include <QDateTime>
#include <QLocale>
#include <QFile>
#include <QInputDevice>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSGVertexColorMaterial>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace {

// The zoom levels, as column counts, from one photo across (a feed) out to
// 29, few of them: neighbours are about 2-3x apart, still close enough that
// a cross-fade between two of them reads as one motion. Every step is an even number of columns
// (so every count is odd, as in Photos on an iPhone), so that two
// neighbouring levels grown about the window's middle (or an edge) have
// their columns on the same lines at every size: at a change of level the
// cells hold still and only the photos in them cross-fade. (A step of 1 or
// 3 columns allows that only about the edges.)
constexpr std::array<int, 5> kLevels = {1, 3, 7, 13, 29};
constexpr int kDefaultLevel = 2;
// Seconds for a zoom to settle after the fingers lift, or for a zoom step.
constexpr double kSettleSeconds = 0.3;
// The cross-fade from one level to the next once a pinch passes the point
// halfway between them, eased at both ends, whatever the fingers do next.
constexpr double kMorphSeconds = 0.35;
// A pinch released while still moving goes on this long, in seconds, when
// picking the level it lands on: a quick pinch commits.
constexpr double kPinchProjection = 0.12;
// A horizontal wheel's notch zooms by this much, eased over about this
// long, and lands on a level once it has been still this long (seconds):
// the next one the way it was turning, unless it turned less than
// kWheelZoomDead notches, too little to mean anything, and goes back. A
// turn goes one level at most: an MX Master's thumb wheel sends several
// notches for the slightest touch, more than a level, so the zoom stops at
// the next level and lands there at once, and the rest of the turn, until
// the wheel has been still this long, counts for nothing.
constexpr double kWheelZoomStep = 1.25;
constexpr double kWheelZoomEase = 0.06;
constexpr double kWheelZoomRest = 0.2;
constexpr double kWheelZoomDead = 0.3;
// A pinch or wheel goes over to the next level once it is this much (log
// of the cell size) nearer to it than to the one shown, not right at the
// point halfway: held there, it would flip between the two.
constexpr double kZoomHysteresis = 0.08;
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
// Marks (marks.h: plainMark()), the neon tubes' shapes unlit in the
// theme's colours: kMarkShare of a cell's width on screen, from kMarkPx up
// to kMarkMaxPx, a little in from its bottom left corner, on cells at
// least kMarksFromPx big; smaller ones would be all marks. In the order
// drawn, left to right.
constexpr double kMarkPx = 22;
constexpr double kMarkMaxPx = 40;
constexpr double kMarkShare = 0.1;
constexpr double kMarkInset = 4;
constexpr double kMarksFromPx = 72;
constexpr std::array<Mark, kMarkKinds> kMarkOrder = {MarkFavorite, MarkVideo, MarkLive, MarkStack};
// At this many columns and further out, seen from afar, the marks glow
// like neon in the theme's colours (marks.h: neonMark()), so a video or a
// favourite is found among hundreds of photos at a glance, and a little
// bigger. A zoom cross-fades the white marks into them with the level.
// Unlike white marks they are on cells down to kNeonMarksFromPx, which
// is what they are for: 29 columns on a window 1600 wide are 54 px. On
// cells too small for kNeonMarkPx they shrink to kNeonMarkShare of one.
constexpr int kNeonFromColumns = 13;
constexpr double kNeonMarkPx = 22;
constexpr double kNeonMarksFromPx = 20;
constexpr double kNeonMarkShare = 0.42;
// Once the grid rests at such a level, coming from a closer one, the photos
// first dim by kDusk over kDuskFall seconds, smoothly, so the tubes stand
// out from them while the grid is still; then the tubes strike (lit ones
// stay lit through a zoom between such levels): each dark until its own
// moment within the first kNeonStagger seconds, then stuttering on and off
// for kNeonStrike, more on than off as it warms, then steady. Drawn in
// kNeonSteps of brightness, from dark to lit.
constexpr double kDuskFall = 0.25;
constexpr double kNeonStagger = 0.8;
constexpr double kNeonStrike = 0.5;
constexpr int kNeonSteps = 6;
constexpr double kDusk = 0.35;

quint32 mix(quint32 h) {
    h ^= h >> 16;
    h *= 0x7feb352d;
    h ^= h >> 15;
    h *= 0x846ca68b;
    return h ^ (h >> 16);
}

// How lit a photo's mark is `t` seconds after the tubes began to strike.
double neonLit(double t, quint32 photo) {
    const double s = t - kNeonStagger * (mix(photo) & 0xffff) / 65535.0;
    if (s < 0)
        return 0;
    if (s >= kNeonStrike)
        return 1;
    // On or off for every 40 ms, on more often as it warms.
    const quint32 h = mix(photo * 0x9e3779b9u + quint32(s / 0.04));
    return (h % 1000) / 1000.0 < 0.2 + 0.8 * s / kNeonStrike ? 1 : 0.2;
}

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
// and every animated frame, for tuning scrolling against what the touchpad
// or wheel really sends and what the screen then shows.
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
        // The dusk over the photos of a lit level (kDusk), then the
        // marks, over the corners.
        QSGGeometryNode *dusk = nullptr;
        QSGGeometryNode *marks = nullptr;
    };
    Slot drawSlots[2];
    // Every mark side by side, in kMarkOrder, drawn for this device pixel
    // ratio in the colours of this version of them.
    QSGTexture *marks = nullptr;
    qreal marksDpr = 0;
    int marksVersion = -1;
    // The same lit as neon: a row of glows over a row of cores, in the
    // colours of this version of them.
    // kNeonSteps rows of each, darkest first.
    QSGTexture *neon = nullptr;
    qreal neonDpr = 0;
    int neonVersion = -1;

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
        delete marks;
        marks = nullptr;
        marksDpr = 0;
        delete neon;
        neon = nullptr;
        neonDpr = 0;
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
    rebuildCells();
    emit libraryChanged();
    emit cellsChanged();
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

void PhotoGrid::libraryReloaded(const std::vector<int> &moved, const std::vector<int> *textures) {
    const Anchor before = m_reloadAnchor;
    auto mapped = [&](int old) { return old >= 0 && old < int(moved.size()) ? moved[size_t(old)] : -1; };

    QList<int> selectedBefore;
    for (int i : selection())
        if (const int n = mapped(i); n >= 0)
            selectedBefore.append(n);
    std::sort(selectedBefore.begin(), selectedBefore.end());
    m_zoom = {};
    rebuildCells(&moved);
    const bool selectionChanged = selectedBefore != selection();
    m_pendingMoves = std::make_shared<std::vector<int>>(textures ? *textures : moved);
    emit libraryChanged();
    emit cellsChanged();

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
    // Said again even where the fraction is the same: what depends on it
    // (at the newest end or not) was read with the new count before the
    // scroll above followed it.
    m_lastFraction = -1;
    notifyScrolled();
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
    const int cell = cellAt(x, y);
    return cell >= 0 ? libraryAt(cell) : -1;
}

int PhotoGrid::cellAt(qreal x, qreal y) const {
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
    return middle >= 0 ? middle : count() > 0 ? libraryAt(count() - 1) : -1;
}

QRectF PhotoGrid::rectOf(int index) const {
    const int cell = cellOf(index);
    if (cell < 0)
        return {};
    return cellRect(cell, Layer{m_columns, 1, QPointF(0, m_scroll), QPointF(0, 0), 1});
}

int PhotoGrid::stackOf(int index) const {
    if (m_stackOf.empty())
        return -1;
    const int cell = cellOf(index);
    if (cell < 0)
        return -1;
    const int card = libraryAt(cell);
    return m_stackOf.contains(card) ? card : -1;
}

int PhotoGrid::cellOf(int index) const {
    if (m_cellOf.empty())
        return index;
    return index >= 0 && index < int(m_cellOf.size()) ? m_cellOf[size_t(index)] : -1;
}

void PhotoGrid::appendPhotos(int cell, QList<int> &out) const {
    const int index = libraryAt(cell);
    if (const auto stack = m_stackOf.find(index); stack != m_stackOf.end())
        out.append(QList<int>(stack->second.begin(), stack->second.end()));
    else
        out.append(index);
}

QList<int> PhotoGrid::photosOf(int cell) const {
    QList<int> photos;
    appendPhotos(cell, photos);
    return photos;
}

void PhotoGrid::setStacks(const QVariantList &stacks) {
    if (stacks == m_stacksGiven)
        return;
    m_stacksGiven = stacks;
    m_stackLists.clear();
    for (const QVariant &stack : stacks) {
        std::vector<int> photos;
        for (const QVariant &index : stack.toList())
            photos.push_back(index.toInt());
        m_stackLists.push_back(std::move(photos));
    }
    // The photo at the top of the view stays where it is, as across a
    // reload, or the view at the newest end.
    const Anchor before = anchor();
    const QList<int> selectedBefore = selection();
    rebuildCells();
    emit stacksChanged();
    emit cellsChanged();
    if (before.atEnd)
        scrollToEnd();
    else if (before.index >= 0)
        setScroll(m_scroll + rectOf(before.index).top() - before.offset);
    else
        setScroll(m_scroll);
    if (selection() != selectedBefore)
        emit selectionChanged();
    m_lastFraction = -1;
    notifyScrolled();
    update();
}

void PhotoGrid::rebuildCells(const std::vector<int> *moved) {
    auto mapped = [&](int old) {
        if (!moved)
            return old;
        return old >= 0 && old < int(moved->size()) ? (*moved)[size_t(old)] : -1;
    };
    // What stays with its photos, as photos.
    QList<int> selected;
    for (int cell : m_selected)
        appendPhotos(cell, selected);
    for (int &i : selected)
        i = mapped(i);
    // Cells as they were (a reload has already changed the library).
    auto photoAt = [&](int cell) {
        return cell < 0 ? -1 : m_cells.empty() ? cell : cell < int(m_cells.size()) ? m_cells[size_t(cell)] : -1;
    };
    const int anchorPhoto = mapped(photoAt(m_selectionAnchor));
    const int zoomPhoto = m_zoom.active ? mapped(photoAt(m_zoom.index)) : -1;
    if (moved) {
        for (auto &stack : m_stackLists) {
            for (int &i : stack)
                i = mapped(i);
            std::erase(stack, -1);
        }
    }

    m_cells.clear();
    m_cellOf.clear();
    m_stackOf.clear();
    const int n = m_library ? m_library->count() : 0;
    // Each photo's card, by the card's position: the first stack to claim a
    // photo keeps it.
    std::vector<int> card;
    for (const auto &stack : m_stackLists) {
        std::vector<int> photos;
        for (int i : stack) {
            if (i < 0 || i >= n)
                continue;
            if (card.empty())
                card.assign(size_t(n), -1);
            if (card[size_t(i)] < 0 && std::find(photos.begin(), photos.end(), i) == photos.end())
                photos.push_back(i);
        }
        if (photos.size() < 2)
            continue;
        for (int i : photos)
            card[size_t(i)] = photos.front();
        const int cover = photos.front();
        std::sort(photos.begin(), photos.end());
        m_stackOf.emplace(cover, std::move(photos));
    }
    if (!m_stackOf.empty()) {
        m_cellOf.assign(size_t(n), -1);
        m_cells.reserve(size_t(n));
        for (int i = 0; i < n; ++i) {
            if (card[size_t(i)] < 0 || card[size_t(i)] == i) {
                m_cellOf[size_t(i)] = int(m_cells.size());
                m_cells.push_back(i);
            }
        }
        for (int i = 0; i < n; ++i)
            if (card[size_t(i)] >= 0 && card[size_t(i)] != i)
                m_cellOf[size_t(i)] = m_cellOf[size_t(card[size_t(i)])];
    }

    m_selected.clear();
    for (int i : selected)
        if (const int cell = cellOf(i); cell >= 0)
            m_selected.insert(cell);
    m_selectionAnchor = anchorPhoto >= 0 ? cellOf(anchorPhoto) : -1;
    if (zoomPhoto >= 0)
        m_zoom.index = cellOf(zoomPhoto);
}

QVariantList PhotoGrid::markColours() const {
    return QVariantList(m_markColours.begin(), m_markColours.end());
}

void PhotoGrid::setMarkColours(const QVariantList &colours) {
    std::array<QColor, kMarkKinds> given = m_markColours;
    for (int k = 0; k < std::min(kMarkKinds, int(colours.size())); ++k)
        if (const QColor c = colours[k].value<QColor>(); c.isValid())
            given[size_t(k)] = c;
    if (given == m_markColours)
        return;
    m_markColours = given;
    ++m_markColoursVersion;
    emit markColoursChanged();
    update();
}

void PhotoGrid::setMarks(int marks) {
    if (marks == m_marks)
        return;
    m_marks = marks;
    emit marksChanged();
    update();
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
    m_pressIndex = cellAt(event->position().x(), event->position().y());
    m_dragged = false;
    event->accept();
    // Any other button (a two-finger tap is the right one) was only a
    // touch: it stopped the grid above, and that is all.
    if (event->button() != Qt::LeftButton)
        m_pressIndex = -1;
}

void PhotoGrid::mouseMoveEvent(QMouseEvent *event) {
    if (m_sweeping) {
        m_sweepAt = event->position();
        sweep();
        // Within a band at the top or bottom the grid scrolls on, faster
        // the nearer the edge (and past it).
        const double band = std::min(80.0, height() / 6);
        const double y = m_sweepAt.y();
        const double depth = y < band ? -(band - y) / band : y > height() - band ? (y - (height() - band)) / band : 0;
        setDriveVelocity(std::clamp(depth, -1.0, 1.0) * 2400);
        return;
    }
    if (m_dragged || m_pressIndex < 0 || !(event->buttons() & Qt::LeftButton))
        return;
    if ((event->position() - m_pressAt).manhattanLength() < 6)
        return;
    m_dragged = true;
    // Selecting, a drag from a photo not ticked ticks the ones it crosses,
    // as in Photos.
    if (m_selecting && !m_selected.contains(m_pressIndex)) {
        m_sweeping = true;
        m_sweepBase = m_selected;
        m_sweepAt = event->position();
        sweep();
        return;
    }
    // A photo in the selection drags the selection; any other, itself.
    emit dragRequested(m_selected.contains(m_pressIndex) ? selection() : photosOf(m_pressIndex));
}

void PhotoGrid::sweep() {
    // The cell under the pointer, or off the photos (in a gap, past an
    // edge, after the last) the nearest one.
    const double s = stride();
    const int rows = (count() + m_columns - 1) / m_columns;
    const int column = std::clamp(int(m_sweepAt.x() / s), 0, m_columns - 1);
    const int row = std::clamp(int(std::floor((m_scroll + m_sweepAt.y()) / s)), 0, rows - 1);
    const int index = std::min(row * m_columns + column, count() - 1);
    if (index < 0)
        return;
    std::set<int> selected = m_sweepBase;
    for (int i = std::min(index, m_pressIndex); i <= std::max(index, m_pressIndex); ++i)
        selected.insert(i);
    m_selectionAnchor = index;
    if (selected != m_selected) {
        m_selected = std::move(selected);
        emit selectionChanged();
        update();
    }
}

void PhotoGrid::endSweep() {
    if (!m_sweeping)
        return;
    m_sweeping = false;
    m_sweepBase.clear();
    setDriveVelocity(0);
}

void PhotoGrid::mouseUngrabEvent() { endSweep(); }

void PhotoGrid::mouseReleaseEvent(QMouseEvent *event) {
    if (m_sweeping) {
        endSweep();
        return;
    }
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
            emit activated(libraryAt(index), rectOf(libraryAt(index)));
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

QList<int> PhotoGrid::selection() const {
    if (m_stackOf.empty())
        return QList<int>(m_selected.begin(), m_selected.end());
    QList<int> photos;
    for (int cell : m_selected)
        appendPhotos(cell, photos);
    std::sort(photos.begin(), photos.end());
    return photos;
}

int PhotoGrid::selectionCount() const {
    if (m_stackOf.empty())
        return int(m_selected.size());
    int n = 0;
    for (int cell : m_selected) {
        const auto stack = m_stackOf.find(libraryAt(cell));
        n += stack != m_stackOf.end() ? int(stack->second.size()) : 1;
    }
    return n;
}

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

int PhotoGrid::count() const {
    if (!m_library)
        return 0;
    return m_cells.empty() ? m_library->count() : int(m_cells.size());
}

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

void PhotoGrid::updateDateLabel() {
    QString label;
    if (m_library && count() > 0 && height() > 0) {
        // The rows on screen, in content coordinates of their level: the
        // grid's, or while a zoom moves, the shown level's layer.
        int columns = m_columns;
        double rowStride = stride(), contentTop = m_scroll, contentBottom = m_scroll + height();
        // The form follows this level: the grid's, or the one a zoom shows
        // or is landing on.
        int formatColumns = m_columns;
        if (m_zoom.active) {
            formatColumns = kLevels[size_t(m_zoom.settling ? m_zoom.target : m_zoom.shown)];
            // The rows of that level too, where it is drawn: the layer
            // underneath changes as the pair cross-fading does, and the
            // label read from it flickered between two answers.
            const std::vector<Layer> drawn = layers();
            Layer l = drawn.front();
            for (const Layer &layer : drawn)
                if (layer.columns == formatColumns)
                    l = layer;
            columns = l.columns;
            rowStride = strideFor(l.columns);
            contentTop = l.pin.y() + (0 - l.at.y()) / l.scale;
            contentBottom = l.pin.y() + (height() - l.at.y()) / l.scale;
        }
        // Far out the year at the top of the view, then the month, then the
        // range of days on screen, then, close in, the date at the top. The
        // window morphs the label as it changes.
        enum Unit { Year, Month, DayRange, Day };
        const Unit unit = formatColumns >= 23 ? Year : formatColumns >= 13 ? Month : formatColumns >= 7 ? DayRange : Day;
        const int rows = (count() + columns - 1) / columns;
        const int first = std::clamp(int(std::floor(contentTop / rowStride)), 0, rows - 1);
        const int last = std::clamp(int(std::floor(contentBottom / rowStride)), 0, rows - 1);
        const QLocale locale;
        auto dateOf = [this](int cell) { return QDateTime::fromMSecsSinceEpoch(m_library->at(libraryAt(cell)).date).date(); };
        // A row belongs to the date of its first photo.
        const QDate from = dateOf(first * columns);

        if (unit == Year) {
            label = QString::number(from.year());
        } else if (unit == Month) {
            // The month on its own (Polish "październik", not the
            // "października" of a full date), capitalised as a heading.
            QString month = locale.standaloneMonthName(from.month());
            if (!month.isEmpty())
                month[0] = month[0].toUpper();
            label = month + ' ' + QString::number(from.year());
        } else {
            label = locale.toString(from, QStringLiteral("d MMMM yyyy"));
            if (unit == DayRange) {
                const QDate to = dateOf(std::min(count() - 1, (last + 1) * columns - 1));
                if (to != from) {
                    if (to.year() != from.year())
                        label = locale.toString(from, QStringLiteral("d MMMM yyyy")) + QStringLiteral(" – ") +
                                locale.toString(to, QStringLiteral("d MMMM yyyy"));
                    else if (to.month() != from.month())
                        label = locale.toString(from, QStringLiteral("d MMMM")) + QStringLiteral(" – ") +
                                locale.toString(to, QStringLiteral("d MMMM yyyy"));
                    else
                        label = QString::number(from.day()) + QStringLiteral("–") +
                                locale.toString(to, QStringLiteral("d MMMM yyyy"));
                }
            }
        }
    }
    if (label != m_dateLabel) {
        m_dateLabel = label;
        emit dateLabelChanged();
    }
}

void PhotoGrid::notifyScrolled() {
    updateDateLabel();
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
    return m_library->at(libraryAt(index)).date;
}

QVariantList PhotoGrid::yearMarks() const {
    QVariantList marks;
    const double max = maxScroll(m_columns);
    if (count() == 0 || max <= 0)
        return marks;
    // The library is oldest first, so each year's first photo is found by
    // halving: a few hundred dates looked at rather than every one, which
    // took 140 ms at 100,000 photos on every zoom step.
    const auto dateOf = [this](int cell) { return m_library->at(libraryAt(cell)).date; };
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
    const double s = strideFor(l.columns);
    const double size = (s - gapFor(l.columns)) * l.scale;
    const double x = (index % l.columns) * s;
    const double y = (index / l.columns) * s;
    return {l.at.x() + (x - l.pin.x()) * l.scale, l.at.y() + (y - l.pin.y()) * l.scale, size, size};
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
        // level spans the window and grows or shrinks about the window's
        // middle, or the edge the fingers are near (beginZoom()): photos on
        // either side move outward, the grid never slides, and since every
        // step between levels is an even number of columns, neighbouring
        // levels have their columns on the same lines at every size.
        // (Holding the pinned photo under the fingers across as well cannot
        // work: in each level it sits in another column.) Scaled up, both
        // edges stay outside the view; scaled down, inside.
        const double left = m_zoom.focusX * (1 - scale);
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
    auto pair = [&](int a, double aWeight, int b) -> std::vector<Layer> {
        const bool aDenser = kLevels[size_t(a)] > kLevels[size_t(b)];
        const int dense = aDenser ? a : b, coarse = aDenser ? b : a;
        const double coarseWeight = aDenser ? 1 - aWeight : aWeight;
        // Alone only once it covers the window: just short of its own size,
        // the coarser level is a little narrower than the window.
        if (const Layer c = layerFor(coarse, 1); coarseWeight >= 0.999 && c.scale >= 1)
            return {c};
        if (coarseWeight <= 0.001)
            return {layerFor(dense, 1)};
        return {layerFor(dense, 1), layerFor(coarse, coarseWeight)};
    };
    // The window is always covered: underneath goes a level that spans it
    // at this cell size; were neither of the two being cross-faded to (in a
    // quick pinch both can be narrower), the coarsest one that does goes
    // underneath and the incoming level fades in over it.
    auto covered = [&](int in, double inWeight, int out) -> std::vector<Layer> {
        const int denser = kLevels[size_t(in)] > kLevels[size_t(out)] ? in : out;
        if (layerFor(denser, 1).scale >= 1 - 1e-9)
            return in == out ? std::vector<Layer>{layerFor(in, 1)} : pair(in, inWeight, out);
        int cover = 0;
        while (cover < levelCount() - 1 && strideFor(kLevels[size_t(cover)]) > m_zoom.stride + 1e-9)
            cover += 1;
        if (cover == in)
            return {layerFor(in, 1)};
        return {layerFor(cover, 1), layerFor(in, inWeight)};
    };
    if (m_zoom.settling) {
        const double w = lerp(m_zoom.fromWeight, 1, easeOut(m_zoom.t));
        return covered(m_zoom.target, w, m_zoom.partner);
    }

    // Pinching: the level nearest the cell size, and while the pinch has
    // just passed the point halfway to another, the cross-fade between the
    // two, played on its own clock (zoomTo(), advance()). Their cells
    // coincide (kLevels), so only the photos in them change.
    if (m_zoom.fade < 1 && m_zoom.fadeFrom >= 0 && m_zoom.fadeFrom != m_zoom.shown) {
        const double t = m_zoom.fade;
        return covered(m_zoom.shown, t * t * (3 - 2 * t), m_zoom.fadeFrom);
    }
    // A level shown a little smaller than its own size is narrower than the
    // window: one that spans it fills in underneath.
    return covered(m_zoom.shown, 1, m_zoom.shown);
}

int PhotoGrid::nearestLevel(double stride) const {
    int best = 0;
    for (int i = 1; i < levelCount(); ++i)
        if (std::abs(std::log(stride / strideFor(kLevels[size_t(i)]))) <
            std::abs(std::log(stride / strideFor(kLevels[size_t(best)]))))
            best = i;
    return best;
}

void PhotoGrid::beginZoom(QPointF at) {
    const bool continuing = m_zoom.active;
    m_wheelZoom.active = false;
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
    m_zoom.shown = levelForColumns(l.columns);
    m_zoom.fadeFrom = -1;
    m_zoom.fade = 1;
    // What the zoom grows about across: the middle, or an edge when the
    // fingers are near it; kept by a settle that is taken over, which is
    // still drawn about its own.
    if (!continuing)
        m_zoom.focusX = at.x() < width() * 0.2 ? 0 : at.x() > width() * 0.8 ? width() : width() / 2;
    m_pinTracked = false; // a new photo is pinned: start measuring afresh
    updateDateLabel(); // one label now, in the shown level's form
    updateAnimating();
    emit zoomingChanged();
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
    // Past the point halfway to the next level, the grid changes to it: one
    // cross-fade played to its end whatever the fingers do next. Back across
    // the point while it plays, it plays back from where it is.
    const auto distance = [&](int level) { return std::abs(std::log(stride / strideFor(kLevels[size_t(level)]))); };
    if (const int level = nearestLevel(stride);
        level != m_zoom.shown && distance(level) < distance(m_zoom.shown) - kZoomHysteresis) {
        if (level == m_zoom.fadeFrom && m_zoom.fade < 1) {
            std::swap(m_zoom.fadeFrom, m_zoom.shown);
            m_zoom.fade = 1 - m_zoom.fade;
        } else {
            m_zoom.fadeFrom = m_zoom.shown;
            m_zoom.shown = level;
            m_zoom.fade = 0;
        }
        updateAnimating();
    }
    // Moving fingers pan: the pinned point follows them.
    m_zoom.at = m_zoom.pinStart + (at - m_zoom.fingerStart);
    updateDateLabel();
    const double now = m_clock.nsecsElapsed() / 1e9;
    m_zoom.samples.emplace_back(now, std::log(stride));
    while (m_zoom.samples.size() > 2 && now - m_zoom.samples.front().first > 0.1)
        m_zoom.samples.erase(m_zoom.samples.begin());
    update();
}

void PhotoGrid::wheelZoom(double notches, QPointF at) {
    const double now = m_clock.nsecsElapsed() / 1e9;
    const bool rested = now - m_wheelZoom.lastAt > kWheelZoomRest;
    m_wheelZoom.lastAt = now;
    if (m_wheelZoom.spent && !rested)
        return;
    m_wheelZoom.spent = false;
    if (!m_wheelZoom.active) {
        // From the level shown, or the one a settle is on its way to.
        m_wheelZoom.from = m_zoom.active && m_zoom.settling ? m_zoom.target : m_level;
        beginZoom(at);
        m_wheelZoom.active = true;
        m_wheelZoom.current = m_wheelZoom.target = std::log(m_zoom.stride);
    }
    // No further than the next level either way (levels go from the
    // largest cells to the smallest); there, it lands.
    const int out = std::min(m_wheelZoom.from + 1, levelCount() - 1), in = std::max(m_wheelZoom.from - 1, 0);
    const double lo = std::log(strideFor(kLevels[size_t(out)])), hi = std::log(strideFor(kLevels[size_t(in)]));
    m_wheelZoom.target = std::clamp(m_wheelZoom.target + notches * std::log(kWheelZoomStep), lo, hi);
    if ((m_wheelZoom.target <= lo && out != m_wheelZoom.from) || (m_wheelZoom.target >= hi && in != m_wheelZoom.from)) {
        m_wheelZoom.spent = true;
        settleZoom(m_wheelZoom.target <= lo ? out : in);
        return;
    }
    updateAnimating();
}

void PhotoGrid::settleZoom(int level) {
    m_wheelZoom.active = false;
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

void PhotoGrid::endWheelZoom() {
    const double from = std::log(m_zoom.startStride), now = m_wheelZoom.current;
    if (std::abs(now - from) < kWheelZoomDead * std::log(kWheelZoomStep)) {
        settleZoom(m_wheelZoom.from);
        return;
    }
    // On to the next level the way the wheel turned (levels go from the
    // largest cells to the smallest).
    settleZoom(m_wheelZoom.from + (now > from ? -1 : 1));
}

void PhotoGrid::setColumnsNow(int columns) {
    const double fraction = maxScroll(m_columns) > 0 ? m_scroll / maxScroll(m_columns) : 1;
    m_zoom = {};
    m_level = levelForColumns(columns);
    m_columns = kLevels[size_t(m_level)];
    setScroll(fraction * maxScroll(m_columns));
    emit columnsChanged();
    updateNeon();
}

void PhotoGrid::scrollToStart() { setScroll(0); }

void PhotoGrid::scrollToEnd() { setScroll(maxScroll(m_columns)); }

bool PhotoGrid::atNewest() const { return maxScroll(m_columns) - m_scroll < height() * 0.5; }

void PhotoGrid::glideToNewest() {
    const double end = maxScroll(m_columns);
    if (end - m_scroll > height() * 1.5)
        setScroll(end - height() * 1.5);
    syncBounds();
    m_physics.jumpTo(m_scroll);
    m_physics.nudge(end - m_scroll, 0.15); // settles in about half a second
    updateAnimating();
}

void PhotoGrid::scrollPages(qreal pages) {
    syncBounds();
    m_physics.nudge(pages * height() * 0.9);
    updateAnimating();
}

void PhotoGrid::scrollByKey(int direction, bool fast) {
    syncBounds();
    const double now = m_clock.nsecsElapsed() / 1e9;
    const double since = now - m_lastKeyScroll;
    m_lastKeyScroll = now;
    // A fresh press steps; the key's repeats, coming every few hundredths
    // of a second while it is held, add up to a steady speed, which the
    // easing smooths into a glide. With Shift the glide gathers speed the
    // longer the key is held, for crossing years.
    const bool fresh = since > 0.12 || since < 0;
    if (fresh)
        m_keyHeldSince = now;
    double speed = 1400;
    if (fast) {
        const double t = std::clamp((now - m_keyHeldSince - 0.4) / 2.5, 0.0, 1.0);
        speed = 2500 + (16000 - 2500) * t * t * (3 - 2 * t);
    }
    const double step = fresh ? (fast ? height() * 0.5 : stride()) : speed * since;
    m_physics.nudge(direction * step, 0.15);
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
    m_neonClock += dt;
    const bool wasZooming = m_zoom.active;
    if (m_driveVelocity != 0) {
        setScroll(m_scroll + m_driveVelocity * dt);
        if (m_sweeping)
            sweep();
    } else if (m_physics.animating()) {
        if (m_hold)
            m_hold->poll();
        m_physics.step(dt);
        m_scroll = m_physics.position();
    }
    if (m_wheelZoom.active) {
        // The cell size eases towards where the notches point; still for a
        // moment, it lands on the nearest level as a released pinch does.
        m_wheelZoom.current = m_wheelZoom.target + (m_wheelZoom.current - m_wheelZoom.target) * std::exp(-dt / kWheelZoomEase);
        zoomTo(std::exp(m_wheelZoom.current), m_zoom.fingerStart);
        if (m_clock.nsecsElapsed() / 1e9 - m_wheelZoom.lastAt > kWheelZoomRest &&
            std::abs(m_wheelZoom.current - m_wheelZoom.target) < 0.005)
            endWheelZoom();
    }
    if (m_zoom.active && !m_zoom.settling && m_zoom.fade < 1)
        m_zoom.fade = std::min(1.0, m_zoom.fade + dt / kMorphSeconds);
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
            updateNeon();
        }
    }
    static const bool logging = qEnvironmentVariableIsSet("PINHOLE_INPUT_LOG");
    if (logging)
        logInput(QStringLiteral("frame dt=%1 scroll=%2").arg(dt * 1000, 0, 'f', 2).arg(m_scroll, 0, 'f', 1));
    updateAnimating();
    update();
    notifyScrolled();
    // After the headers: a label the landing changes still morphs.
    if (wasZooming != m_zoom.active)
        emit zoomingChanged();
}

bool PhotoGrid::neonAt(int columns) const { return columns >= kNeonFromColumns; }

void PhotoGrid::updateNeon() {
    // A zoom keeps what was lit, fading with its level; it strikes again
    // where it lands.
    if (m_zoom.active)
        return;
    const bool lit = width() > 0 && neonAt(m_columns);
    if (lit && m_neonSince < 0)
        m_neonSince = m_neonClock;
    else if (!lit)
        m_neonSince = -1;
    updateAnimating();
}

void PhotoGrid::updateAnimating() {
    const bool striking = m_neonSince >= 0 && m_neonClock - m_neonSince < kDuskFall + kNeonStagger + kNeonStrike + 0.1;
    const bool animating = m_driveVelocity != 0 || m_physics.animating() || m_wheelZoom.active ||
                           (m_zoom.active && (m_zoom.settling || m_zoom.fade < 1)) || striking;
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
    updateNeon();
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
        m_zoomNotches += event->angleDelta().y() / 120.0;
        const int notches = int(m_zoomNotches);
        if (notches != 0) {
            m_zoomNotches -= notches;
            const int from = m_zoom.active ? m_zoom.target : m_level;
            beginZoom(event->position());
            settleZoom(from - notches);
        }
        event->accept();
        return;
    }
    if (event->phase() == Qt::NoScrollPhase && std::abs(event->angleDelta().x()) > std::abs(event->angleDelta().y())) {
        // A mouse's horizontal wheel (an MX Master's thumb wheel) zooms as
        // a pinch does, about the pointer. A touchpad's sideways swipes come
        // with phases and are not this.
        wheelZoom(event->angleDelta().x() / 120.0, event->position());
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
    // Zoomed out, the same swipe carries further: √(columns / 7), so the
    // default 7 columns (about where it was tuned) is unchanged and 29 is
    // 2x. Measured from Photos on an iPhone zoomed all the way out: a swipe
    // there moves about a screen, some 2,300 photos; here, unscaled, half a
    // screen.
    const double density = std::sqrt(std::max(1.0, m_columns / double(kLevels[kDefaultLevel])));
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
        if (event->angleDelta().y() == 0 && !event->pixelDelta().isNull()) {
            // Smooth scrolling without phases or notches: follow it directly.
            setScroll(m_scroll + dy);
        } else {
            // Mouse wheel, 180 px a notch. On Wayland its notches come with
            // pixels too (15 a notch, after the compositor's scroll
            // factor), which set the distance. Glided, and sped up the
            // longer it spins (ScrollPhysics::wheel()).
            m_physics.wheel(t, (!event->pixelDelta().isNull() ? dy : -event->angleDelta().y() / 8.0 * density) * 12);
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
            slot.dusk = new QSGGeometryNode;
            auto *duskGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0);
            duskGeometry->setDrawingMode(QSGGeometry::DrawTriangles);
            slot.dusk->setGeometry(duskGeometry);
            slot.dusk->setFlag(QSGNode::OwnsGeometry);
            slot.dusk->setMaterial(new QSGVertexColorMaterial);
            slot.dusk->setFlag(QSGNode::OwnsMaterial);
            slot.opacity->appendChildNode(slot.dusk);
            slot.marks = new QSGGeometryNode;
            auto *markGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0);
            markGeometry->setDrawingMode(QSGGeometry::DrawTriangles);
            slot.marks->setGeometry(markGeometry);
            slot.marks->setFlag(QSGNode::OwnsGeometry);
            auto *markMaterial = new QSGTextureMaterial;
            markMaterial->setFiltering(QSGTexture::Linear);
            slot.marks->setMaterial(markMaterial);
            slot.marks->setFlag(QSGNode::OwnsMaterial);
            slot.opacity->appendChildNode(slot.marks);
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
    if (!m_library || !m_loader || width() <= 0 || count() == 0) {
        // Nothing to show (another source's library, still empty): nothing
        // of the last one stays on screen. Drawing anything sets them again.
        for (auto &slot : rs.drawSlots)
            if (slot.opacity->opacity() != 0)
                slot.opacity->setOpacity(0);
        if (rs.selection->geometry()->vertexCount() > 0) {
            rs.selection->geometry()->allocate(0);
            rs.selection->markDirty(QSGNode::DirtyGeometry);
        }
        return root;
    }

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
        ranges.push_back({std::max(0, int(std::floor(topLeft.y() / ls))), int(std::floor(bottomRight.y() / ls)),
                          std::clamp(int(std::floor(topLeft.x() / ls)), 0, l.columns - 1),
                          std::clamp(int(std::floor(bottomRight.x() / ls)), 0, l.columns - 1)});
    }

    // 2. Turn decoded thumbnails into textures, within this frame's budget.
    // A thumbnail whose cell has meanwhile gone far off screen is dropped:
    // uploading it would only delay the ones that are still wanted, and
    // when scrolling fast that delay is what leaves cells empty.
    // In library positions, as textures and pages are kept: a card's cell
    // shows its first photo's.
    auto keptRange = [&](int lo, int hi) {
        for (size_t k = 0; k < drawn.size(); ++k) {
            const int margin = std::max(int(height() / strideFor(drawn[k].columns) * 3) + 2, k == 0 ? lookahead + 2 : 0);
            const int from = std::max(0, ranges[k].firstRow - margin) * drawn[k].columns;
            const int to = (ranges[k].lastRow + margin) * drawn[k].columns;
            const int libraryFrom = libraryAt(std::min(from, count() - 1));
            const int libraryTo = to >= count() ? std::numeric_limits<int>::max() : libraryAt(to);
            if (hi >= libraryFrom && lo < libraryTo)
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
    // The marks on each layer's cells: where, and which.
    struct Marked {
        QRectF rect;
        int marks;
        int photo;
    };
    std::vector<std::vector<Marked>> marked(drawn.size());
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
        const double marksFromPx = neonAt(l.columns) ? kNeonMarksFromPx : kMarksFromPx;
        int lastWantedPage = -1;
        for (int row = r.firstRow; row <= r.lastRow; ++row) {
            for (int col = r.firstCol; col <= r.lastCol; ++col) {
                const int cell = row * l.columns + col;
                if (cell >= count())
                    break;
                const QRectF rect = cellRect(cell, l);
                if (!rect.intersects(view))
                    continue;
                // The photo it shows, by which its textures are kept.
                const int i = libraryAt(cell);
                const LibraryItem &item = m_library->at(i);
                if (m_marks && rect.width() >= marksFromPx) {
                    int marks = (item.video ? MarkVideo : 0) | (item.live ? MarkLive : 0) | (item.favorite ? MarkFavorite : 0);
                    if (!m_stackOf.empty() && m_stackOf.contains(i))
                        marks |= MarkStack;
                    if (marks & m_marks)
                        marked[k].push_back({rect, marks & m_marks, i});
                }
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
        const int lastPage = (m_library->count() - 1) / kMosaicCount;
        // The page of a row's first photo.
        auto pageAt = [&](int row) { return libraryAt(std::clamp(row * m_columns, 0, count() - 1)) / kMosaicCount; };
        auto wantPages = [&](int a, int b) {
            for (int p = std::max(0, a); p <= std::min(lastPage, b); ++p)
                if (!rs.pages.contains(p))
                    wanted.push_back(pageKey(p));
                else
                    rs.pages[p].lastUsed = rs.frame;
        };
        wantPages(pageAt(fromRow), pageAt(toRow + 1));
        if (down)
            wantPages(pageAt(r.firstRow) - 1, pageAt(r.firstRow) - 1);
        else
            wantPages(pageAt(r.lastRow + 1) + 1, pageAt(r.lastRow + 1) + 1);
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
                    const int cell = row * m_columns + c;
                    if (cell < 0 || cell >= count())
                        continue;
                    const int i = libraryAt(cell);
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

    // Two levels cross-fading: how far apart are their column lines?
    if (drawn.size() == 2) {
        const double cell = strideFor(drawn[0].columns) * drawn[0].scale;
        auto apart = [cell](double a, double b) {
            const double d = std::fmod(std::abs(a - b), cell);
            return std::min(d, cell - d);
        };
        const QPointF o0 = drawn[0].at - drawn[0].pin * drawn[0].scale, o1 = drawn[1].at - drawn[1].pin * drawn[1].scale;
        m_fadeFrames += 1;
        m_columnOffsetMax = std::max(m_columnOffsetMax, apart(o0.x(), o1.x()));
        m_rowOffsetMax = std::max(m_rowOffsetMax, apart(o0.y(), o1.y()));
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

    // The marks, drawn once for this screen's pixels.
    if (!rs.marks || rs.marksDpr != dpr || rs.marksVersion != m_markColoursVersion) {
        const int px = int(std::lround(kMarkMaxPx * dpr));
        QImage strip(px * kMarkKinds, px, QImage::Format_ARGB32_Premultiplied);
        strip.fill(Qt::transparent);
        {
            QPainter p(&strip);
            for (int k = 0; k < kMarkKinds; ++k)
                p.drawImage(k * px, 0, plainMark(kMarkOrder[size_t(k)], m_markColours[size_t(std::countr_zero(unsigned(kMarkOrder[size_t(k)])))], px));
        }
        // The nodes may still point at the old one.
        for (auto &slot : rs.drawSlots)
            static_cast<QSGTextureMaterial *>(slot.marks->material())->setTexture(nullptr);
        delete rs.marks;
        rs.marks = window()->createTextureFromImage(strip);
        rs.marks->setFiltering(QSGTexture::Linear);
        rs.marksDpr = dpr;
        rs.marksVersion = m_markColoursVersion;
    }
    if (!rs.neon || rs.neonDpr != dpr || rs.neonVersion != m_markColoursVersion) {
        const int px = int(std::lround(kNeonMarkPx * dpr));
        const int side = int(std::lround(px * kNeonSpread));
        // Copied in, not painted: the glows' light is premultiplied colour
        // stronger than its alpha, which painting would not keep.
        QImage strip(side * kMarkKinds, side * 2 * kNeonSteps, QImage::Format_ARGB32_Premultiplied);
        strip.fill(Qt::transparent);
        for (int k = 0; k < kMarkKinds; ++k) {
            const Mark mark = kMarkOrder[size_t(k)];
            for (int step = 0; step < kNeonSteps; ++step) {
                const NeonMark neon = neonMark(mark, m_markColours[size_t(std::countr_zero(unsigned(mark)))], px,
                                               double(step) / (kNeonSteps - 1));
                const int glowAt = step * side, coreAt = (kNeonSteps + step) * side;
                for (int y = 0; y < side; ++y) {
                    std::memcpy(strip.scanLine(glowAt + y) + size_t(k) * side * 4, neon.glow.constScanLine(y), size_t(side) * 4);
                    std::memcpy(strip.scanLine(coreAt + y) + size_t(k) * side * 4, neon.core.constScanLine(y), size_t(side) * 4);
                }
            }
        }
        for (auto &slot : rs.drawSlots)
            static_cast<QSGTextureMaterial *>(slot.marks->material())->setTexture(nullptr);
        delete rs.neon;
        rs.neon = window()->createTextureFromImage(strip);
        rs.neon->setFiltering(QSGTexture::Linear);
        rs.neonDpr = dpr;
        rs.neonVersion = m_markColoursVersion;
    }
    // A mark's corner snapped to a device pixel, so it is drawn crisp.
    auto snap = [dpr](double v) { return std::round(v * dpr) / dpr; };

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

        {
            static const std::vector<Marked> none;
            const auto &cells = size_t(k) < marked.size() ? marked[size_t(k)] : none;
            // A layer of small cells has its marks lit, glows then cores,
            // each as far as it has struck.
            const bool neon = size_t(k) < drawn.size() && neonAt(drawn[size_t(k)].columns);
            const double since = m_neonSince < 0 ? -1 : m_neonClock - m_neonSince;
            {
                // The dusk falls before the tubes strike.
                const double t = std::clamp(since / kDuskFall, 0.0, 1.0);
                const float a = float(neon && since >= 0 ? kDusk * t * t * (3 - 2 * t) : 0);
                QSGGeometry *dg = slot.dusk->geometry();
                dg->allocate(a > 0 ? 6 : 0);
                if (a > 0) {
                    auto *dv = dg->vertexDataAsColoredPoint2D();
                    const float w = float(width()), h = float(height());
                    dv[0].set(0, 0, 0, 0, 0, uchar(a * 255));
                    dv[1].set(w, 0, 0, 0, 0, uchar(a * 255));
                    dv[2].set(0, h, 0, 0, 0, uchar(a * 255));
                    dv[3].set(w, 0, 0, 0, 0, uchar(a * 255));
                    dv[4].set(w, h, 0, 0, 0, uchar(a * 255));
                    dv[5].set(0, h, 0, 0, 0, uchar(a * 255));
                }
                slot.dusk->markDirty(QSGNode::DirtyGeometry);
            }
            const int passes = neon ? 2 : 1;
            int quads = 0;
            for (const Marked &c : cells)
                quads += std::popcount(unsigned(c.marks)) * passes;
            QSGGeometry *mg = slot.marks->geometry();
            mg->allocate(quads * 6);
            auto *mv = mg->vertexDataAsTexturedPoint2D();
            const double cellWidth = cells.empty() ? 0 : cells.front().rect.width();
            const double size = !neon ? std::clamp(cellWidth * kMarkShare, kMarkPx, kMarkMaxPx)
                                : cells.empty() ? kNeonMarkPx
                                                : std::min(kNeonMarkPx, cellWidth * kNeonMarkShare);
            // A lit mark's square is kNeonSpread times as big, about the
            // same middle.
            const double grow = neon ? size * (kNeonSpread - 1) / 2 : 0;
            for (int pass = 0; pass < passes; ++pass) {
                for (const Marked &c : cells) {
                    float v0 = 0, v1 = 1;
                    if (neon) {
                        const double lit = since < kDuskFall ? 0 : neonLit(since - kDuskFall, quint32(c.photo));
                        const int row = pass * kNeonSteps + int(std::lround(lit * (kNeonSteps - 1)));
                        v0 = float(row) / (2 * kNeonSteps);
                        v1 = float(row + 1) / (2 * kNeonSteps);
                    }
                    double x = snap(c.rect.left() + kMarkInset);
                    const double y = snap(c.rect.bottom() - kMarkInset - size);
                    for (int m = 0; m < kMarkKinds; ++m) {
                        if (!(c.marks & kMarkOrder[size_t(m)]))
                            continue;
                        const float x0 = float(x - grow), y0 = float(y - grow);
                        const float x1 = float(x + size + grow), y1 = float(y + size + grow);
                        const float u0 = float(m) / kMarkKinds, u1 = float(m + 1) / kMarkKinds;
                        mv[0].set(x0, y0, u0, v0);
                        mv[1].set(x1, y0, u1, v0);
                        mv[2].set(x0, y1, u0, v1);
                        mv[3].set(x1, y0, u1, v0);
                        mv[4].set(x1, y1, u1, v1);
                        mv[5].set(x0, y1, u0, v1);
                        mv += 6;
                        // The next a little closer than a whole mark: each has
                        // room around it for its shadow.
                        x += std::round(size * 0.8);
                    }
                }
            }
            static_cast<QSGTextureMaterial *>(slot.marks->material())->setTexture(neon ? rs.neon : rs.marks);
            slot.marks->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
        }
        if (opacity > 0)
            batches += int(b.byTexture.size()) + 3;
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
        // Each: a border, and a check badge in the bottom right corner, square
        // as Omarchy draws things: the accent, with a tick in the background
        // colour.
        constexpr int kPerCell = 24 + 6 + 12;
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
        const uchar tr = uchar(m_background.red()), tg = uchar(m_background.green()), tb = uchar(m_background.blue());
        // A stroke from p to q, `t` thick, in the background colour.
        auto stroke = [&](QPointF p, QPointF q, double t) {
            const QPointF d = q - p;
            const double len = std::max(1e-6, std::hypot(d.x(), d.y()));
            const QPointF n(-d.y() / len * t / 2, d.x() / len * t / 2);
            const QPointF a = p + n, b2 = p - n, c = q + n, e = q - n;
            v[0].set(float(a.x()), float(a.y()), tr, tg, tb, 255);
            v[1].set(float(b2.x()), float(b2.y()), tr, tg, tb, 255);
            v[2].set(float(c.x()), float(c.y()), tr, tg, tb, 255);
            v[3].set(float(b2.x()), float(b2.y()), tr, tg, tb, 255);
            v[4].set(float(e.x()), float(e.y()), tr, tg, tb, 255);
            v[5].set(float(c.x()), float(c.y()), tr, tg, tb, 255);
            v += 6;
        };
        for (const QRectF &f : frames) {
            const float x0 = float(f.left()), y0 = float(f.top()), x1 = float(f.right()), y1 = float(f.bottom());
            quad(x0, y0, x1, y0 + w);
            quad(x0, y1 - w, x1, y1);
            quad(x0, y0 + w, x0 + w, y1 - w);
            quad(x1 - w, y0 + w, x1, y1 - w);
            const double radius = std::clamp(f.width() * 0.09, 6.0, 10.0);
            const double cx = f.right() - radius - 6, cy = f.bottom() - radius - 6;
            quad(float(cx - radius), float(cy - radius), float(cx + radius), float(cy + radius));
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
