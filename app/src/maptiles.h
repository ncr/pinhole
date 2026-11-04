#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QUrl>

// The map only requests tiles intersecting its current viewport. Coordinates
// are fractions of the Mercator world; the horizontal axis wraps.
class MapTiles : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(qreal centerX MEMBER m_x NOTIFY viewChanged)
    Q_PROPERTY(qreal centerY MEMBER m_y NOTIFY viewChanged)
    Q_PROPERTY(qreal zoom MEMBER m_zoom NOTIFY viewChanged)
    Q_PROPERTY(bool unavailable READ unavailable NOTIFY unavailableChanged)
public:
    explicit MapTiles(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;
    bool unavailable() const { return m_unavailable; }
    Q_INVOKABLE void retry();

signals:
    void viewChanged();
    void unavailableChanged();

private:
    struct Tile { int z, x, y; QRectF rect; };
    QList<Tile> viewport() const;
    static QString key(int z, int x, int y);
    void schedule();
    void loadVisible();

    QNetworkAccessManager m_network;
    QTimer m_delay;
    QUrl m_base;
    QHash<QString, QNetworkReply *> m_inFlight;
    QHash<QString, qint64> m_failed;
    QCache<QString, QImage> m_images{192};
    qreal m_x = 0.5, m_y = 0.5, m_zoom = 1;
    bool m_unavailable = false;
};
