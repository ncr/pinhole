#include "photogrid.h"

#include "framestats.h"
#include "library.h"
#include "thumbnailloader.h"

#include <QNativeGestureEvent>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSGVertexColorMaterial>
#include <QThread>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

constexpr int kMinColumns = 1;
constexpr int kMaxColumns = 40;
// Seconds for a cell to glide to its place after the column count changes,
// and for the visual zoom to settle after a pinch.
constexpr double kTransition = 0.22;
// Inertia: velocity falls by this factor per millisecond, like a flick on a
// Mac trackpad.
constexpr double kDecayPerMs = 0.998;
// Texture data uploaded per frame, at most, and the time that may take.
// More and a frame could miss its vsync. Counted in bytes, not textures: a
// 1/8 thumbnail is 64 times cheaper than a full one.
const qint64 kUploadBytesPerFrame = qint64(qEnvironmentVariableIntValue("PINHOLE_UPLOAD_MB") > 0
                                                    ? qEnvironmentVariableIntValue("PINHOLE_UPLOAD_MB") : 8) << 20;
constexpr qint64 kUploadNanos = 3'000'000;
// Each upload also has a fixed cost on the GPU, whatever its size: frames
// with a hundred tiny uploads stalled for 20-30 ms.
const int kUploadsPerFrame = qEnvironmentVariableIntValue("PINHOLE_UPLOAD_COUNT") > 0
                                 ? qEnvironmentVariableIntValue("PINHOLE_UPLOAD_COUNT") : 1000;

double easeOut(double t) { return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), 3); }

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

// Everything the render thread owns.
struct GridRenderState {
    struct Entry {
        QSGTexture *texture[4] = {};
        QSize size[4];
        quint64 lastUsed = 0;
    };
    std::unordered_map<int, Entry> cache;
    qint64 bytes = 0;
    int textures = 0;
    quint64 frame = 0;

    // One node per texture (atlas page), keyed by the texture's comparison
    // key, plus one for the colour placeholders.
    std::unordered_map<qint64, QSGGeometryNode *> groups;
    QSGGeometryNode *placeholders = nullptr;

    double lastScroll = 0;
    qint64 lastFrameAt = 0;

    void dropAll() {
        for (auto &[index, entry] : cache)
            for (QSGTexture *t : entry.texture)
                delete t;
        cache.clear();
        bytes = 0;
        textures = 0;
    }
    ~GridRenderState() { dropAll(); }
};

