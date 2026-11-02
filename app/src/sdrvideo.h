#pragma once

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QVideoFrame>
#include <QVideoSink>

#include <atomic>

// HDR video in SDR (docs/hdr.md, H2): between a MediaPlayer and its
// VideoOutput, as `MediaPlayer { videoOutput: sdr }` and
// `SdrVideo { id: sdr; output: videoOutput }`.
//
// Qt Multimedia 6.12 does not show HDR video right in SDR: decoded in
// software (10-bit planar), an HLG or PQ frame's codes are shown as if
// they were SDR's, flat and grey; decoded by VA-API (P010, as Apple
// silicon's AVD gives HEVC), its HLG shader takes no gamut into account
// and colours beyond sRGB come out as garbage. So an HLG or PQ frame is
// tone mapped here, as a still is (colour.h): its light (HLG's OOTF at
// 1000 nits), BT.2390's EETF to SDR white at 203 nits on the largest
// channel, BT.2020 to sRGB, clipped; on the CPU, in bands on Qt's thread
// pool, and handed on as an RGB frame. Any other frame is handed on as it
// is, untouched.
//
// Slow: a 4K60 HLG clip takes seven cores and shows half its frames. What
// shows video now is VideoView (videoview.h), which tone maps on the GPU;
// SdrVideo stays for plugins of API 4, and hands frames to a VideoView
// untouched.
class SdrVideo : public QObject {
    Q_OBJECT
    // The VideoOutput (or a QVideoSink) the frames go on to.
    Q_PROPERTY(QObject *output READ output WRITE setOutput NOTIFY outputChanged)
    // What the MediaPlayer gives its frames to.
    Q_PROPERTY(QVideoSink *videoSink READ videoSink CONSTANT)
    // The frames are HDR, tone mapped here.
    Q_PROPERTY(bool hdr READ hdr NOTIFY hdrChanged)
public:
    explicit SdrVideo(QObject *parent = nullptr);
    ~SdrVideo() override;

    QObject *output() const { return m_output; }
    void setOutput(QObject *output);
    // What the MediaPlayer gives its frames to (it asks for it by name).
    Q_INVOKABLE QVideoSink *videoSink() { return &m_sink; }
    bool hdr() const { return m_hdr; }

signals:
    void outputChanged();
    void hdrChanged();

private:
    void frame(const QVideoFrame &frame);

    QVideoSink m_sink;
    QPointer<QObject> m_output;
    // Held while a frame is made and handed on, on the player's thread.
    QMutex m_busy;
    QVideoSink *m_targetSink = nullptr;
    std::atomic<bool> m_hdr{false};
    // The output is a VideoView, which tone maps on the GPU as it draws:
    // every frame handed on as it is.
    bool m_passThrough = false;
    QMetaObject::Connection m_rhiFollow;
    // Pictures the frames made are drawn into, used again once shown.
    QList<QImage> m_buffers;
};

// An HLG or PQ frame in SDR, as an RGB frame of the same size, turn and
// times; null for any other (SDR, or a format not read here). Drawn into
// one of `buffers` that no frame holds any more, if any is (a few kept).
QVideoFrame toneMapFrame(const QVideoFrame &frame, QList<QImage> *buffers = nullptr);
