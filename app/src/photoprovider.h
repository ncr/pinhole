#pragma once

#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <atomic>

// Decode a photo file at no more than `maxSize` (any size if invalid), with
// its orientation applied: HEIC through libheif, everything else through
// Qt's readers. `cancelled`, if given, set while it decodes: given up at the
// next chance, and a null image.
QImage decodePhoto(const QString &path, QSize maxSize, const std::atomic<bool> *cancelled = nullptr);

// "image://photo/<url-encoded path>" for QML, decoded off the GUI thread:
// the viewer's full size and the Photo frame's. On two threads of its own,
// not Qt's global pool of one a core: each thread decoding a HEIC at once
// grows a malloc arena of its own, which keeps what it took. A request the
// engine drops (the photo gone on before it came) stops at the next
// chance, and is never delivered. Once decodes go quiet for a few seconds,
// what they freed goes back to the system (malloc_trim), rather than
// staying with the window for good.
class PhotoProvider : public QQuickAsyncImageProvider {
public:
    PhotoProvider();
    ~PhotoProvider() override;
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // A decode is over, delivered or not (from its thread).
    void decoded();

private:
    QThreadPool m_pool;
    QTimer m_trim;
    std::atomic<int> m_decoding{0};
};
