#include "vectortile.h"

#include <QHash>
#include <QPainter>
#include <QPen>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Protocol Buffers, the little of them a vector tile needs.
class Reader {
public:
    Reader(const char *data, qsizetype size) : m_at(data), m_end(data + size) {}
    bool more() const { return m_at < m_end && m_ok; }
    bool ok() const { return m_ok; }
    quint64 varint() {
        quint64 value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (m_at >= m_end) {
                m_ok = false;
                return 0;
            }
            const auto byte = quint8(*m_at++);
            value |= quint64(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return value;
        }
        m_ok = false;
        return 0;
    }
    // The next field: its number and wire type, false at the end.
    bool next(quint32 *field, int *type) {
        if (!more())
            return false;
        const quint64 key = varint();
        *field = quint32(key >> 3);
        *type = int(key & 7);
        return m_ok;
    }
    Reader message() {
        const quint64 size = varint();
        if (!m_ok || size > quint64(m_end - m_at)) {
            m_ok = false;
            return Reader(m_end, 0);
        }
        Reader inner(m_at, qsizetype(size));
        m_at += size;
        return inner;
    }
    QByteArray bytes() {
        Reader inner = message();
        return QByteArray(inner.m_at, inner.m_end - inner.m_at);
    }
    double fixed(int width) {
        if (m_end - m_at < width) {
            m_ok = false;
            return 0;
        }
        double value;
        if (width == 4) {
            float f;
            std::memcpy(&f, m_at, 4);
            value = f;
        } else {
            std::memcpy(&value, m_at, 8);
        }
        m_at += width;
        return value;
    }
    void skip(int type) {
        switch (type) {
        case 0: varint(); break;
        case 1: m_at += 8; break;
        case 2: message(); break;
        case 5: m_at += 4; break;
        default: m_ok = false;
        }
        if (m_at > m_end)
            m_ok = false;
    }
    QVector<quint32> packed() {
        QVector<quint32> values;
        Reader inner = message();
        while (inner.more())
            values.append(quint32(inner.varint()));
        if (!inner.ok())
            m_ok = false;
        return values;
    }

private:
    const char *m_at;
    const char *m_end;
    bool m_ok = true;
};

qint32 zigzag(quint32 n) { return qint32(n >> 1) ^ -qint32(n & 1); }

QVariant value(Reader message) {
    QVariant result;
    quint32 field;
    int type;
    while (message.next(&field, &type)) {
        switch (field) {
        case 1: result = QString::fromUtf8(message.bytes()); break;
        case 2: result = message.fixed(4); break;
        case 3: result = message.fixed(8); break;
        case 4: result = qint64(message.varint()); break;
        case 5: result = quint64(message.varint()); break;
        case 6: { const quint64 n = message.varint(); result = qint64(n >> 1) ^ -qint64(n & 1); break; }
        case 7: result = message.varint() != 0; break;
        default: message.skip(type);
        }
    }
    return result;
}

struct Feature {
    int type = 0;
    QVector<quint32> tags, geometry;
};

// Areas and lines as one path in tile units; points as their first.
QPainterPath shape(const QVector<quint32> &geometry, bool area, QPointF *point = nullptr) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    qint32 x = 0, y = 0;
    for (qsizetype i = 0; i < geometry.size();) {
        const quint32 command = geometry[i] & 7, count = geometry[i] >> 3;
        ++i;
        if (command == 7) {
            if (area)
                path.closeSubpath();
            continue;
        }
        for (quint32 n = 0; n < count && i + 1 < geometry.size(); ++n) {
            x += zigzag(geometry[i]);
            y += zigzag(geometry[i + 1]);
            i += 2;
            if (point && path.isEmpty() && command == 1)
                *point = QPointF(x, y);
            if (command == 1)
                path.moveTo(x, y);
            else if (command == 2)
                path.lineTo(x, y);
        }
    }
    return path;
}

