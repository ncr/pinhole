#pragma once

#include <QMutex>
#include <QPointer>
#include <QQuickItem>
#include <QVideoFrame>
#include <QVideoSink>

#include <atomic>
#include <memory>

class QVideoFrameTexturePool;
class QQuickWindow;

// A VideoView's sink, as a VideoOutput's: its frame signal also without the
// frame, which is the one QML's `onVideoFrameChanged` takes. Given the
// frame, a handler's every frame waits in JavaScript for the collector,
// and a hardware decoder runs out of surfaces within a second.
class VideoViewSink : public QVideoSink {
    Q_OBJECT
public:
    explicit VideoViewSink(QObject *parent = nullptr) : QVideoSink(parent) {
        connect(this, &QVideoSink::videoFrameChanged, this, &VideoViewSink::videoFrameChanged, Qt::DirectConnection);
    }
signals:
    void videoFrameChanged();
};

// A video drawn as it should look (docs/hdr.md, H2): Qt's VideoOutput, but
// an HLG or PQ frame tone mapped on the GPU as it is drawn
// (shaders/hdrvideo.frag), in the very maths of a still's (colour.h): its
// light (HLG's OOTF at 1000 nits; PQ's peak as the stream says, else 1000),
// BT.2390's EETF on the largest channel to SDR white times `headroom`, at
// 203 nits, BT.2020 to sRGB, clipped. Any other frame is drawn by Qt's own
// shaders, as a VideoOutput draws it.
//
//     MediaPlayer { source: url; videoOutput: view }
//     VideoView { id: view; anchors.fill: parent }
//
// Qt Multimedia 6.12 shows an HDR frame flat and grey (decoded in software)
// or with its brightest colours broken (decoded by VA-API); tone mapped on
// the CPU instead (SdrVideo), a 4K60 HLG clip took seven cores and showed
// half its frames. Here the decoder's frame goes to the GPU as it is, as a
// VideoOutput's does. Only where the GPU cannot sample its planes (no
// 16-bit textures) is a frame made SDR on the CPU first (toneMapFrame).
class VideoView : public QQuickItem {
    Q_OBJECT
    // Where the picture goes in the item, as a VideoOutput's.
    Q_PROPERTY(FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    // What a MediaPlayer gives its frames to (it asks for it by name).
    Q_PROPERTY(QVideoSink *videoSink READ videoSink CONSTANT)
    // Where the picture is drawn, in the item's coordinates (beyond it,
    // cropped).
    Q_PROPERTY(QRectF contentRect READ contentRect NOTIFY contentRectChanged)
    // As a VideoOutput's: the last frame cleared once the player stops, or
    // kept.
    Q_PROPERTY(EndOfStreamPolicy endOfStreamPolicy READ endOfStreamPolicy WRITE setEndOfStreamPolicy NOTIFY
                   endOfStreamPolicyChanged)
    // The frames are HDR (HLG or PQ), tone mapped.
    Q_PROPERTY(bool hdr READ hdr NOTIFY hdrChanged)
    // How far above SDR white the output may go, as a display's headroom:
    // 1 is SDR (the window's now). Beyond 1, for a stage that shows HDR
    // (docs/hdr.md, H3), the EETF maps to that many times SDR white and
    // the values above 1 stay, in extended sRGB.
    Q_PROPERTY(qreal headroom READ headroom WRITE setHeadroom NOTIFY headroomChanged)
    // Frames drawn so far: new ones the window took, for a test to count.
    Q_PROPERTY(int framesDrawn READ framesDrawn NOTIFY framesDrawnChanged)
    // Linear light out, for a window whose swapchain is scRGB (the stage,
    // docs/hdr.md H3): an HDR frame by Pinhole's shader without sRGB's
    // curve, any other by Qt's own shaders for such a swapchain.
    Q_PROPERTY(bool linear READ linear WRITE setLinear NOTIFY linearChanged)
    // Another view whose frames this one shows too, as they come: the
    // stage's copy of the viewer's video, which plays to the viewer's.
    Q_PROPERTY(VideoView *follows READ follows WRITE setFollows NOTIFY followsChanged)
public:
    enum FillMode {
        Stretch = Qt::IgnoreAspectRatio,
        PreserveAspectFit = Qt::KeepAspectRatio,
        PreserveAspectCrop = Qt::KeepAspectRatioByExpanding,
    };
    Q_ENUM(FillMode)
    enum EndOfStreamPolicy { ClearOutput, KeepLastFrame };
    Q_ENUM(EndOfStreamPolicy)

    explicit VideoView(QQuickItem *parent = nullptr);
    ~VideoView() override;

    FillMode fillMode() const { return m_fillMode; }
    void setFillMode(FillMode mode);
    Q_INVOKABLE QVideoSink *videoSink() { return &m_sink; }
    QRectF contentRect() const { return m_contentRect; }
    EndOfStreamPolicy endOfStreamPolicy() const { return m_endOfStream.load(); }
    void setEndOfStreamPolicy(EndOfStreamPolicy policy);
    bool hdr() const { return m_hdr; }
    qreal headroom() const { return m_headroom; }
    void setHeadroom(qreal headroom);
    int framesDrawn() const { return m_drawnShown; }
    bool linear() const { return m_linear; }
    void setLinear(bool linear);
    VideoView *follows() const { return m_follows; }
    void setFollows(VideoView *view);

signals:
    void fillModeChanged();
    void contentRectChanged();
    void endOfStreamPolicyChanged();
    void hdrChanged();
    void headroomChanged();
    void framesDrawnChanged();
    void linearChanged();
    void followsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    void geometryChange(const QRectF &now, const QRectF &before) override;
    void releaseResources() override;

private:
    // On the thread the player hands its frames over on.
    void take(const QVideoFrame &frame);
    // On the window's thread, once a frame has come.
    void arrived();
    void updateContentRect();
    void followWindow(QQuickWindow *window);

    VideoViewSink m_sink;
    QPointer<QQuickWindow> m_window;
    FillMode m_fillMode = PreserveAspectFit;
    std::atomic<EndOfStreamPolicy> m_endOfStream{ClearOutput};
    QRectF m_contentRect;
    bool m_hdr = false;
    qreal m_headroom = 1;
    bool m_linear = false;
    QPointer<VideoView> m_follows;
    QMetaObject::Connection m_following;
    // The frame to draw next, and whether it is new; held while either
    // thread touches it.
    QMutex m_lock;
    QVideoFrame m_frame;
    bool m_frameHdr = false;
    bool m_frameChanged = false;
    // Its size, turn and HDR as last seen on this thread.
    QSize m_frameSize;
    int m_frameTurn = 0;
    // The GPU samples 16-bit planes (set once the window's graphics are
    // known): else an HDR frame is made SDR on the CPU as it comes.
    std::atomic<bool> m_gpuPlanes{true};
    QList<QImage> m_cpuBuffers;
    // The planes' textures, the render thread's, shared with the node's
    // material so that they outlive whichever goes first.
    std::shared_ptr<QVideoFrameTexturePool> m_pool;
    std::atomic<int> m_drawn{0};
    int m_drawnShown = 0;
};
