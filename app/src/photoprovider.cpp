#include "photoprovider.h"

#include "colour.h"

#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QRunnable>
#include <QUrl>

#include <libheif/heif.h>
#include <malloc.h>

namespace {

// HEIC, HEIF and AVIF, which libheif reads and Qt does not.
bool heifByName(const QString &path) {
    return path.endsWith(".heic", Qt::CaseInsensitive) || path.endsWith(".heif", Qt::CaseInsensitive)
        || path.endsWith(".avif", Qt::CaseInsensitive);
}

// The profile a HEIC's pixels are in: its ICC, else its nclx as one.
QByteArray profileOf(const heif_image_handle *handle) {
    QByteArray icc(qsizetype(heif_image_handle_get_raw_color_profile_size(handle)), Qt::Uninitialized);
    if (!icc.isEmpty() && heif_image_handle_get_raw_color_profile(handle, icc.data()).code == heif_error_Ok)
        return icc;
    heif_color_profile_nclx *nclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code != heif_error_Ok)
        return {};
    icc = iccOfNclx(nclx);
    heif_nclx_color_profile_free(nclx);
    return icc;
}

// A PQ or HLG picture, decoded at 16 bits a channel and tone mapped into
// sRGB (colour.h). Null if it cannot be.
QImage decodeHdrHeif(heif_image_handle *handle, const heif_color_profile_nclx &nclx, const heif_decoding_options *options,
                     const std::atomic<bool> *cancelled) {
    const bool alpha = heif_image_handle_has_alpha_channel(handle);
    heif_image *image = nullptr;
    if (heif_decode_image(handle, &image, heif_colorspace_RGB,
                          alpha ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBB_LE, options)
            .code != heif_error_Ok)
        return {};
    QImage out;
    size_t stride = 0;
    const uint8_t *data = heif_image_get_plane_readonly2(image, heif_channel_interleaved, &stride);
    if (data && !(cancelled && *cancelled))
        out = toneMapToSrgb(data, heif_image_get_width(image, heif_channel_interleaved),
                            heif_image_get_height(image, heif_channel_interleaved), qsizetype(stride),
                            heif_image_get_bits_per_pixel_range(image, heif_channel_interleaved), alpha, nclx,
                            pqPeakOf(handle));
    heif_image_release(image);
    return out;
}

// libheif's pixels scaled straight into the image returned, or, at their
// own size, handed over as they are: no copy of a 4032x3024 photo first.
// Made sRGB once scaled, so a 12 MP photo shown at 3456 px converts 3456
// px.
QImage decodeHeif(const QString &path, QSize maxSize, const std::atomic<bool> *cancelled) {
    heif_context *context = heif_context_alloc();
    heif_decoding_options *options = heif_decoding_options_alloc();
    // Asked between tiles (an iPhone's HEIC is a grid of them).
    if (cancelled) {
        options->progress_user_data = const_cast<std::atomic<bool> *>(cancelled);
        options->cancel_decoding = [](void *data) { return int(static_cast<std::atomic<bool> *>(data)->load()); };
    }
    QImage out;
    heif_image_handle *handle = nullptr;
    heif_image *image = nullptr;
    if (heif_context_read_from_file(context, QFile::encodeName(path).constData(), nullptr).code == heif_error_Ok &&
        heif_context_get_primary_image_handle(context, &handle).code == heif_error_Ok) {
        // PQ or HLG (a camera's HDR HEIF): 16 bits a channel, tone mapped
        // into sRGB at its own size, then scaled.
        heif_color_profile_nclx *nclx = nullptr;
        if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code == heif_error_Ok && isHdrTransfer(nclx)) {
            out = decodeHdrHeif(handle, *nclx, options, cancelled);
            heif_nclx_color_profile_free(nclx);
            if (!out.isNull() && maxSize.isValid() && (out.width() > maxSize.width() || out.height() > maxSize.height()))
                out = out.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            heif_image_handle_release(handle);
            heif_decoding_options_free(options);
            heif_context_free(context);
            return out;
        }
        if (nclx)
            heif_nclx_color_profile_free(nclx);
    }
    if (handle &&
        // libheif applies the stored rotation and mirroring while decoding.
        heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options).code ==
            heif_error_Ok &&
        !(cancelled && *cancelled)) {
        size_t stride = 0;
        // Writable: converted where it is.
        uint8_t *data = heif_image_get_plane2(image, heif_channel_interleaved, &stride);
        const int width = heif_image_get_width(image, heif_channel_interleaved);
        const int height = heif_image_get_height(image, heif_channel_interleaved);
        // Opaque, as photos are: scaled as it is, not converted first.
        const QImage::Format format =
            heif_image_handle_has_alpha_channel(handle) ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888;
        if (data && maxSize.isValid() && (width > maxSize.width() || height > maxSize.height())) {
            out = QImage(data, width, height, qsizetype(stride), format)
                      .scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        } else if (data) {
            out = QImage(
                data, width, height, qsizetype(stride), format,
                [](void *image) { heif_image_release(static_cast<heif_image *>(image)); }, image);
            image = nullptr;
        }
        toSrgb(out, profileOf(handle));
    }
    if (image)
        heif_image_release(image);
    if (handle)
        heif_image_handle_release(handle);
    heif_decoding_options_free(options);
    heif_context_free(context);
    return out;
}

