#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <QUrl>

#include <memory>

#include "colour.h"
#include "gainmap.h"

class ColourDisplay;
class QQmlEngine;
class QQuickWindow;
class QWindow;

// The stage (docs/hdr.md, H3): wide gamut and HDR in the viewer. A second
// window under the main one, a Wayland subsurface with an scRGB swapchain
// (linear light, 1 is SDR white, values below 0 and above 1 kept), drawing
// only the backdrop, the settled photo in its own colours and HDR video.
// The main window above it, made with an alpha channel, turns its photo
// and backdrop transparent where the stage shows and draws everything
// else, as it always does (Stage.qml, Main.qml, PhotoViewer.qml).
//
// It exists only where the display can show more than sRGB (headroom, or
// a gamut wider than sRGB, as the compositor says when the window is
// made), or when PINHOLE_STAGE=force; PINHOLE_STAGE=off never. A display
// whose Vulkan surface offers no scRGB (fp16, extended linear sRGB) has
// none, said once. Forced on such a display it runs on an ordinary
// swapchain, its output clipped to sRGB and in sRGB's curve: a stage that
// shows nothing more, for testing how it comes and goes.
class Stage : public QObject {
    Q_OBJECT
    // The window was made for a stage: with an alpha channel, its own
    // backdrop drawn.
    Q_PROPERTY(bool wanted READ wanted CONSTANT)
    // Whether its swapchain is scRGB: known once it has drawn; until then,
    // and on a forced stage without it, false.
    Q_PROPERTY(bool scrgb READ scrgb NOTIFY scrgbChanged)
    // The stage can show: wanted, and not found unable to (no scRGB and not
    // forced, or its window not placed under the main one).
    Q_PROPERTY(bool usable READ usable NOTIFY usableChanged)
    // The side panel's "Full HDR": off keeps the stage at SDR white. On by
    // default; kept in window.conf.
    Q_PROPERTY(bool fullHdr READ fullHdr WRITE setFullHdr NOTIFY fullHdrChanged)
    // Its window (Stage.qml), once made; null where there is none.
    Q_PROPERTY(QQuickWindow *window READ window NOTIFY windowChanged)
public:
    // Decided from `display` as it is now, before the window is made.
    explicit Stage(ColourDisplay *display, QObject *parent = nullptr);

    bool wanted() const { return m_wanted; }
    bool scrgb() const { return m_scrgb; }
    bool usable() const { return m_usable; }
    bool fullHdr() const { return m_fullHdr; }
    void setFullHdr(bool on);
    QQuickWindow *window() const { return m_window; }

    // Makes its window (Stage.qml) where it is wanted, once the main window
    // is: a child of it (a subsurface on Wayland), with an scRGB swapchain
    // asked for; what it got is found once it has drawn. Gone with the
    // main window.
    // Puts the stage's window under its parent, the main window, as the
    // stage's subsurface; false where it cannot be (then there is no
    // stage). Called each time it is shown, as its surface is new.
    Q_INVOKABLE bool placeBelow(QQuickWindow *window);

    // PINHOLE_STAGE: "force", "off", or "" (by the display).
    static QString mode();

signals:
    void scrgbChanged();
    void usableChanged();
    void fullHdrChanged();
    void windowChanged();

public:
    void make(QQmlEngine *engine, QQuickWindow *main);

private:
    void create(QQmlEngine *engine, QQuickWindow *main, bool scrgb);
    void setUsable(bool usable);
    QPointer<QQuickWindow> m_window;
    bool m_wanted = false;
    bool m_forced = false;
    bool m_scrgb = false;
    bool m_usable = false;
    bool m_fullHdr = true;
    bool m_checked = false;
};

// One window's subsurface placed under its parent's surface, through Qt
// Wayland's private classes (QWaylandWindow::subSurfaceWindow()): Qt's own
// lower() leaves subsurfaces where they are. Only on the Qt it was built
// against, whose classes these are; false elsewhere, and where there is no
// subsurface.
bool placeSubsurfaceBelowParent(QWindow *child);

