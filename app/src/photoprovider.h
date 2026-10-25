#pragma once

#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QSize>
#include <QString>

// Decode a photo file at no more than `maxSize` (any size if invalid), with
// its orientation applied: HEIC through libheif, everything else through
// Qt's readers.
QImage decodePhoto(const QString &path, QSize maxSize);

// "image://photo/<url-encoded path>" for QML, decoded off the GUI thread.
class PhotoProvider : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;
};