class Response : public QQuickImageResponse, public QRunnable {
public:
    Response(PhotoProvider *provider, QString path, QSize size)
        : m_provider(provider), m_path(std::move(path)), m_size(size) {
        setAutoDelete(false);
    }
    QQuickTextureFactory *textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(m_image); }
    // From the engine's thread, while it waits or decodes: it still says
    // it finished, so the engine lets it go, but with nothing.
    void cancel() override { m_cancelled = true; }
    void run() override {
        if (!m_cancelled)
            m_image = decodePhoto(m_path, m_size, &m_cancelled);
        if (m_cancelled)
            m_image = QImage();
        m_provider->decoded();
        emit finished();
    }

private:
    PhotoProvider *m_provider;
    QString m_path;
    QSize m_size;
    QImage m_image;
    std::atomic<bool> m_cancelled{false};
};

} // namespace

QImage decodePhoto(const QString &path, QSize maxSize, const std::atomic<bool> *cancelled) {
    QImage image;
    if (heifByName(path))
        image = decodeHeif(path, maxSize, cancelled);
    if (cancelled && *cancelled)
        return {};
    // Not HEIF after all, whatever its name says (iCloud calls a loop's
    // JPEG a HEIC): read for what it is.
    if (image.isNull()) {
        QImageReader reader(path);
        reader.setDecideFormatFromContent(true);
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
        // In the profile it carried (QImageReader keeps it, as JPEG and
        // PNG have it); a CMYK JPEG, as Qt reads one, by Qt.
        if (image.format() == QImage::Format_CMYK8888)
            image = image.colorSpace().isValid() ? image.convertedToColorSpace(QColorSpace(QColorSpace::SRgb), QImage::Format_RGB32)
                                                 : image.convertToFormat(QImage::Format_RGB32);
        else
            toSrgb(image);
    }
    if (!image.isNull() && maxSize.isValid() && (image.width() > maxSize.width() || image.height() > maxSize.height()))
        image = image.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

PhotoProvider::PhotoProvider() {
    m_pool.setMaxThreadCount(2);
    m_trim.setSingleShot(true);
    m_trim.setInterval(3000);
    QObject::connect(&m_trim, &QTimer::timeout, &m_trim, [this] {
        if (m_decoding == 0)
            m_pool.start([] { malloc_trim(0); });
    });
}

PhotoProvider::~PhotoProvider() {
    // Those not begun never will; those decoding finish first.
    m_pool.clear();
    m_pool.waitForDone();
}

QQuickImageResponse *PhotoProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    auto *response = new Response(this, QUrl::fromPercentEncoding(id.toUtf8()), requestedSize);
    ++m_decoding;
    m_pool.start(response);
    return response;
}

void PhotoProvider::decoded() {
    if (--m_decoding == 0)
        QMetaObject::invokeMethod(&m_trim, qOverload<>(&QTimer::start), Qt::QueuedConnection);
}

QUrl FileProvider::urlOf(const QString &path) {
    if (path.isEmpty())
        return {};
    return QUrl(QStringLiteral("image://file/") + QString::fromUtf8(QUrl::toPercentEncoding(path)));
}

QImage FileProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize,
                                  const QQuickImageProviderOptions &options) {
    const QString path = QUrl::fromPercentEncoding(id.toUtf8());
    // HEIC, which Qt does not read: as the photo provider decodes it.
    if (heifByName(path)) {
        QImage image = decodePhoto(path, requestedSize.width() > 0 && requestedSize.height() > 0 ? requestedSize : QSize());
        if (!image.isNull()) {
            if (size)
                *size = image.size();
            return image;
        }
    }
    // As Qt Quick reads a file (qquickpixmapcache.cpp, readImage).
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (options.autoTransform() != QQuickImageProviderOptions::UsePluginDefaultTransform)
        reader.setAutoTransform(options.autoTransform() == QQuickImageProviderOptions::ApplyTransform);
    const QSize original = reader.size();
    const QSize scaled = loadSize(original, requestedSize, reader.format(), options);
    if (scaled.isValid())
        reader.setScaledSize(scaled);
    if (!options.sourceClipRect().isNull())
        reader.setScaledClipRect(options.sourceClipRect().toRect());
    QImage image = reader.read();
    if (size)
        *size = original.isValid() ? original : image.size();
    toSrgb(image);
    return image;
}
