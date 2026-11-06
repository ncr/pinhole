#include "maptiles.h"

#include <QDateTime>
#include <QDir>
#include <QFontMetricsF>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

namespace {
constexpr int edge = 256;
constexpr int maxZoom = 19;
// OpenFreeMap's tiles go to 14; closer, those are drawn larger.
constexpr int dataZoom = 14;
const QString planet = QStringLiteral("https://tiles.openfreemap.org/planet");
int wrap(int x, int n) { return ((x % n) + n) % n; }
qreal delta(qreal x, qreal centre) { return x - centre - std::floor(x - centre + 0.5); }
}

MapTiles::MapTiles(QQuickItem *parent) : QQuickPaintedItem(parent) {
    // A local server instead, for tests: its tiles at {z}/{x}/{y}.pbf.
    QUrl base(QString::fromUtf8(qgetenv("PINHOLE_MAP_TILES")));
    if (!base.host().isEmpty() && (base.scheme() == QStringLiteral("https") ||
        (base.scheme() == QStringLiteral("http") &&
         (base.host() == QStringLiteral("127.0.0.1") || base.host() == QStringLiteral("localhost"))))) {
        m_custom = true;
        m_template = base.toString(QUrl::StripTrailingSlash) + QStringLiteral("/{z}/{x}/{y}.pbf");
    }
    auto *disk = new QNetworkDiskCache(&m_network);
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cache.isEmpty()) {
        disk->setCacheDirectory(QDir(cache).filePath(QStringLiteral("map-vector")));
        // The raster tiles of before.
        QDir(QDir(cache).filePath(QStringLiteral("map-tiles"))).removeRecursively();
        disk->setMaximumCacheSize(160 * 1024 * 1024);
        m_network.setCache(disk);
    } else {
        delete disk;
    }
    m_language = QLocale().name().section('_', 0, 0);
    m_delay.setSingleShot(true);
    m_delay.setInterval(90);
    connect(&m_delay, &QTimer::timeout, this, &MapTiles::loadVisible);
    connect(this, &MapTiles::viewChanged, this, &MapTiles::schedule);
    connect(this, &QQuickItem::widthChanged, this, &MapTiles::schedule);
    connect(this, &QQuickItem::heightChanged, this, &MapTiles::schedule);
    connect(this, &QQuickItem::visibleChanged, this, &MapTiles::schedule);
    connect(this, &MapTiles::styleChanged, this, &MapTiles::restyle);
    connect(this, &MapTiles::avoidChanged, this, [this] { update(); });
    connect(this, &MapTiles::languageChanged, this, [this] {
        m_data.clear();
        restyle();
    });
    restyle();
    if (!m_custom)
        requestTemplate(false);
}

void MapTiles::restyle() {
    const MapStyle next = mapStyle(m_background, m_foreground, m_bright, m_blue, m_green, m_light);
    m_style = next;
    m_languages = {QStringLiteral("name:") + m_language, QStringLiteral("name:latin")};
    ++m_generation;
    m_labels.clear();
    schedule();
}

// Which of OpenFreeMap's weekly builds is current, from its TileJSON; the
// one kept from before while offline.
void MapTiles::requestTemplate(bool fromCache) {
    QNetworkRequest request{QUrl(planet)};
    request.setRawHeader("User-Agent", "Pinhole (https://github.com/ncr/pinhole)");
    request.setTransferTimeout(8000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         fromCache ? QNetworkRequest::AlwaysCache : QNetworkRequest::PreferNetwork);
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, fromCache] {
        reply->deleteLater();
        const auto tiles = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("tiles")).toArray();
        const QString url = tiles.isEmpty() ? QString() : tiles.first().toString();
        if (QUrl(url).scheme() == QStringLiteral("https")) {
            m_template = url;
            m_failed.clear();
            schedule();
        } else if (!fromCache) {
            requestTemplate(true);
        } else if (!m_unavailable) {
            m_unavailable = true;
            emit unavailableChanged();
        }
    });
}

