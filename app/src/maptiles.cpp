#include "maptiles.h"
#include "keymap.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QImageReader>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QPainter>
#include <QQuickWindow>
#include <QSet>
#include <QStandardPaths>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr int edge = 256;
constexpr int maxZoom = 19;
int wrap(int x, int n) { return ((x % n) + n) % n; }
}

MapTiles::MapTiles(QQuickItem *parent) : QQuickPaintedItem(parent) {
    m_base = QUrl(QString::fromUtf8(qgetenv("PINHOLE_MAP_TILES")));
    if (m_base.host().isEmpty() || (m_base.scheme() != QStringLiteral("https") &&
        !(m_base.scheme() == QStringLiteral("http") &&
          (m_base.host() == QStringLiteral("127.0.0.1") || m_base.host() == QStringLiteral("localhost")))))
        m_base = QUrl(QStringLiteral("https://tile.openstreetmap.org/"));
    if (!m_base.path().endsWith('/'))
        m_base.setPath(m_base.path() + '/');
    auto *disk = new QNetworkDiskCache(&m_network);
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cache.isEmpty()) {
        disk->setCacheDirectory(QDir(cache).filePath(QStringLiteral("map-tiles")));
        disk->setMaximumCacheSize(96 * 1024 * 1024);
        m_network.setCache(disk);
    } else {
        delete disk;
    }
    m_delay.setSingleShot(true);
    m_delay.setInterval(120);
    connect(&m_delay, &QTimer::timeout, this, &MapTiles::loadVisible);
    connect(this, &MapTiles::viewChanged, this, &MapTiles::schedule);
    connect(this, &QQuickItem::widthChanged, this, &MapTiles::schedule);
    connect(this, &QQuickItem::heightChanged, this, &MapTiles::schedule);
    connect(this, &QQuickItem::visibleChanged, this, &MapTiles::schedule);
}

QString MapTiles::key(int z, int x, int y) {
    return QStringLiteral("%1/%2/%3.png").arg(z).arg(x).arg(y);
}

QList<MapTiles::Tile> MapTiles::viewport() const {
    const qreal level = std::clamp(m_zoom, 0.0, qreal(maxZoom));
    const int z = int(std::floor(level));
    const int n = 1 << z;
    const qreal size = edge * std::exp2(level - z);
    const qreal left = m_x * n - width() / (2 * size);
    const qreal top = m_y * n - height() / (2 * size);
    QList<Tile> tiles;
    for (int y = std::max(0, int(std::floor(top))); y < n && y < top + height() / size; ++y) {
        for (int x = int(std::floor(left)); x < left + width() / size; ++x)
            tiles.append({z, wrap(x, n), y, QRectF((x - left) * size, (y - top) * size, size, size)});
    }
    return tiles;
}

void MapTiles::paint(QPainter *painter) {
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    for (const auto &tile : viewport()) {
        // During a zoom, use the already loaded parent until the sharper
        // tile arrives. No parent requests are made for this fallback.
        for (int z = tile.z; z >= std::max(0, tile.z - 3); --z) {
            const int shift = tile.z - z;
            const int subdivisions = 1 << shift;
            if (const auto *image = m_images.object(key(z, tile.x >> shift, tile.y >> shift))) {
                const qreal w = qreal(image->width()) / subdivisions;
                const qreal h = qreal(image->height()) / subdivisions;
                painter->drawImage(tile.rect, *image, QRectF((tile.x % subdivisions) * w,
                    (tile.y % subdivisions) * h, w, h));
                break;
            }
        }
    }
}

void MapTiles::schedule() {
    update();
    // Repaint immediately and throttle requests to the latest viewport.
    // A long drag can still fill in the map before the gesture ends.
    if (!m_delay.isActive())
        m_delay.start();
}

void MapTiles::retry() {
    m_failed.clear();
    loadVisible();
}

MapWheel::MapWheel(QQuickItem *parent) : QQuickItem(parent) {
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        if (m_window)
            m_window->removeEventFilter(this);
        m_window = window;
        if (m_window)
            m_window->installEventFilter(this);
    });
}

bool MapWheel::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_window || event->type() != QEvent::Wheel || !isVisible() || !isEnabled())
        return false;
    const auto *wheel = static_cast<QWheelEvent *>(event);
    const auto at = mapFromScene(wheel->position());
    if (!contains(at))
        return false;
    const QPoint pixels = wheel->pixelDelta(), angle = wheel->angleDelta();
    if (Keymap::isTouchpadScroll(*wheel) && !(wheel->modifiers() & Qt::ControlModifier)) {
        const QPointF delta = pixels.isNull() ? QPointF(angle) / 8 : QPointF(pixels);
        if (!delta.isNull())
            emit pan(delta.x(), delta.y());
    } else if (const int dy = angle.y() ? angle.y() : pixels.y()) {
        emit zoomAt(at.x(), at.y(), dy / 120.0 * 0.65);
    } else if (angle.x()) {
        emit pan(angle.x() / 2.0, 0);
    }
    event->accept();
    return true;
}

void MapTiles::loadVisible() {
    QSet<QString> needed;
    auto tiles = isVisible() && width() > 0 && height() > 0 ? viewport() : QList<Tile>{};
    const QPointF center(width() / 2, height() / 2);
    std::sort(tiles.begin(), tiles.end(), [center](const Tile &a, const Tile &b) {
        return (a.rect.center() - center).manhattanLength() < (b.rect.center() - center).manhattanLength();
    });
    for (const auto &tile : std::as_const(tiles))
        needed.insert(key(tile.z, tile.x, tile.y));
    // Avoid evicting tiles that are still on screen, even on a large display.
    m_images.setMaxCost(std::max<qsizetype>(192, needed.size() + 32));
    const auto busy = m_inFlight.keys();
    for (const auto &name : busy) {
        if (!needed.contains(name))
            m_inFlight.take(name)->abort();
    }
    const auto now = QDateTime::currentSecsSinceEpoch();
    m_failed.removeIf([now](QHash<QString, qint64>::iterator entry) { return entry.value() <= now; });
    bool failed = false;
    for (const auto &tile : std::as_const(tiles)) {
        const QString name = key(tile.z, tile.x, tile.y);
        if (m_images.contains(name) || m_inFlight.contains(name))
            continue;
        if (m_failed.contains(name)) {
            failed = true;
            continue;
        }
        if (m_inFlight.size() >= 6)
            continue;
        QNetworkRequest request(m_base.resolved(QUrl(name)));
        request.setRawHeader("User-Agent", "Pinhole (https://github.com/ncr/pinhole)");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
        request.setTransferTimeout(8000);
        auto *reply = m_network.get(request);
        m_inFlight.insert(name, reply);
        connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
            if (received > 1024 * 1024 || total > 1024 * 1024)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, name, reply] {
            reply->deleteLater();
            if (m_inFlight.value(name) != reply)
                return;
            m_inFlight.remove(name);
            QImage image;
            if (reply->error() == QNetworkReply::NoError) {
                auto bytes = reply->readAll();
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::ReadOnly);
                QImageReader png(&buffer, "png");
                if (png.size() == QSize(edge, edge))
                    image = png.read();
            }
            if (!image.isNull())
                m_images.insert(name, new QImage(std::move(image)));
            else
                m_failed.insert(name, QDateTime::currentSecsSinceEpoch() + 60);
            update();
            if (!m_delay.isActive())
                loadVisible();
        });
    }
    if (m_unavailable != failed) {
        m_unavailable = failed;
        emit unavailableChanged();
    }
}
