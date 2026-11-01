#pragma once

#include "viewermotion.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QRectF>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <vector>

class Library;
class ServiceClient;

// One photo or video full-window. What it shows sharpens as it arrives: the
// thumbnail at once, then the medium rendition, then the full size, each
// from pinholed. A video plays once its file is here. A Live Photo's motion
// and effects are the Live Photos plugin's (plugins/live), over the photo.
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
    Q_PROPERTY(bool favorite READ isFavorite NOTIFY favoriteChanged)
    Q_PROPERTY(QString title READ title NOTIFY indexChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    // The full photo, or the video, still on its way (and nothing wrong).
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    // How the photos lie (viewermotion.h): the row's offset, the shown
    // photo's zoom and pan, and whether any of it is moving.
    Q_PROPERTY(qreal offset READ offset NOTIFY motionChanged)
    Q_PROPERTY(qreal stripOffset READ stripOffset NOTIFY motionChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY motionChanged)
    Q_PROPERTY(qreal panX READ panX NOTIFY motionChanged)
    Q_PROPERTY(qreal panY READ panY NOTIFY motionChanged)
    // Dragged up or down to close: how far.
    Q_PROPERTY(qreal dismiss READ dismiss NOTIFY motionChanged)
    Q_PROPERTY(qreal pageWidth READ pageWidth NOTIFY motionChanged)
    Q_PROPERTY(bool animating READ animating NOTIFY motionChanged)
    // Whether ← and →, the wheel and the touchpad are the viewer's now (not
    // while the help, a tour or a plugin is over it). It reads the keys
    // itself, pressed and let go, to move on while one is held
    // (ViewerMotion::hold()).
    Q_PROPERTY(bool takesInput READ takesInput WRITE setTakesInput NOTIFY takesInputChanged)
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY motionChanged)
    // The row the photos lie in: the library's, without the photos hidden
    // from the grid (library positions, Main.qml), so ←, →, the carousel
    // and the ends pass over those. `count` is the photos in it, `place`
    // the shown one's place there.
    Q_PROPERTY(QVariantList hidden READ hidden WRITE setHidden NOTIFY hiddenChanged)
    Q_PROPERTY(int count READ count NOTIFY indexChanged)
    Q_PROPERTY(int place READ place NOTIFY indexChanged)
    // During a far jump (goTo()), the photo sliding out beside the new one;
    // -1 otherwise. Which photo shows at each place in the row: beside().
    Q_PROPERTY(int jumpFrom READ jumpFrom NOTIFY motionChanged)
    // Bumped when a sharper image of some photo arrives (imageFor()).
    Q_PROPERTY(int revision READ revision NOTIFY imagesChanged)

public:
    Viewer(const Library *library, ServiceClient *service, QObject *parent = nullptr);

    bool isOpen() const { return m_open; }
    int index() const { return m_index; }
    QRectF origin() const { return m_origin; }
    QUrl thumbnail() const;
    QUrl image() const { return m_image; }
    QString stage() const { return m_stage; }
    bool loading() const;
    qreal aspect() const;
    bool isVideo() const;
    QUrl videoSource() const { return m_video; }
    bool isFavorite() const;
    QString title() const;
    QString message() const { return m_message; }
    qreal offset() const { return m_motion.offset(); }
    qreal stripOffset() const { return m_motion.stripOffset(); }
    qreal zoom() const { return m_motion.zoom(); }
    qreal panX() const { return m_motion.panX(); }
    qreal panY() const { return m_motion.panY(); }
    qreal dismiss() const { return m_motion.dismiss(); }
    qreal pageWidth() const { return m_motion.pageWidth(); }
    bool animating() const { return m_motion.animating(); }
    bool takesInput() const { return m_takesInput; }
    void setTakesInput(bool takes);
    bool zoomed() const { return m_motion.zoomed(); }
    QVariantList hidden() const { return m_hiddenGiven; }
    void setHidden(const QVariantList &hidden);
    int count() const;
    int place() const;
    // The photo at a place in the row, -1 for none.
    Q_INVOKABLE int photoAt(int place) const;
    int revision() const { return m_revision; }
    int jumpFrom() const { return m_jumpFrom; }
    // The photo at place `k` of the row (0: the shown one), -1 for none:
    // the neighbours, except during a far jump, where the photo it came
    // from sits next to it and nothing further.
    Q_INVOKABLE int beside(int k) const;

    // The best image of any photo at hand now: its medium rendition once
    // fetched, else its thumbnail. For the photos beside the shown one.
    Q_INVOKABLE QUrl imageFor(int index) const;
    Q_INVOKABLE QUrl thumbnailAt(int index) const;
    Q_INVOKABLE qreal aspectAt(int index) const;
    // The area photos are fitted to: the window above the carousel.
    Q_INVOKABLE void setViewport(qreal width, qreal height);
    Q_INVOKABLE void advance(qreal seconds);
    // Zoom about a point of the viewport, or its centre by steps (keys).
    Q_INVOKABLE void zoomBy(qreal factor, qreal x, qreal y);
    Q_INVOKABLE void zoomStep(int steps);
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    // The unzoomed photo grabbed with the mouse: it follows the pointer
    // (moved this much since the last), sideways to the next photo, up or
    // down to close, as a touchpad swipe does; let go, it lands or springs.
    Q_INVOKABLE void grab();
    Q_INVOKABLE void drag(qreal dx, qreal dy);
    Q_INVOKABLE void drop();
    // A finger came to rest on the touchpad (TouchpadHold): stop coasting.
    void touch(double timeMs);
    // A press or tap of any kind: stop coasting.
    Q_INVOKABLE void stopCoasting();

    // `from` is the photo's cell in the grid, where the opening grows from.
    Q_INVOKABLE void openAt(int index, QRectF from);
    Q_INVOKABLE void close();
    // The next or previous photo (keys, the mouse wheel, a click), shown at
    // once with no slide, so two neighbours can be compared by flicking
    // between them; the carousel glides after it.
    Q_INVOKABLE void step(int delta);
    // Straight to the photo at a place in the row, no slide (scrubbing the
    // carousel, the ends).
    Q_INVOKABLE void jumpTo(int place);
    // To the photo at a place in the row, sliding, the carousel with it (a
    // tap on the carousel); a neighbour at once, as step() does.
    Q_INVOKABLE void goTo(int place);
    // After the library was read again: follow the open photo to its new
    // position, or close if it is gone.
    void libraryReloaded(const std::vector<int> &moved);