QString MapTiles::key(int z, int x, int y) {
    return QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y);
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
    painter->fillRect(QRectF(0, 0, width(), height()), m_style.land);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    for (const auto &tile : viewport()) {
        // While a sharper one is drawn, the one drawn before, or a larger
        // tile's part.
        for (int z = tile.z; z >= std::max(0, tile.z - 4); --z) {
            const int shift = tile.z - z;
            const int parts = 1 << shift;
            if (const Drawn *drawn = m_drawn.object(key(z, tile.x >> shift, tile.y >> shift))) {
                const QSizeF whole = drawn->image.deviceIndependentSize();
                const qreal w = whole.width() / parts, h = whole.height() / parts;
                const qreal ratio = drawn->image.devicePixelRatio();
                painter->drawImage(tile.rect, drawn->image, QRectF((tile.x % parts) * w * ratio,
                    (tile.y % parts) * h * ratio, w * ratio, h * ratio));
                break;
            }
        }
    }
    drawLabels(painter);
}

void MapTiles::schedule() {
    update();
    // Repaint at once, and ask for tiles and drawings for the latest view a
    // moment later, so a long drag fills in before it ends.
    if (!m_delay.isActive())
        m_delay.start();
}

void MapTiles::retry() {
    m_failed.clear();
    if (m_template.isEmpty() && !m_custom)
        requestTemplate(false);
    loadVisible();
}

void MapTiles::fetch(int z, int x, int y) {
    const QString name = key(z, x, y);
    QString url = m_template;
    url.replace(QStringLiteral("{z}"), QString::number(z)).replace(QStringLiteral("{x}"), QString::number(x))
        .replace(QStringLiteral("{y}"), QString::number(y));
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", "Pinhole (https://github.com/ncr/pinhole)");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(10000);
    auto *reply = m_network.get(request);
    m_inFlight.insert(name, reply);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
        if (received > 4 * 1024 * 1024 || total > 4 * 1024 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, name, reply] {
        reply->deleteLater();
        if (m_inFlight.value(name) != reply)
            return;
        m_inFlight.remove(name);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && status != 204) {
            m_failed.insert(name, QDateTime::currentSecsSinceEpoch() + 60);
            schedule();
            return;
        }
        // An empty tile (open sea) is a tile with nothing on it.
        m_decoding.insert(name);
        auto *watcher = new QFutureWatcher<std::shared_ptr<const VectorTile>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, name, watcher] {
            watcher->deleteLater();
            m_decoding.remove(name);
            auto tile = watcher->result();
            if (!tile)
                tile = std::make_shared<const VectorTile>();
            m_data.insert(name, new Data{tile});
            schedule();
        });
        watcher->setFuture(QtConcurrent::run([bytes = reply->readAll(), languages = m_languages] {
            return decodeVectorTile(bytes, languages);
        }));
    });
}

// Drawn away from the window at the size it shows now, in device pixels.
void MapTiles::render(const Tile &tile, qreal size) {
    const int dz = std::min(tile.z, dataZoom);
    const int shift = tile.z - dz;
    const Data *data = m_data.object(key(dz, tile.x >> shift, tile.y >> shift));
    if (!data)
        return;
    const QString name = key(tile.z, tile.x, tile.y);
    m_rendering.insert(name);
    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1;
    const int parts = 1 << shift;
    const QRectF target(-(tile.x % parts) * size, -(tile.y % parts) * size, size * parts, size * parts);
    auto *watcher = new QFutureWatcher<QImage>(this);
    const int generation = m_generation;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, name, watcher, size, generation] {
        watcher->deleteLater();
        m_rendering.remove(name);
        const QImage image = watcher->result();
        if (generation == m_generation && !image.isNull()) {
            const int cost = std::max(1, int(image.sizeInBytes() / (256 * 256 * 4)));
            m_drawn.insert(name, new Drawn{image, size, generation}, cost);
        }
        update();
        if (!m_delay.isActive())
            m_delay.start();
    });
    watcher->setFuture(QtConcurrent::run([tile = data->tile, style = m_style, target, size, ratio, zoom = m_zoom] {
        const int pixels = int(std::ceil(size * ratio));
        QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        image.fill(style.land);
        QPainter painter(&image);
        painter.setClipRect(QRectF(0, 0, size, size));
        drawVectorTile(painter, *tile, style, target, zoom);
        return image;
    }));
}

