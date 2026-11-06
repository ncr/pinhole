#pragma once

#include "vectortile.h"

#include <QCache>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <memory>

// The map, drawn here from OpenFreeMap's vector tiles in the theme's
// colours, with names in the language asked for, sharp at any scale. It
// only requests the tiles intersecting its viewport. Coordinates are
// fractions of the Mercator world; the horizontal axis wraps.
class MapTiles : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(qreal centerX MEMBER m_x NOTIFY viewChanged)
    Q_PROPERTY(qreal centerY MEMBER m_y NOTIFY viewChanged)
    Q_PROPERTY(qreal zoom MEMBER m_zoom NOTIFY viewChanged)
    Q_PROPERTY(bool unavailable READ unavailable NOTIFY unavailableChanged)
    // The theme's: its background is the land.
    Q_PROPERTY(QColor background MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground MEMBER m_foreground NOTIFY styleChanged)
    Q_PROPERTY(QColor brightForeground MEMBER m_bright NOTIFY styleChanged)
    Q_PROPERTY(QColor blue MEMBER m_blue NOTIFY styleChanged)
    Q_PROPERTY(QColor green MEMBER m_green NOTIFY styleChanged)
    Q_PROPERTY(bool light MEMBER m_light NOTIFY styleChanged)
    Q_PROPERTY(QString fontFamily MEMBER m_font NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    // Names in this language where the map has them ("pl"), else in Latin
    // letters, else as written there.
    Q_PROPERTY(QString language MEMBER m_language NOTIFY languageChanged)
    Q_PROPERTY(QColor land READ land NOTIFY styleChanged)
    // Where names should not go (the photos' markers), in its coordinates.
    Q_PROPERTY(QVariantList avoid MEMBER m_avoid NOTIFY avoidChanged)
public:
    explicit MapTiles(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;
    bool unavailable() const { return m_unavailable; }
    QColor land() const { return m_style.land; }
    Q_INVOKABLE void retry();

signals:
    void viewChanged();
    void unavailableChanged();
    void styleChanged();
    void languageChanged();
    void avoidChanged();

private:
    struct Tile { int z, x, y; QRectF rect; };
    struct Data { std::shared_ptr<const VectorTile> tile; };
    struct Drawn { QImage image; qreal size; int generation; };
    QList<Tile> viewport() const;
    static QString key(int z, int x, int y);
    void schedule();
    void loadVisible();
    void restyle();
    void requestTemplate(bool fromCache);
    void fetch(int z, int x, int y);
    void render(const Tile &tile, qreal size);
    void drawLabels(QPainter *painter);
    const QImage *labelImage(const VectorTile::Label &label);

    QNetworkAccessManager m_network;
    QTimer m_delay;
    QString m_template;
    bool m_custom = false;
    QHash<QString, QNetworkReply *> m_inFlight;
    QSet<QString> m_decoding, m_rendering;
    QHash<QString, qint64> m_failed;
    QCache<QString, Data> m_data{96};
    QCache<QString, Drawn> m_drawn{192};
    QCache<QString, QImage> m_labels{600};
    QStringList m_languages;
    MapStyle m_style;
    int m_generation = 0;
    qreal m_x = 0.5, m_y = 0.5, m_zoom = 1;
    QColor m_background{Qt::black}, m_foreground{Qt::lightGray}, m_bright{Qt::white};
    QColor m_blue{QColor(0x5f, 0x87, 0xd7)}, m_green{QColor(0x87, 0xaf, 0x5f)};
    bool m_light = false;
    QString m_font;
    int m_fontSize = 13;
    QString m_language;
    QVariantList m_avoid;
    bool m_unavailable = false;
};