QPointF firstPoint(const QVector<quint32> &geometry) {
    if (geometry.size() >= 3 && (geometry[0] & 7) == 1)
        return QPointF(zigzag(geometry[1]), zigzag(geometry[2]));
    return QPointF(-1, -1);
}

VectorTile::Road roadOf(const QString &kind) {
    static const QHash<QString, VectorTile::Road> roads = {
        {"motorway", VectorTile::Motorway}, {"trunk", VectorTile::Trunk}, {"primary", VectorTile::Primary},
        {"secondary", VectorTile::Secondary}, {"tertiary", VectorTile::Tertiary}, {"minor", VectorTile::Minor},
        {"service", VectorTile::Service}, {"track", VectorTile::Path}, {"path", VectorTile::Path},
        {"rail", VectorTile::Rail}, {"transit", VectorTile::Rail}};
    return roads.value(kind, VectorTile::Roads);
}

} // namespace

std::shared_ptr<const VectorTile> decodeVectorTile(const QByteArray &data, const QStringList &languages) {
    auto tile = std::make_shared<VectorTile>();
    Reader reader(data.constData(), data.size());
    quint32 field;
    int type;
    bool any = false;
    while (reader.next(&field, &type)) {
        if (field != 3 || type != 2) {
            reader.skip(type);
            continue;
        }
        Reader layer = reader.message();
        QString name;
        QStringList keys;
        QVector<QVariant> values;
        QVector<Feature> features;
        int extent = 4096;
        quint32 f;
        int t;
        while (layer.next(&f, &t)) {
            if (f == 1 && t == 2) {
                name = QString::fromUtf8(layer.bytes());
            } else if (f == 2 && t == 2) {
                Reader message = layer.message();
                Feature feature;
                quint32 g;
                int u;
                while (message.next(&g, &u)) {
                    if (g == 2 && u == 2) feature.tags = message.packed();
                    else if (g == 3 && u == 0) feature.type = int(message.varint());
                    else if (g == 4 && u == 2) feature.geometry = message.packed();
                    else message.skip(u);
                }
                features.append(std::move(feature));
            } else if (f == 3 && t == 2) {
                keys.append(QString::fromUtf8(layer.bytes()));
            } else if (f == 4 && t == 2) {
                values.append(value(layer.message()));
            } else if (f == 5 && t == 0) {
                extent = int(layer.varint());
            } else {
                layer.skip(t);
            }
        }
        if (!layer.ok())
            return nullptr;
        any = true;
        tile->extent = extent > 0 ? extent : 4096;
        for (const Feature &feature : std::as_const(features)) {
            QHash<QString, QVariant> tags;
            for (qsizetype i = 0; i + 1 < feature.tags.size(); i += 2) {
                const auto k = qsizetype(feature.tags[i]), v = qsizetype(feature.tags[i + 1]);
                if (k < keys.size() && v < values.size())
                    tags.insert(keys[k], values[v]);
            }
            const QString kind = tags.value(QStringLiteral("class")).toString();
            const bool area = feature.type == 3, line = feature.type == 2;
            if (name == QLatin1String("water") && area) {
                tile->water.addPath(shape(feature.geometry, true));
            } else if (name == QLatin1String("landcover") && area) {
                if (kind == QLatin1String("wood")) tile->wood.addPath(shape(feature.geometry, true));
                else if (kind == QLatin1String("grass") || kind == QLatin1String("wetland")) tile->grass.addPath(shape(feature.geometry, true));
                else if (kind == QLatin1String("ice")) tile->ice.addPath(shape(feature.geometry, true));
                else if (kind == QLatin1String("sand")) tile->sand.addPath(shape(feature.geometry, true));
            } else if (name == QLatin1String("landuse") && area) {
                if (kind == QLatin1String("residential") || kind == QLatin1String("suburb") || kind == QLatin1String("neighbourhood")
                        || kind == QLatin1String("commercial") || kind == QLatin1String("industrial") || kind == QLatin1String("retail"))
                    tile->urban.addPath(shape(feature.geometry, true));
                else if (kind == QLatin1String("cemetery") || kind == QLatin1String("pitch") || kind == QLatin1String("stadium"))
                    tile->park.addPath(shape(feature.geometry, true));
            } else if (name == QLatin1String("park") && area) {
                tile->park.addPath(shape(feature.geometry, true));
            } else if (name == QLatin1String("building") && area) {
                tile->building.addPath(shape(feature.geometry, true));
            } else if (name == QLatin1String("waterway") && line) {
                (kind == QLatin1String("river") || kind == QLatin1String("canal") ? tile->river : tile->stream)
                    .addPath(shape(feature.geometry, false));
            } else if (name == QLatin1String("transportation") && line) {
                const auto road = roadOf(kind);
                if (road != VectorTile::Roads)
                    tile->roads[road].addPath(shape(feature.geometry, false));
            } else if (name == QLatin1String("boundary") && line) {
                if (tags.value(QStringLiteral("maritime")).toInt() == 1)
                    continue;
                const int level = tags.value(QStringLiteral("admin_level")).toInt();
                if (level == 2) tile->country.addPath(shape(feature.geometry, false));
                else if (level == 4) tile->region.addPath(shape(feature.geometry, false));
            } else if ((name == QLatin1String("place") || name == QLatin1String("water_name")) && feature.type == 1) {
                QString text;
                for (const QString &language : languages) {
                    text = tags.value(language).toString();
                    if (!text.isEmpty())
                        break;
                }
                if (text.isEmpty())
                    text = tags.value(QStringLiteral("name")).toString();
                const QPointF at = firstPoint(feature.geometry);
                if (text.isEmpty() || at.x() < 0 || at.y() < 0 || at.x() >= tile->extent || at.y() >= tile->extent)
                    continue;
                static const QHash<QString, VectorTile::Kind> kinds = {
                    {"country", VectorTile::Country}, {"state", VectorTile::State}, {"city", VectorTile::City},
                    {"town", VectorTile::Town}, {"village", VectorTile::Village}, {"suburb", VectorTile::Suburb},
                    {"ocean", VectorTile::Ocean}, {"sea", VectorTile::Sea}, {"bay", VectorTile::Sea},
                    {"lake", VectorTile::Lake}};
                const auto found = kinds.find(kind);
                if (found == kinds.end())
                    continue;
                // The smallest states would sit on their neighbours' names.
                static const QSet<QString> tiny = {"AD", "GI", "LI", "MC", "SM", "VA"};
                if (found.value() == VectorTile::Country && tiny.contains(tags.value(QStringLiteral("iso_a2")).toString()))
                    continue;
                tile->labels.append({at, text, found.value(), tags.value(QStringLiteral("rank"), 99).toInt(),
                                     tags.value(QStringLiteral("capital")).toInt() == 2});
            }
        }
    }
    if (!reader.ok() || !any)
        return nullptr;
    return tile;
}