// The photo as the stage draws it: decoded from the file the viewer's
// image is of, at the size the viewer has it, its pixels left in their own
// profile, and drawn by shaders/photo.frag: linear light by the profile's
// tables, its primaries to sRGB's, all of its gamut or clipped to sRGB's.
// Filling the item as the viewer's Image does (PreserveAspectCrop).
class StagePhoto : public QQuickItem {
    Q_OBJECT
    // The viewer's image: image://photo/<path> or image://file/<path>.
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    // The pixels it is decoded at (the viewer's image's own size); invalid
    // for the file's.
    Q_PROPERTY(QSize sourceSize READ sourceSize WRITE setSourceSize NOTIFY sourceChanged)
    Q_PROPERTY(bool mipmap READ mipmap WRITE setMipmap NOTIFY mipmapChanged)
    // 0: clipped to sRGB, the main window's photo; 1: all of its gamut.
    Q_PROPERTY(qreal gamut READ gamut WRITE setGamut NOTIFY gamutChanged)
    // Linear light out (an scRGB stage), else sRGB's curve.
    Q_PROPERTY(bool linear READ linear WRITE setLinear NOTIFY linearChanged)
    // The source and size whose pixels it draws now: the one asked for,
    // once decoded (until then, the one before).
    Q_PROPERTY(QUrl shown READ shown NOTIFY shownChanged)
    Q_PROPERTY(QSize shownSize READ shownSize NOTIFY shownChanged)
    // The size of the pixels it draws, as decoded.
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY shownChanged)
    // Its gain map (docs/hdr.md, H4), decoded with it on an scRGB stage:
    // "iso", "apple" or "" for none.
    Q_PROPERTY(QString gainMap READ gainMapKind NOTIFY shownChanged)
    // The display's headroom in stops, and how much of it the stage uses
    // now (its fade, holdSdr, Full HDR): the map applies at hdrWeight
    // times its own weight at that headroom.
    Q_PROPERTY(qreal headroom READ headroom WRITE setHeadroom NOTIFY weightChanged)
    Q_PROPERTY(qreal hdrWeight READ hdrWeight WRITE setHdrWeight NOTIFY weightChanged)
    // Off, the photo is drawn without its map: its SDR base (for tests).
    Q_PROPERTY(bool gainMapOn READ gainMapOn WRITE setGainMapOn NOTIFY weightChanged)
    // The weight the map is drawn at now (log domain): 0 is the base.
    Q_PROPERTY(qreal gainMapWeight READ gainMapWeight NOTIFY weightChanged)
public:
    explicit StagePhoto(QQuickItem *parent = nullptr);
    ~StagePhoto() override;

    QUrl source() const { return m_source; }
    void setSource(const QUrl &source);
    QSize sourceSize() const { return m_sourceSize; }
    void setSourceSize(const QSize &size);
    bool mipmap() const { return m_mipmap; }
    void setMipmap(bool on);
    qreal gamut() const { return m_gamut; }
    void setGamut(qreal gamut);
    bool linear() const { return m_linear; }
    void setLinear(bool linear);
    QUrl shown() const { return m_shown; }
    QSize shownSize() const { return m_shownSize; }
    QSize imageSize() const { return m_imageSize; }
    QString gainMapKind() const { return m_gainMap.kind; }
    qreal headroom() const { return m_headroom; }
    void setHeadroom(qreal stops);
    qreal hdrWeight() const { return m_hdrWeight; }
    void setHdrWeight(qreal weight);
    bool gainMapOn() const { return m_gainMapOn; }
    void setGainMapOn(bool on);
    qreal gainMapWeight() const;

    // The file an image URL of the viewer's is of.
    static QString pathOf(const QUrl &url);
    // Decodes `path` as the stage draws it (any thread), and its gain map
    // if `map` is given.
    static QImage decode(const QString &path, QSize size, StageColour &colour, const std::atomic<bool> *cancelled,
                         GainMap *map = nullptr);

signals:
    void sourceChanged();
    void mipmapChanged();
    void gamutChanged();
    void linearChanged();
    void shownChanged();
    void weightChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void releaseResources() override;

private:
    void load();
    void decoded(quint64 serial, const QImage &image, const StageColour &colour, const GainMap &map, const QUrl &source,
                 QSize size);

    QUrl m_source;
    QSize m_sourceSize;
    bool m_mipmap = false;
    qreal m_gamut = 0;
    bool m_linear = false;
    QUrl m_shown;
    QSize m_shownSize;
    QSize m_imageSize;
    QImage m_image;          // to upload, once
    bool m_imageChanged = false;
    GainMap m_gainMap;       // its image let go once uploaded
    bool m_gainMulti = false;
    qreal m_headroom = 0;
    qreal m_hdrWeight = 0;
    bool m_gainMapOn = true;
    std::unique_ptr<StageColour> m_colour;
    quint64 m_serial = 0;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};
