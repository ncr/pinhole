#pragma once

#include <QByteArray>
#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <QVector>
#include <array>
#include <memory>

class QPainter;

// A vector tile in OpenMapTiles' schema (as OpenFreeMap serves it), decoded
// into what the map draws: areas and lines in the tile's own units (0 to
// `extent` across) and the names worth showing.
struct VectorTile {
    enum Road { Motorway, Trunk, Primary, Secondary, Tertiary, Minor, Service, Rail, Path, Roads };
    enum Kind { Country, State, City, Town, Village, Suburb, Ocean, Sea, Lake, Kinds };
    struct Label {
        QPointF at;
        QString text;
        Kind kind;
        int rank;
        bool capital;
    };
    int extent = 4096;
    QPainterPath water, park, wood, grass, ice, sand, urban, building, river, stream;
    std::array<QPainterPath, Roads> roads;
    QPainterPath country, region;
    QVector<Label> labels;
};

// `languages`: the name fields to try in turn, "name:pl" then "name:latin"…;
// "name" is always last. Null for data that is no vector tile.
std::shared_ptr<const VectorTile> decodeVectorTile(const QByteArray &data, const QStringList &languages);

// The map's colours, from the theme's: its background as the land, water a
// shade off it towards its blue, the text colours for roads and names.
struct MapStyle {
    QColor land, water, waterLine, park, wood, grass, ice, sand, urban, building;
    QColor road, majorRoad, motorway, rail, boundary, label, strongLabel, faintLabel, waterLabel, halo;
};
MapStyle mapStyle(const QColor &background, const QColor &foreground, const QColor &bright,
                  const QColor &blue, const QColor &green, bool light);

// The tile drawn at `zoom` into `target`, the whole data tile's rectangle in
// the painter's coordinates (larger than what is shown when overzoomed).
void drawVectorTile(QPainter &painter, const VectorTile &tile, const MapStyle &style, const QRectF &target, qreal zoom);