namespace {
QColor mix(const QColor &a, const QColor &b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}
QColor faded(QColor colour, qreal alpha) {
    colour.setAlphaF(alpha);
    return colour;
}
} // namespace

MapStyle mapStyle(const QColor &background, const QColor &foreground, const QColor &bright,
                  const QColor &blue, const QColor &green, bool light) {
    MapStyle s;
    const QColor paper = QColor::fromRgbF(background.redF(), background.greenF(), background.blueF());
    // Land a touch off the window's background, so the map reads as a
    // surface; water deeper, leaning to the theme's blue.
    s.land = mix(paper, foreground, light ? 0.03 : 0.13);
    s.water = light ? mix(mix(paper, blue, 0.30), foreground, 0.06) : mix(mix(paper, QColor(0, 0, 0), 0.55), blue, 0.20);
    s.waterLine = mix(s.water, s.land, 0.25);
    s.park = mix(s.land, green, light ? 0.16 : 0.10);
    s.wood = mix(s.land, green, light ? 0.22 : 0.14);
    s.grass = mix(s.land, green, light ? 0.09 : 0.06);
    s.ice = mix(s.land, bright, light ? 0.6 : 0.10);
    s.sand = mix(s.land, QColor(230, 200, 140), light ? 0.18 : 0.06);
    s.urban = mix(s.land, foreground, light ? 0.05 : 0.05);
    s.building = mix(s.land, foreground, light ? 0.12 : 0.11);
    s.road = mix(s.land, foreground, light ? 0.18 : 0.16);
    s.majorRoad = mix(s.land, foreground, light ? 0.30 : 0.26);
    s.motorway = mix(s.land, bright, light ? 0.42 : 0.36);
    s.rail = mix(s.land, foreground, 0.22);
    s.boundary = mix(s.land, foreground, light ? 0.50 : 0.48);
    s.label = mix(s.land, foreground, 0.9);
    s.strongLabel = bright;
    s.faintLabel = mix(s.land, foreground, 0.62);
    s.waterLabel = mix(s.water, mix(blue, bright, 0.4), light ? 0.75 : 0.6);
    s.halo = faded(s.land, 0.85);
    return s;
}