PhotoGrid::PhotoGrid(QQuickItem *parent)
    : QQuickItem(parent), m_render(std::make_unique<GridRenderState>()) {
    setFlag(ItemHasContents);
    setAcceptedMouseButtons(Qt::AllButtons);
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

int PhotoGrid::count() const { return m_library ? m_library->count() : 0; }

void PhotoGrid::setStatsVisible(bool visible) {
    if (visible == m_statsVisible)
        return;
    m_statsVisible = visible;
    emit statsVisibleChanged();
}

// Layout

double PhotoGrid::strideFor(int columns) const { return width() / std::max(1, columns); }
double PhotoGrid::stride() const { return strideFor(m_columns); }

double PhotoGrid::maxScroll(int columns) const {
    const double s = strideFor(columns);
    const int rows = (count() + columns - 1) / std::max(1, columns);
    return std::max(0.0, rows * s - height());
}

void PhotoGrid::clampScroll() {
    const double max = maxScroll(m_columns);
    if (m_scroll < 0 || m_scroll > max) {
        m_scroll = std::clamp(m_scroll, 0.0, max);
        m_velocity = 0;
        m_wheelRemaining = 0;
    }
}

QRectF PhotoGrid::cellRect(int index, const Layout &l) const {
    const double s = strideFor(l.columns);
    // A hairline gap between cells, wider when cells are big.
    const double gap = l.columns <= 12 ? 2 : 1;
    const int row = index / l.columns;
    const int col = index % l.columns;
    const double x = col * s;
    const double y = row * s - l.scroll;
    const QPointF a = l.anchor;
    return {a.x() + (x - a.x()) * l.scale, a.y() + (y - a.y()) * l.scale, (s - gap) * l.scale, (s - gap) * l.scale};
}

// Switch to `columns` at visual `scale` around `anchor`, keeping the photo
// under the anchor under it, and let the cells glide there.
void PhotoGrid::relayout(int columns, double scale, QPointF anchor, bool glide) {
    columns = std::clamp(columns, kMinColumns, kMaxColumns);
    const Layout before{m_columns, m_scroll, m_scale, m_anchor};
    if (columns == m_columns && anchor == m_anchor) {
        m_scale = scale;
        update();
        return;
    }

    // The point under the anchor, as an item and a fraction within it.
    const double s0 = strideFor(m_columns);
    const double lx = m_anchor.x() + (anchor.x() - m_anchor.x()) / m_scale;
    const double ly = m_anchor.y() + (anchor.y() - m_anchor.y()) / m_scale + m_scroll;
    const int col0 = std::clamp(int(lx / s0), 0, m_columns - 1);
    const int row0 = std::max(0, int(ly / s0));
    const double fy = ly / s0 - row0;
    const int index = std::clamp(row0 * m_columns + col0, 0, std::max(0, count() - 1));

    const bool columnsChanged = columns != m_columns;
    m_columns = columns;
    m_scale = scale;
    m_anchor = anchor;
    const double s1 = strideFor(columns);
    const double y1 = (index / columns + fy) * s1;
    // With the anchor as the fixed point of the scale, the anchor shows
    // layout point `anchor` whatever the scale.
    m_scroll = y1 - anchor.y();
    clampScroll();

    if (columnsChanged) {
        if (glide) {
            m_transitionFrom = before;
            m_transitionT = 0;
        }
        emit this->columnsChanged();
    }
    updateAnimating();
    update();
}

void PhotoGrid::zoomStep(int delta) {
    const QPointF anchor = isUnderMouse() ? mapFromGlobal(QCursor::pos()) : QPointF(width() / 2, height() / 2);
    relayout(m_columns + delta, 1, anchor, true);
}

void PhotoGrid::setEffectiveStride(double effective, QPointF anchor) {
    m_pinching = true;
    m_snapT = 1;
    const int columns = std::clamp(int(std::lround(width() / effective)), kMinColumns, kMaxColumns);
    // During a pinch the column count changes several times a second; a
    // glide each time would keep every cell permanently in flight. The
    // scale is continuous, so reflowing at once reads as one motion.
    relayout(columns, effective / strideFor(columns), anchor, false);
}

void PhotoGrid::endContinuousZoom() {
    m_pinching = false;
    if (std::abs(m_scale - 1) > 1e-3) {
        m_snapFrom = m_scale;
        m_snapT = 0;
    }
    updateAnimating();
}

void PhotoGrid::scrollToStart() {
    m_velocity = 0;
    m_wheelRemaining = 0;
    m_scroll = 0;
    update();
}

void PhotoGrid::scrollToEnd() {
    m_velocity = 0;
    m_wheelRemaining = 0;
    m_scroll = maxScroll(m_columns);
    update();
}

void PhotoGrid::scrollPages(qreal pages) {
    m_wheelRemaining += pages * height() * 0.9;
    updateAnimating();
}

void PhotoGrid::setDriveVelocity(double v) {
    m_driveVelocity = v;
    m_velocity = 0;
    updateAnimating();
}

void PhotoGrid::fling(double v) {
    m_velocity = v;
    updateAnimating();
}

void PhotoGrid::jumpToFraction(double f) {
    m_velocity = 0;
    m_wheelRemaining = 0;
    m_scroll = std::clamp(f, 0.0, 1.0) * maxScroll(m_columns);
    update();
}

// Motion

void PhotoGrid::advance(qreal dt) {
    dt = std::clamp(dt, 0.0, 0.05);
    if (m_driveVelocity != 0) {
        m_scroll += m_driveVelocity * dt;
        clampScroll();
    } else if (m_velocity != 0) {
        m_scroll += m_velocity * dt;
        m_velocity *= std::pow(kDecayPerMs, dt * 1000);
        if (std::abs(m_velocity) < 8)
            m_velocity = 0;
        clampScroll();
    }
    if (m_wheelRemaining != 0) {
        // Mouse wheel notches ease in over ~80 ms instead of jumping.
        const double step = m_wheelRemaining * (1 - std::exp(-dt / 0.08));
        m_scroll += step;
        m_wheelRemaining -= step;
        if (std::abs(m_wheelRemaining) < 0.5)
            m_wheelRemaining = 0;
        clampScroll();
    }
    if (m_transitionT < 1)
        m_transitionT = std::min(1.0, m_transitionT + dt / kTransition);
    if (m_snapT < 1) {
        m_snapT = std::min(1.0, m_snapT + dt / kTransition);
        m_scale = m_snapFrom + (1 - m_snapFrom) * easeOut(m_snapT);
    }
    updateAnimating();
    update();
}

void PhotoGrid::updateAnimating() {
    const bool animating = m_driveVelocity != 0 || m_velocity != 0 || m_wheelRemaining != 0 || m_transitionT < 1 ||
                           m_snapT < 1;
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
    clampScroll();
    update();
}

void PhotoGrid::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const int notches = event->angleDelta().y() / 120;
        if (notches != 0)
            relayout(m_columns - notches, 1, event->position(), true);
        event->accept();
        return;
    }

    const qint64 now = m_clock.nsecsElapsed();
    if (!event->pixelDelta().isNull()) {
        // Touchpad: follow the fingers exactly, and keep the motion going
        // after they lift.
        if (event->phase() == Qt::ScrollBegin) {
            m_velocity = 0;
            m_wheelSamples.clear();
        }
        const double dy = -event->pixelDelta().y();
        if (event->phase() == Qt::ScrollMomentum) {
            m_scroll += dy;
        } else if (event->phase() == Qt::ScrollEnd) {
            // Release velocity from the last 80 ms of movement.
            double distance = 0;
            qint64 first = now;
            for (const auto &s : m_wheelSamples) {
                if (now - s.at < 80'000'000) {
                    distance += s.dy;
                    first = std::min(first, s.at);
                }
            }
            const double span = std::max(0.016, (now - first) / 1e9);
            if (std::abs(distance) > 4)
                m_velocity = distance / span;
            m_wheelSamples.clear();
        } else {
            m_velocity = 0;
            m_scroll += dy;
            m_wheelSamples.push_back({now, dy});
            if (m_wheelSamples.size() > 32)
                m_wheelSamples.erase(m_wheelSamples.begin());
        }
    } else {
        // Mouse wheel: a third of a row per notch, eased.
        m_wheelRemaining += -event->angleDelta().y() / 120.0 * stride() * 0.75;
    }
    clampScroll();
    updateAnimating();
    update();
    event->accept();
}

