#pragma once

#include <QObject>
#include <QRectF>
#include <QUrl>

class Library;
class ServiceClient;

// One photo or video full-window. What it shows sharpens as it arrives: the
// thumbnail at once, then the medium rendition, then the original, each from
// pinholed. A video plays once its file is here; a Live Photo's motion plays
// on request.
class Viewer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    Q_PROPERTY(int index READ index NOTIFY indexChanged)
    Q_PROPERTY(QRectF origin READ origin NOTIFY indexChanged)
    Q_PROPERTY(QUrl thumbnail READ thumbnail NOTIFY indexChanged)
    Q_PROPERTY(QUrl image READ image NOTIFY imageChanged)
    Q_PROPERTY(QString stage READ stage NOTIFY imageChanged)
    Q_PROPERTY(qreal aspect READ aspect NOTIFY indexChanged)
    Q_PROPERTY(bool video READ isVideo NOTIFY indexChanged)
    Q_PROPERTY(QUrl videoSource READ videoSource NOTIFY videoChanged)
    Q_PROPERTY(bool live READ isLive NOTIFY indexChanged)
    Q_PROPERTY(QUrl liveSource READ liveSource NOTIFY liveChanged)
    Q_PROPERTY(bool favorite READ isFavorite NOTIFY indexChanged)
    Q_PROPERTY(QString title READ title NOTIFY indexChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)

public:
    Viewer(const Library *library, ServiceClient *service, QObject *parent = nullptr);

    bool isOpen() const { return m_open; }
    int index() const { return m_index; }
    QRectF origin() const { return m_origin; }
    QUrl thumbnail() const;
    QUrl image() const { return m_image; }
    QString stage() const { return m_stage; }
    qreal aspect() const;
    bool isVideo() const;
    QUrl videoSource() const { return m_video; }
    bool isLive() const;
    QUrl liveSource() const { return m_live; }
    bool isFavorite() const;
    QString title() const;
    QString message() const { return m_message; }

    // `from` is the photo's cell in the grid, where the opening grows from.
    Q_INVOKABLE void openAt(int index, QRectF from);
    Q_INVOKABLE void close();
    Q_INVOKABLE void step(int delta);
    Q_INVOKABLE void playLive();
    // After the library was read again: follow the open photo to its new
    // position, or close if it is gone.
    void libraryReloaded(const std::vector<int> &moved);

signals:
    void openChanged();
    void indexChanged();
    void imageChanged();
    void videoChanged();
    void liveChanged();
    void messageChanged();

private:
    void show(int index);
    void request(int index, const QString &kind);
    void setMessage(const QString &message);
    static QUrl providerUrl(const QString &path);

    const Library *m_library;
    ServiceClient *m_service;
    bool m_open = false;
    int m_index = -1;
    // Replies for a photo no longer shown are dropped: each show() starts a
    // new generation.
    int m_generation = 0;
    QRectF m_origin;
    QUrl m_image;
    QString m_stage;
    QUrl m_video;
    QUrl m_live;
    QString m_message;
};
