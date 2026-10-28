#pragma once

#include "viewermotion.h"

#include <QHash>
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
    // A Live Photo's effect: as set on the iPhone (live when none), and the
    // one shown, which choosing another here changes for as long as the
    // window runs, never in the library: live, loop, bounce, long_exposure.
    Q_PROPERTY(QString savedEffect READ savedEffect NOTIFY indexChanged)
    Q_PROPERTY(QString effect READ effect NOTIFY effectChanged)
    // The motion plays round and round (a loop, a bounce), as it shows.
    Q_PROPERTY(bool looping READ looping NOTIFY effectChanged)
    // The long exposure, once here.
    Q_PROPERTY(QUrl longExposure READ longExposure NOTIFY effectChanged)
    Q_PROPERTY(bool favorite READ isFavorite NOTIFY indexChanged)
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
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY motionChanged)
    Q_PROPERTY(int count READ count NOTIFY indexChanged)
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
    bool isLive() const;
    QUrl liveSource() const { return m_live; }
    QString savedEffect() const;
    QString effect() const { return m_effect; }
    bool looping() const;
    QUrl longExposure() const { return m_longExposure; }
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
    bool zoomed() const { return m_motion.zoomed(); }
    int count() const;
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
    // A finger came to rest on the touchpad (TouchpadHold): stop coasting.
    void touch(double timeMs);
    // A press or tap of any kind: stop coasting.
    Q_INVOKABLE void stopCoasting();

    // `from` is the photo's cell in the grid, where the opening grows from.
    Q_INVOKABLE void openAt(int index, QRectF from);
    Q_INVOKABLE void close();
    Q_INVOKABLE void step(int delta);
    // Straight to a photo, no slide (scrubbing the carousel).
    Q_INVOKABLE void jumpTo(int index);
    // To a photo, sliding, the carousel with it (a tap on the carousel).
    Q_INVOKABLE void goTo(int index);
    // Plays the motion as the effect shows it: once for Live and a long
    // exposure, from the start for a loop or a bounce.
    Q_INVOKABLE void playLive();
    Q_INVOKABLE void setEffect(const QString &effect);
    // The next of live, loop, bounce, long exposure.
    Q_INVOKABLE void nextEffect();
    // After the library was read again: follow the open photo to its new
    // position, or close if it is gone.
    void libraryReloaded(const std::vector<int> &moved);

signals:
    void openChanged();
    void indexChanged();
    void imageChanged();
    void videoChanged();
    void liveChanged();
    void effectChanged();
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
    void request(int index, const QString &kind);
    void setMessage(const QString &message);
    void fitPhoto();
    // What the effect needs fetched: a loop or a bounce to play, a long
    // exposure to show.
    void showEffect();
    // The motion the effect plays (live, loop or bounce), and the photo
    // under it: the edit, except for a long exposure set on the iPhone
    // shown as anything else, where the edit is the long exposure.
    QString motionKind() const;
    QString stillKind() const;
    void remember(int index, const QString &mediumPath);
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
    // Which motion m_live is.
    QString m_liveKind;
    QString m_effect;
    QUrl m_longExposure;
    // The effects chosen here, by photo.
    QHash<QByteArray, QString> m_chosen;
    QString m_message;
    ViewerMotion m_motion;
    QSizeF m_viewport{1, 1};
    // Mouse wheel notches not yet used; high-resolution wheels send parts.
    double m_notches = 0;
    // The pinch's scale so far: 1 + the sum of Qt's per-event changes.
    double m_pinchScale = 1;
    // Medium renditions fetched, by photo, while the viewer is open.
    QHash<QByteArray, QString> m_mediums;
    int m_revision = 0;
    int m_jumpFrom = -1;
    int m_jumpSide = 0;
};
