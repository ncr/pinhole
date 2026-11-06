#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVariantMap>

// What the person did to a photo in Pinhole, never to its file: turned by
// quarters, then cut to a part of it (docs/plugins.md, Turned and cut).
// Applied wherever the photo is drawn or given away as a picture: the
// grid, the viewer, an export as JPEG, a copy. The original stays as it is.
struct Adjustment {
    // Quarter turns clockwise, 0 to 3.
    int turn = 0;
    // The part kept, of the photo as turned: fractions of its width and
    // height, (0, 0, 1, 1) for all of it.
    QRectF crop{0, 0, 1, 1};

    bool isIdentity() const { return turn == 0 && crop == QRectF(0, 0, 1, 1); }
    bool operator==(const Adjustment &other) const { return turn == other.turn && crop == other.crop; }
    bool operator!=(const Adjustment &other) const { return !(*this == other); }

    // Its size, of a picture of `size` as it comes (turned, then cut).
    QSize applied(QSize size) const;
    // The picture turned and cut.
    QImage applied(const QImage &image) const;

    // As a URL's query keeps it ("adjust=1,0.1,0,0.8,1"), and back; none
    // for the identity.
    QString query() const;
    static Adjustment fromQuery(const QString &value);
    // A URL with it, and the one it was added to with what it held.
    static QUrl withAdjustment(const QUrl &url, const Adjustment &adjustment);
    static Adjustment ofUrl(const QString &id, QString *bare = nullptr);

    // As JSON keeps it: { "turn": 1, "crop": [x, y, w, h] }.
    QVariantMap toMap() const;
    static Adjustment fromMap(const QVariantMap &map);
    // Within its bounds: a turn 0 to 3, a crop inside the picture and not
    // empty.
    Adjustment normalized() const;
    // Turned `quarters` more clockwise (negative: the other way), the part
    // kept turning with the picture.
    Adjustment turned(int quarters) const;
};

using Adjustments = QHash<QByteArray, Adjustment>;

#include <QFileSystemWatcher>
#include <QObject>
#include <mutex>

// What has been done to the photos of the library shown, kept apart from
// it (~/.local/state/pinhole/adjustments/<its folder's kind-name>.json):
// the service never writes it, and a library read again keeps it. Another
// window's change is followed through the file.
class AdjustmentStore : public QObject {
    Q_OBJECT
public:
    explicit AdjustmentStore(QObject *parent = nullptr);
    // The library whose they are now (its folder, as Library::dir()).
    void setLibrary(const QString &dir);
    // All of them now. Any thread.
    Adjustments snapshot() const;
    Adjustment of(const QByteArray &id) const;
    // These changed (the identity takes one back), kept at once.
    bool set(const Adjustments &changes);
    static QString fileFor(const QString &libraryDir);
    // A library's, read now from its file. Any thread.
    static Adjustments load(const QString &libraryDir);

signals:
    // They changed, here or in another window.
    void changed();

private:
    void read();

    mutable std::mutex m_mutex;
    Adjustments m_all;
    QString m_file;
    QFileSystemWatcher m_watcher;
};