signals:
    void takesInputChanged();
    void hiddenChanged();
    void openChanged();
    // openAt() called, open already or not: one shrinking away grows back.
    void openAsked();
    void indexChanged();
    // Another photo shown, or this one's favourite changed (favourites.h)
    // or read again.
    void favoriteChanged();
    void imageChanged();
    void videoChanged();
    void messageChanged();
    void loadingChanged();
    void motionChanged();
    void imagesChanged();
    // Pinched closed: the window should animate the viewer away.
    void closeRequested();

protected:
    // Installed on the window: while the viewer is open, the wheel and the
    // touchpad step through photos, and nothing behind it scrolls.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void show(int index);
    // The row's places: a photo's (a hidden one's, the place of the next
    // shown after it), and the place `direction` steps go from. A hidden
    // photo shown (a plugin opened it) lies between two places.
    int placeOf(int index) const;
    int placeFrom(int direction) const;
    // The photo `k` places from the shown one in the row, -1 for none
    // (beside(), without a far jump's).
    int rowBeside(int k) const;
    // Where a step of `delta` goes: the photo, -1 for nowhere, and its place.
    int stepTarget(int delta, int *place = nullptr) const;
    // The row again, from the hidden photos given and the library.
    void rebuildRow();
    // The held arrow let go (or lost track of): the photos stop moving on.
    void letGo();
    // Asks pinholed for the shown photo and its neighbours.
    void fetchShown();
    // The photo a touchpad swipe paged to: it slides on from where the
    // fingers left the row.
    void slide(int delta);
    void request(int index, const QString &kind);
    void setMessage(const QString &message);
    void fitPhoto();
    void remember(int index, const QString &mediumPath);
    static QUrl providerUrl(const QString &path);

    const Library *m_library;
    ServiceClient *m_service;
    // The library positions in the row, in order, while any is hidden
    // (`m_rowMapped`); the hidden ones as given, through any reload since.
    QVariantList m_hiddenGiven;
    std::vector<int> m_hiddenList;
    std::vector<int> m_row;
    bool m_rowMapped = false;
    bool m_open = false;
    int m_index = -1;
    // Replies for a photo no longer shown are dropped: each show() starts a
    // new generation.
    int m_generation = 0;
    QRectF m_origin;
    QUrl m_image;
    QString m_stage;
    QUrl m_video;
    QString m_message;
    ViewerMotion m_motion;
    QSizeF m_viewport{1, 1};
    // Mouse wheel notches not yet used; high-resolution wheels send parts.
    double m_notches = 0;
    // Times for a mouse grab's moves, as the touchpad's events carry their own.
    QElapsedTimer m_grabClock;
    // The pinch's scale so far: 1 + the sum of Qt's per-event changes.
    double m_pinchScale = 1;
    // Medium renditions fetched, by photo, while the viewer is open.
    QHash<QByteArray, QString> m_mediums;
    int m_revision = 0;
    int m_jumpFrom = -1;
    int m_jumpSide = 0;
    // Photos passing quicker than a person looks at them (a held key, the
    // carousel scrubbed) show their thumbnails and are fetched only once
    // it stops on one, so that one is not queued behind all the others.
    QElapsedTimer m_lastShown;
    QTimer m_fetchLater;
    bool m_takesInput = false;
    // The arrow held down (Qt::Key_Left or Key_Right), or 0.
    int m_heldKey = 0;
};
