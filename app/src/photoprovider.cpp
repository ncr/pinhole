#include "photoprovider.h"

#include <QFile>
#include <QImageReader>
#include <QRunnable>
#include <QThreadPool>
#include <QUrl>

#include <libheif/heif.h>

namespace {

QImage decodeHeif(const QString &path) {
    heif_context *context = heif_context_alloc();
    QImage out;
    heif_image_handle *handle = nullptr;
    heif_image *image = nullptr;
    if (heif_context_read_from_file(context, QFile::encodeName(path).constData(), nullptr).code == heif_error_Ok &&
        heif_context_get_primary_image_handle(context, &handle).code == heif_error_Ok &&
        // libheif applies the stored rotation and mirroring while decoding.
        heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr).code ==
            heif_error_Ok) {
        size_t stride = 0;
        const uint8_t *data = heif_image_get_plane_readonly2(image, heif_channel_interleaved, &stride);
        const int width = heif_image_get_width(image, heif_channel_interleaved);
        const int height = heif_image_get_height(image, heif_channel_interleaved);
        if (data)
            out = QImage(data, width, height, qsizetype(stride), QImage::Format_RGBA8888).copy();
    }
    if (image)
        heif_image_release(image);
    if (handle)
        heif_image_handle_release(handle);
    heif_context_free(context);
    return out;
}

class Response : public QQuickImageResponse, public QRunnable {
public:
    Response(QString path, QSize size) : m_path(std::move(path)), m_size(size) { setAutoDelete(false); }
    QQuickTextureFactory *textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(m_image); }
    void run() override {
        m_image = decodePhoto(m_path, m_size);
        emit finished();
    }

private:
    QString m_path;
    QSize m_size;
    QImage m_image;
};

} // namespace

QImage decodePhoto(const QString &path, QSize maxSize) {
    QImage image;
    if (path.endsWith(".heic", Qt::CaseInsensitive) || path.endsWith(".heif", Qt::CaseInsensitive)) {
        image = decodeHeif(path);
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        // Scaled while decoding where the format can (JPEG at 1/2, 1/4, 1/8).
        if (maxSize.isValid() && reader.size().isValid()) {
            QSize size = reader.size();
            if (reader.transformation() & QImageIOHandler::TransformationRotate90)
                size.transpose();
            if (size.width() > maxSize.width() || size.height() > maxSize.height()) {
                QSize scaled = size.scaled(maxSize, Qt::KeepAspectRatio);
                if (reader.transformation() & QImageIOHandler::TransformationRotate90)
                    scaled.transpose();
                reader.setScaledSize(scaled);
            }
        }
        image = reader.read();
    }
    if (!image.isNull() && maxSize.isValid() && (image.width() > maxSize.width() || image.height() > maxSize.height()))
        image = image.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

QQuickImageResponse *PhotoProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    auto *response = new Response(QUrl::fromPercentEncoding(id.toUtf8()), requestedSize);
    QThreadPool::globalInstance()->start(response);
    return response;
}