const QImage *MapTiles::labelImage(const VectorTile::Label &label) {
    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1;
    const QString name = QStringLiteral("%1|%2|%3|%4").arg(int(label.kind)).arg(label.capital).arg(ratio).arg(label.text);
    if (const QImage *found = m_labels.object(name))
        return found;
    using K = VectorTile;
    QFont font(m_font.isEmpty() ? QStringLiteral("sans-serif") : m_font);
    qreal size = m_fontSize;
    QColor colour = m_style.label;
    QString text = label.text;
    qreal spacing = 0;
    switch (label.kind) {
    case K::Country: size *= 0.82; colour = m_style.faintLabel; text = text.toUpper(); spacing = 2; font.setWeight(QFont::Medium); break;
    case K::State: size *= 0.75; colour = m_style.faintLabel; text = text.toUpper(); spacing = 1.5; break;
    case K::City: size *= label.capital ? 1.08 : 1.0; colour = label.capital ? m_style.strongLabel : m_style.label;
        font.setWeight(label.capital ? QFont::DemiBold : QFont::Medium); break;
    case K::Town: size *= 0.9; break;
    case K::Village: case K::Suburb: size *= 0.8; colour = m_style.faintLabel; break;
    case K::Ocean: size *= 0.9; colour = m_style.waterLabel; text = text.toUpper(); spacing = 3; font.setItalic(true); break;
    case K::Sea: case K::Lake: size *= 0.82; colour = m_style.waterLabel; font.setItalic(true); break;
    case K::Kinds: break;
    }
    font.setPixelSize(std::max(8, int(std::round(size))));
    font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    QPainterPath path;
    path.addText(0, 0, font, text);
    const QRectF bounds = path.boundingRect().adjusted(-3, -3, 3, 3);
    auto *image = new QImage(QSize(int(std::ceil(bounds.width() * ratio)), int(std::ceil(bounds.height() * ratio))),
                             QImage::Format_ARGB32_Premultiplied);
    image->setDevicePixelRatio(ratio);
    image->fill(Qt::transparent);
    QPainter painter(image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(-bounds.topLeft());
    // Water names stand on the water without a halo; the rest keep clear
    // of the roads and borders under them.
    if (label.kind < K::Ocean) {
        QPen halo(m_style.halo, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.strokePath(path, halo);
    }
    painter.fillPath(path, colour);
    painter.end();
    m_labels.insert(name, image);
    return m_labels.object(name);
}

// Names over the whole view at once, so none is cut at a tile's edge: the
// most important first, the rest only where they fit.
void MapTiles::drawLabels(QPainter *painter) {
    using K = VectorTile;
    const qreal level = std::clamp(m_zoom, 0.0, qreal(maxZoom));
    const int dz = std::min(int(std::floor(level)), dataZoom);
    const int n = 1 << dz;
    const qreal world = edge * std::exp2(level);
    const qreal left = m_x * n - width() / (2 * world / n), top = m_y * n - height() / (2 * world / n);
    struct Candidate { QPointF at; const K::Label *label; int order; };
    QVector<Candidate> candidates;
    QSet<QString> seen;
    const auto shown = [level](const K::Label &l) {
        switch (l.kind) {
        case K::Country: return level < 7.5 && l.rank <= 4;
        case K::State: return false;
        // Larger cities first: OpenMapTiles ranks them 1 (largest) on.
        case K::City: return l.capital || level >= 1.5 + l.rank * 0.75;
        case K::Town: return level >= 8.5;
        case K::Village: return level >= 11;
        case K::Suburb: return level >= 13;
        case K::Ocean: return level < 6;
        case K::Sea: return level >= 3 && level < 9;
        case K::Lake: return level >= 7;
        case K::Kinds: break;
        }
        return false;
    };
    const auto order = [](const K::Label &l) {
        switch (l.kind) {
        case K::Ocean: return 0;
        case K::Country: return 1;
        case K::City: return l.capital ? 2 : 3;
        case K::Sea: return 4;
        case K::Town: return 5;
        case K::Lake: return 6;
        case K::Village: return 7;
        default: return 8;
        }
    };
    for (int y = std::max(0, int(std::floor(top))); y < n && y < top + height() / (world / n); ++y) {
        for (int x = int(std::floor(left)); x < left + width() / (world / n); ++x) {
            const Data *data = m_data.object(key(dz, wrap(x, n), y));
            if (!data)
                continue;
            for (const auto &label : data->tile->labels) {
                if (!shown(label))
                    continue;
                const QString id = QString::number(int(label.kind)) + label.text;
                if (seen.contains(id))
                    continue;
                const qreal wx = (wrap(x, n) + label.at.x() / data->tile->extent) / n;
                const qreal wy = (y + label.at.y() / data->tile->extent) / n;
                const QPointF at(width() / 2 + delta(wx, m_x) * world, height() / 2 + (wy - m_y) * world);
                if (at.x() < -100 || at.y() < -40 || at.x() > width() + 100 || at.y() > height() + 40)
                    continue;
                seen.insert(id);
                candidates.append({at, &label, order(label)});
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.order != b.order ? a.order < b.order : a.label->rank < b.label->rank;
    });
    QVector<QRectF> placed;
    QVector<QRectF> taken;
    for (const QVariant &rect : std::as_const(m_avoid))
        taken.append(rect.toRectF());
    const QRectF bounds(0, 0, width(), height());
    for (const auto &candidate : std::as_const(candidates)) {
        const QImage *image = labelImage(*candidate.label);
        if (!image)
            continue;
        const QSizeF size = image->deviceIndependentSize();
        const QRectF rect(candidate.at - QPointF(size.width() / 2, size.height() / 2), size);
        const QRectF room = rect.adjusted(-14, -8, 14, 8);
        if (!bounds.contains(rect)
                || std::any_of(placed.cbegin(), placed.cend(), [&](const QRectF &other) { return other.intersects(room); })
                || std::any_of(taken.cbegin(), taken.cend(), [&](const QRectF &other) { return other.intersects(rect); }))
            continue;
        placed.append(rect);
        painter->drawImage(rect.topLeft(), *image);
    }
}

void MapTiles::loadVisible() {
    auto tiles = isVisible() && width() > 0 && height() > 0 ? viewport() : QList<Tile>{};
    const QPointF center(width() / 2, height() / 2);
    std::sort(tiles.begin(), tiles.end(), [center](const Tile &a, const Tile &b) {
        return (a.rect.center() - center).manhattanLength() < (b.rect.center() - center).manhattanLength();
    });
    QSet<QString> needed;
    for (const auto &tile : std::as_const(tiles)) {
        const int dz = std::min(tile.z, dataZoom), shift = tile.z - dz;
        needed.insert(key(dz, tile.x >> shift, tile.y >> shift));
    }
    // Keep what is on screen, even on a large display.
    m_data.setMaxCost(std::max<qsizetype>(96, needed.size() + 32));
    m_drawn.setMaxCost(std::max<qsizetype>(192, tiles.size() * 8 + 64));
    const auto busy = m_inFlight.keys();
    for (const auto &name : busy) {
        if (!needed.contains(name))
            m_inFlight.take(name)->abort();
    }
    const auto now = QDateTime::currentSecsSinceEpoch();
    m_failed.removeIf([now](QHash<QString, qint64>::iterator entry) { return entry.value() <= now; });
    bool failed = false;
    for (const auto &tile : std::as_const(tiles)) {
        const int dz = std::min(tile.z, dataZoom), shift = tile.z - dz;
        const QString data = key(dz, tile.x >> shift, tile.y >> shift);
        if (m_data.contains(data)) {
            const QString name = key(tile.z, tile.x, tile.y);
            const Drawn *drawn = m_drawn.object(name);
            const qreal size = tile.rect.width();
            if ((!drawn || drawn->generation != m_generation || std::abs(drawn->size - size) > 0.5)
                    && !m_rendering.contains(name) && m_rendering.size() < 6)
                render(tile, size);
            continue;
        }
        if (m_inFlight.contains(data) || m_decoding.contains(data))
            continue;
        if (m_failed.contains(data)) {
            failed = true;
            continue;
        }
        if (m_template.isEmpty() || m_inFlight.size() >= 6)
            continue;
        fetch(dz, tile.x >> shift, tile.y >> shift);
    }
    if (m_unavailable != failed && !(m_template.isEmpty() && !m_custom)) {
        m_unavailable = failed;
        emit unavailableChanged();
    }
}