namespace {
// Lines stay the same few pixels at any scale: wider for larger roads and
// closer zooms.
qreal roadWidth(int road, qreal zoom) {
    static const std::array<qreal, VectorTile::Roads> base = {1.6, 1.4, 1.2, 1.0, 0.8, 0.6, 0.45, 0.6, 0.45};
    static const std::array<qreal, VectorTile::Roads> from = {5, 6, 7, 9, 10, 12, 14, 10, 15};
    if (zoom < from[size_t(road)])
        return 0;
    return base[size_t(road)] * std::clamp(std::exp2((zoom - 10) * 0.42), 0.5, 7.0);
}

void fill(QPainter &painter, const QPainterPath &path, const QColor &colour) {
    if (!path.isEmpty())
        painter.fillPath(path, colour);
}

void stroke(QPainter &painter, const QPainterPath &path, const QColor &colour, qreal width,
            Qt::PenStyle style = Qt::SolidLine) {
    if (path.isEmpty() || width <= 0)
        return;
    QPen pen(colour, width, style, Qt::RoundCap, Qt::RoundJoin);
    pen.setCosmetic(true);
    painter.strokePath(path, pen);
}
} // namespace

void drawVectorTile(QPainter &painter, const VectorTile &tile, const MapStyle &s, const QRectF &target, qreal zoom) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(target.topLeft());
    painter.scale(target.width() / tile.extent, target.height() / tile.extent);
    // Far out, landcover is speckle: only from a region's scale on.
    if (zoom >= 7) {
        fill(painter, tile.urban, s.urban);
        fill(painter, tile.grass, s.grass);
        fill(painter, tile.wood, s.wood);
        fill(painter, tile.sand, s.sand);
        fill(painter, tile.park, s.park);
    }
    fill(painter, tile.ice, s.ice);
    fill(painter, tile.water, s.water);
    stroke(painter, tile.stream, s.water, zoom >= 13 ? 0.8 : 0);
    stroke(painter, tile.river, s.water, std::clamp((zoom - 7) * 0.45, 0.0, 3.0));
    if (zoom >= 14)
        fill(painter, tile.building, s.building);
    for (int road = VectorTile::Path; road >= VectorTile::Motorway; --road) {
        const qreal width = roadWidth(road, zoom);
        const QColor colour = road == VectorTile::Motorway || road == VectorTile::Trunk ? s.motorway
            : road <= VectorTile::Secondary ? s.majorRoad : road == VectorTile::Rail ? s.rail : s.road;
        stroke(painter, tile.roads[size_t(road)], colour, width,
               road == VectorTile::Rail || road == VectorTile::Path ? Qt::DashLine : Qt::SolidLine);
    }
    stroke(painter, tile.region, s.boundary, zoom >= 5 ? 0.7 : 0, Qt::DotLine);
    stroke(painter, tile.country, s.boundary, std::clamp(0.6 + zoom * 0.12, 0.8, 1.8), Qt::DashLine);
    painter.restore();
}