bool PhotoGrid::event(QEvent *event) {
    if (event->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(event);
        switch (g->gestureType()) {
        case Qt::BeginNativeGesture:
            m_velocity = 0;
            break;
        case Qt::ZoomNativeGesture:
            setEffectiveStride(stride() * m_scale * (1 + g->value()), g->position());
            break;
        case Qt::EndNativeGesture:
            if (m_pinching)
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
    m_render->groups.clear();
    m_render->placeholders = nullptr;
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
        rs.groups.clear();
        rs.placeholders = new QSGGeometryNode;
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        rs.placeholders->setGeometry(geometry);
        rs.placeholders->setFlag(QSGNode::OwnsGeometry);
        rs.placeholders->setMaterial(new QSGVertexColorMaterial);
        rs.placeholders->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(rs.placeholders);
    }
    if (!m_library || !m_loader || width() <= 0 || count() == 0)
        return root;

    const qreal dpr = window()->effectiveDevicePixelRatio();

    // Speed, in rows per second, decides how sharp a thumbnail is worth
    // decoding and how far ahead to look.
    const qint64 now = m_clock.nsecsElapsed();
    const double s = stride();
    const double rowsOnScreen = height() / (s * m_scale);
    if (rs.lastFrameAt > 0) {
        const double dt = std::max(1e-3, (now - rs.lastFrameAt) / 1e9);
        const double rows = (m_scroll - rs.lastScroll) / s;
        // A jump (scrubber, Home, End) is not motion: nothing is moving once
        // it lands, so it should not be decoded as if it were.
        const bool jumped = std::abs(rows) > rowsOnScreen && m_driveVelocity == 0 && m_velocity == 0;
        m_speedRows = jumped ? 0 : m_speedRows * 0.7 + rows / dt * 0.3;
    }
    rs.lastFrameAt = now;
    rs.lastScroll = m_scroll;
    const double speed = std::abs(m_speedRows);
    // Faster than about three screens a second nobody sees detail, and the
    // decoders could not keep up anyway: coarse previews only.
    const bool scrubbing = speed > rowsOnScreen * 3;
    const bool fast = speed > rowsOnScreen * 0.75;

    // 1. What is on screen.
    const Layout layout{m_columns, m_scroll, m_scale, m_anchor};
    const double top = m_anchor.y() + (0 - m_anchor.y()) / m_scale + m_scroll;
    const double bottom = m_anchor.y() + (height() - m_anchor.y()) / m_scale + m_scroll;
    const int firstRow = std::max(0, int(std::floor(top / s)));
    const int lastRow = int(std::floor(bottom / s));
    const int first = std::min(count(), firstRow * m_columns);
    const int last = std::min(count(), (lastRow + 1) * m_columns);
    const double transition = easeOut(m_transitionT);

    // 2. Turn decoded thumbnails into textures, within this frame's budget.
    // A thumbnail whose cell has meanwhile gone far off screen is dropped:
    // uploading it would only delay the ones that are still wanted, and
    // when scrolling fast that delay is what leaves cells empty.
    const int keepFrom = std::max(0, firstRow - int(rowsOnScreen * 3) - 1) * m_columns;
    const int keepTo = (lastRow + int(rowsOnScreen * 3) + 2) * m_columns;
    int uploads = 0;
    qint64 uploadBytes = 0;
    QElapsedTimer uploadTimer;
    uploadTimer.start();
    while (uploadBytes < kUploadBytesPerFrame && uploads < kUploadsPerFrame && uploadTimer.nsecsElapsed() < kUploadNanos) {
        auto ready = m_loader->takeReady(size_t(std::min(16, kUploadsPerFrame - uploads)));
        if (ready.empty())
            break;
        for (auto &d : ready) {
            const int index = keyIndex(d.key);
            if (index < keepFrom || index >= keepTo)
                continue;
            auto &entry = rs.cache[index];
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
    const QRectF view(0, 0, width(), height());

    const double cellDevicePx = (s - 2) * m_scale * dpr;
    auto desiredLevel = [&](const LibraryItem &item) {
        const int shortEdge = std::max(1, int(std::min(item.thumbW, item.thumbH)));
        const double need = scrubbing ? cellDevicePx * 0.25 : fast ? cellDevicePx * 0.5 : cellDevicePx * 0.85;
        int level = 0;
        while (level < 3 && (shortEdge >> (level + 1)) >= need)
            level += 1;
        return level;
    };

    // 3. Build the geometry: one list of quads per texture, one for colours.
    struct Quad { QRectF rect; QRectF uv; };
    std::unordered_map<qint64, std::pair<QSGTexture *, std::vector<Quad>>> byTexture;
    std::vector<std::pair<QRectF, quint32>> colours;
    std::vector<quint64> wanted;
    wanted.reserve(size_t(last - first) * 2);
    int unresolved = 0;

    for (int i = first; i < last; ++i) {
        QRectF rect = cellRect(i, layout);
        if (m_transitionT < 1) {
            const QRectF from = cellRect(i, m_transitionFrom);
            // Cells that were far off screen just appear; flying in from
            // thousands of pixels away looks wrong.
            if (from.intersects(view.adjusted(-width(), -height(), width(), height()))) {
                rect = QRectF(from.x() + (rect.x() - from.x()) * transition,
                              from.y() + (rect.y() - from.y()) * transition,
                              from.width() + (rect.width() - from.width()) * transition,
                              from.height() + (rect.height() - from.height()) * transition);
            }
        }
        if (!rect.intersects(view))
            continue;

        const LibraryItem &item = m_library->at(i);
        const int want = desiredLevel(item);
        QSGTexture *texture = nullptr;
        int have = -1;
        auto found = rs.cache.find(i);
        if (found != rs.cache.end()) {
            auto &entry = found->second;
            // The wanted level, else anything sharper, else anything at all.
            for (int l = want; l >= 0 && !texture; --l)
                if (entry.texture[l]) { texture = entry.texture[l]; have = l; }
            for (int l = want + 1; l < 4 && !texture; ++l)
                if (entry.texture[l]) { texture = entry.texture[l]; have = l; }
            entry.lastUsed = rs.frame;
        }
        if (have > want || have < 0) {
            unresolved += 1;
            wanted.push_back(thumbKey(i, want));
        }
        if (texture) {
            const QRectF crop = squareCrop(int(item.thumbW), int(item.thumbH));
            const QRectF sub = texture->normalizedTextureSubRect();
            const QRectF uv(sub.x() + crop.x() * sub.width(), sub.y() + crop.y() * sub.height(),
                            crop.width() * sub.width(), crop.height() * sub.height());
            auto &group = byTexture[texture->comparisonKey()];
            group.first = texture;
            group.second.push_back({rect, uv});
        } else {
            colours.emplace_back(rect, item.avgColor);
        }
    }

    // 4. Look ahead in the direction of travel, so rows arrive decoded.
    if (!scrubbing) {
        const int rowsAhead = int(std::ceil(rowsOnScreen * (fast ? 2.5 : 1.0)));
        const int rowsBehind = int(std::ceil(rowsOnScreen * 0.5));
        const bool down = m_speedRows >= 0;
        auto prefetchRows = [&](int fromRow, int toRow) {
            for (int r = fromRow; r <= toRow; ++r) {
                for (int c = 0; c < m_columns; ++c) {
                    const int i = r * m_columns + c;
                    if (i < 0 || i >= count())
                        continue;
                    const int want = desiredLevel(m_library->at(i));
                    auto found = rs.cache.find(i);
                    if (found != rs.cache.end()) {
                        found->second.lastUsed = rs.frame;
                        bool good = false;
                        for (int l = want; l >= 0; --l)
                            good = good || found->second.texture[l];
                        if (good)
                            continue;
                    }
                    wanted.push_back(thumbKey(i, want));
                }
            }
        };
        if (down) {
            prefetchRows(lastRow + 1, lastRow + rowsAhead);
            prefetchRows(firstRow - rowsBehind, firstRow - 1);
        } else {
            prefetchRows(firstRow - rowsAhead, firstRow - 1);
            prefetchRows(lastRow + 1, lastRow + rowsBehind);
        }
    }
    m_loader->setWanted(wanted);

    // 5. Write the geometry.
    {
        QSGGeometry *g = rs.placeholders->geometry();
        g->allocate(int(colours.size() * 6));
        auto *v = g->vertexDataAsColoredPoint2D();
        for (const auto &[r, c] : colours) {
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
        rs.placeholders->markDirty(QSGNode::DirtyGeometry);
    }

    for (auto it = rs.groups.begin(); it != rs.groups.end();) {
        if (!byTexture.contains(it->first)) {
            // Nothing on this page this frame. The node goes, so it never
            // points at a texture that may be evicted below.
            root->removeChildNode(it->second);
            delete it->second;
            it = rs.groups.erase(it);
        } else {
            ++it;
        }
    }
    for (auto &[key, group] : byTexture) {
        QSGGeometryNode *&node = rs.groups[key];
        if (!node) {
            node = new QSGGeometryNode;
            auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            node->setGeometry(geometry);
            node->setFlag(QSGNode::OwnsGeometry);
            auto *material = new QSGOpaqueTextureMaterial;
            material->setFiltering(QSGTexture::Linear);
            node->setMaterial(material);
            node->setFlag(QSGNode::OwnsMaterial);
            root->appendChildNode(node);
        }
        auto *material = static_cast<QSGOpaqueTextureMaterial *>(node->material());
        // Any texture on the page binds the page; this one is in use this
        // frame, so it cannot be evicted before the frame is drawn.
        material->setTexture(group.first);
        QSGGeometry *g = node->geometry();
        g->allocate(int(group.second.size() * 6));
        auto *v = g->vertexDataAsTexturedPoint2D();
        for (const Quad &q : group.second) {
            const float x0 = float(q.rect.left()), y0 = float(q.rect.top());
            const float x1 = float(q.rect.right()), y1 = float(q.rect.bottom());
            const float u0 = float(q.uv.left()), v0 = float(q.uv.top());
            const float u1 = float(q.uv.right()), v1 = float(q.uv.bottom());
            v[0].set(x0, y0, u0, v0);
            v[1].set(x1, y0, u1, v0);
            v[2].set(x0, y1, u0, v1);
            v[3].set(x1, y0, u1, v0);
            v[4].set(x1, y1, u1, v1);
            v[5].set(x0, y1, u0, v1);
            v += 6;
        }
        node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
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
            for (int l = 0; l < 4; ++l) {
                if (entry.texture[l]) {
                    rs.bytes -= qint64(entry.size[l].width()) * entry.size[l].height() * 4;
                    delete entry.texture[l];
                    rs.textures -= 1;
                }
            }
            rs.cache.erase(index);
        }
    }

    m_unresolved = unresolved;
    m_textureCount = rs.textures;
    m_textureBytes = rs.bytes;
    if (m_stats) {
        const int cells = int(colours.size()) + [&] {
            int n = 0;
            for (auto &[k, g] : byTexture)
                n += int(g.second.size());
            return n;
        }();
        m_stats->frameBuilt(timer.nsecsElapsed(), cells, int(colours.size()), uploads, int(byTexture.size()) + 1,
                            evicted, uploadNanos, evictTimer.nsecsElapsed());
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
