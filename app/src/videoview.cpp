#include "videoview.h"

#include "colour.h"
#include "sdrvideo.h"

#include <QQuickWindow>
#include <QRunnable>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QtMultimedia/private/qhwvideobuffer_p.h>
#include <QtMultimedia/private/qmultimediautils_p.h>
#include <QtMultimedia/private/qvideoframetexturepool_p.h>
#include <QtMultimedia/private/qvideotexturehelper_p.h>
#include <QtMultimediaQuick/private/qsgvideotexture_p.h>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace {

bool isHdr(const QVideoFrameFormat &format) {
    const auto transfer = format.colorTransfer();
    return transfer == QVideoFrameFormat::ColorTransfer_STD_B67 || transfer == QVideoFrameFormat::ColorTransfer_ST2084;
}

// How an HDR frame's planes are laid out, for its shader: 0 if it has
// none it reads (then the frame is made SDR on the CPU first).
struct Planes {
    int count = 0;
    int bits = 8;
    // A sample's value in the texture (0 to 1) times this is its code.
    float scale = 255;
};

Planes planesOf(QVideoFrameFormat::PixelFormat format) {
    switch (format) {
    case QVideoFrameFormat::Format_YUV420P:
        return {3, 8, 255};
    case QVideoFrameFormat::Format_NV12:
        return {2, 8, 255};
    case QVideoFrameFormat::Format_YUV420P10:
        return {3, 10, 65535};
    // Its ten bits the high ones of sixteen.
    case QVideoFrameFormat::Format_P010:
        return {2, 10, 65535.f / 64};
    case QVideoFrameFormat::Format_P016:
        return {2, 16, 65535};
    default:
        return {};
    }
}

// The shaders a frame is drawn with: Qt's own for an SDR one, Pinhole's for
// HDR. One material type each, as the scene graph keeps a pipeline a type.
struct ShaderKey {
    QString vertex, fragment;
    bool hdr = false;
    bool linear = false;
    bool operator==(const ShaderKey &o) const {
        return vertex == o.vertex && fragment == o.fragment && hdr == o.hdr && linear == o.linear;
    }
};

QSGMaterialType *typeFor(const ShaderKey &key) {
    static QMutex lock;
    static std::map<QString, std::unique_ptr<QSGMaterialType>> types;
    QMutexLocker locked(&lock);
    auto &type = types[key.vertex + QLatin1Char('|') + key.fragment];
    if (!type)
        type = std::make_unique<QSGMaterialType>();
    return type.get();
}

// Pinhole's HDR shader's uniform block (shaders/hdrvideo.frag), std140.
struct HdrUniforms {
    float matrix[16];
    float codes[4];   // code scale, Y' offset, Y' scale, C offset
    float ycc[4];     // C scale, Cr to R, Cb to G, Cr to G
    float ycc2[4];    // Cb to B, kr, kg, kb
    float gamut[12];  // the primaries' matrix to sRGB's, by rows
    float curve[4];   // HLG (1) or PQ (0), peak nits, target nits, headroom
    float knee[4];    // PQ of the peak, PQ of the target over it, the knee, above (1)
    float more[4];    // opacity, planes, linear (1) or sRGB's curve (0)
};
static_assert(sizeof(HdrUniforms) == 208);

// What an HDR frame is drawn with, but the matrix and opacity: from its
// format and the headroom.
HdrUniforms hdrUniformsFor(const QVideoFrameFormat &format, double headroom) {
    HdrUniforms u{};
    const Planes planes = planesOf(format.pixelFormat());
    const int bits = planes.bits;
    const float top = float((1 << bits) - 1);
    const float unit = float(1 << (bits - 8));
    const bool full = format.colorRange() == QVideoFrameFormat::ColorRange_Full;
    u.codes[0] = planes.scale;
    u.codes[1] = full ? 0 : 16 * unit;
    u.codes[2] = full ? 1 / top : 1 / (219 * unit);
    u.codes[3] = float(1 << (bits - 1));
    const auto space = format.colorSpace();
    const bool bt2020 = space == QVideoFrameFormat::ColorSpace_BT2020 || space == QVideoFrameFormat::ColorSpace_Undefined;
    const float kr = bt2020 ? 0.2627f : space == QVideoFrameFormat::ColorSpace_BT601 ? 0.299f : 0.2126f;
    const float kb = bt2020 ? 0.0593f : space == QVideoFrameFormat::ColorSpace_BT601 ? 0.114f : 0.0722f;
    const float kg = 1 - kr - kb;
    u.ycc[0] = full ? 1 / top : 1 / (224 * unit);
    u.ycc[1] = 2 * (1 - kr);
    u.ycc[2] = -2 * (1 - kb) * kb / kg;
    u.ycc[3] = -2 * (1 - kr) * kr / kg;
    u.ycc2[0] = 2 * (1 - kb);
    u.ycc2[1] = kr, u.ycc2[2] = kg, u.ycc2[3] = kb;
    static const float kFrom2020[12] = {1.6605f, -0.5876f, -0.0728f, 0, -0.1246f, 1.1329f, -0.0083f, 0, -0.0182f, -0.1006f, 1.1187f, 0};
    static const float kSame[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(u.gamut, bt2020 ? kFrom2020 : kSame, sizeof u.gamut);
    const bool hlg = format.colorTransfer() == QVideoFrameFormat::ColorTransfer_STD_B67;
    // PQ's peak: as the stream says, else 1000 nits (Qt says 10000 for
    // none, and its FFmpeg backend multiplied a mastering display's peak,
    // already in nits, by 10000): as toneMapFrame has it.
    double peak = kHlgPeakNits;
    if (!hlg) {
        peak = format.maxLuminance();
        if (peak > 10000)
            peak /= 10000;
        if (peak <= kSdrWhiteNits || peak >= 10000)
            peak = 1000;
    }
    headroom = std::max(1.0, headroom);
    const double target = kSdrWhiteNits * headroom;
    u.curve[0] = hlg ? 1 : 0;
    u.curve[1] = float(peak);
    u.curve[2] = float(target);
    u.curve[3] = float(headroom);
    const double topPq = nitsToPq(peak);
    const double maxLum = nitsToPq(target) / topPq;
    u.knee[0] = float(topPq);
    u.knee[1] = float(maxLum);
    u.knee[2] = float(1.5 * maxLum - 0.5);
    u.knee[3] = peak > target ? 1 : 0;
    u.more[1] = float(planes.count);
    return u;
}

class VideoMaterial : public QSGMaterial {
public:
    VideoMaterial(const ShaderKey &key, std::shared_ptr<QVideoFrameTexturePool> pool, bool alpha)
        : m_key(key), m_pool(std::move(pool)) {
        for (auto &texture : m_textures)
            texture = std::make_unique<QSGVideoTexture>();
        setFlag(Blending, alpha);
    }
    QSGMaterialType *type() const override { return typeFor(m_key); }
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *other) const override {
        return this == other ? 0 : (this < other ? -1 : 1);
    }

    // The current frame's planes as textures, once a frame.
    void updateTextures(QRhi *rhi, QRhiResourceUpdateBatch *batch) {
        if (!m_pool->texturesDirty())
            return;
        QVideoFrameTextures *textures = m_pool->updateTextures(*rhi, *batch);
        if (!textures)
            return;
        for (uint plane = 0; plane < 3; ++plane)
            m_textures[plane]->setRhiTexture(textures->texture(plane));
    }

    ShaderKey m_key;
    std::shared_ptr<QVideoFrameTexturePool> m_pool;
    std::unique_ptr<QSGVideoTexture> m_textures[3];
    HdrUniforms m_hdr{};
};

class VideoShader : public QSGMaterialShader {
public:
    explicit VideoShader(const ShaderKey &key) : m_hdr(key.hdr) {
        setShaderFileName(VertexStage, key.vertex);
        setShaderFileName(FragmentStage, key.fragment);
    }

    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override {
        auto *m = static_cast<VideoMaterial *>(newMaterial);
        // Here rather than per texture: done once, and before the
        // uniforms, which Qt's may take from the textures.
        m->updateTextures(state.rhi(), state.resourceUpdateBatch());
        if (!m_hdr) {
            const QVideoFrame &frame = m->m_pool->currentFrame();
            QVideoTextureHelper::updateUniformData(state.uniformData(), state.rhi(), frame.surfaceFormat(), frame,
                                                   state.combinedMatrix(), state.opacity());
            return true;
        }
        HdrUniforms u = m->m_hdr;
        std::memcpy(u.matrix, state.combinedMatrix().constData(), sizeof u.matrix);
        u.more[0] = state.opacity();
        QByteArray *buffer = state.uniformData();
        std::memcpy(buffer->data(), &u, std::min<size_t>(sizeof u, size_t(buffer->size())));
        return true;
    }

    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture, QSGMaterial *newMaterial,
                            QSGMaterial *) override {
        if (binding < 1 || binding > 3)
            return;
        auto *m = static_cast<VideoMaterial *>(newMaterial);
        m->updateTextures(state.rhi(), state.resourceUpdateBatch());
        QSGVideoTexture *t = m->m_textures[binding - 1].get();
        // NV12's two planes: the third binding of the HDR shader samples
        // the second again, unused.
        if (!t->rhiTexture() && binding == 3)
            t = m->m_textures[1].get();
        t->setFiltering(QSGTexture::Linear);
        *texture = t;
    }

private:
    bool m_hdr;
};

QSGMaterialShader *VideoMaterial::createShader(QSGRendererInterface::RenderMode) const { return new VideoShader(m_key); }

class VideoNode : public QSGGeometryNode {
public:
    VideoNode(const ShaderKey &key, std::shared_ptr<QVideoFrameTexturePool> pool, bool alpha)
        : m_key(key), m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4), m_material(key, std::move(pool), alpha) {
        m_geometry.setDrawingMode(QSGGeometry::DrawTriangleStrip);
        setGeometry(&m_geometry);
        setMaterial(&m_material);
    }
    ShaderKey m_key;
    QSGGeometry m_geometry;
    VideoMaterial m_material;
};

bool hasAlpha(QVideoFrameFormat::PixelFormat format) {
    switch (format) {
    case QVideoFrameFormat::Format_ARGB8888:
    case QVideoFrameFormat::Format_ARGB8888_Premultiplied:
    case QVideoFrameFormat::Format_BGRA8888:
    case QVideoFrameFormat::Format_BGRA8888_Premultiplied:
    case QVideoFrameFormat::Format_ABGR8888:
    case QVideoFrameFormat::Format_RGBA8888:
    case QVideoFrameFormat::Format_AYUV:
    case QVideoFrameFormat::Format_AYUV_Premultiplied:
        return true;
    default:
        return false;
    }
}

// The textures of a pool let go on the render thread, the window's
// graphics still there.
class Release : public QRunnable {
public:
    explicit Release(std::shared_ptr<QVideoFrameTexturePool> pool) : m_pool(std::move(pool)) {}
    void run() override { m_pool->clearTextures(); }

private:
    std::shared_ptr<QVideoFrameTexturePool> m_pool;
};

} // namespace

VideoView::VideoView(QQuickItem *parent) : QQuickItem(parent), m_pool(std::make_shared<QVideoFrameTexturePool>()) {
    setFlag(ItemHasContents);
    connect(&m_sink, qOverload<const QVideoFrame &>(&QVideoSink::videoFrameChanged), this, &VideoView::take, Qt::DirectConnection);
}

VideoView::~VideoView() {
    disconnect(m_following);
    disconnect(&m_sink, nullptr, this, nullptr);
    if (m_window)
        m_window->scheduleRenderJob(new Release(m_pool), QQuickWindow::NoStage);
}

void VideoView::setFillMode(FillMode mode) {
    if (mode == m_fillMode)
        return;
    m_fillMode = mode;
    updateContentRect();
    update();
    emit fillModeChanged();
}

void VideoView::setEndOfStreamPolicy(EndOfStreamPolicy policy) {
    if (policy == m_endOfStream.load())
        return;
    m_endOfStream = policy;
    emit endOfStreamPolicyChanged();
}

void VideoView::setHeadroom(qreal headroom) {
    headroom = std::max<qreal>(1, headroom);
    if (qFuzzyCompare(headroom, m_headroom))
        return;
    m_headroom = headroom;
    update();
    emit headroomChanged();
}

void VideoView::setLinear(bool linear) {
    if (linear == m_linear)
        return;
    m_linear = linear;
    update();
    emit linearChanged();
}

void VideoView::setFollows(VideoView *view) {
    if (view == m_follows)
        return;
    disconnect(m_following);
    m_follows = view;
    if (view) {
        // On the player's thread, as its own view takes them; the one it
        // has now at once, so a paused video shows too.
        m_following = connect(&view->m_sink, qOverload<const QVideoFrame &>(&QVideoSink::videoFrameChanged), &m_sink,
                              &QVideoSink::setVideoFrame, Qt::DirectConnection);
        m_sink.setVideoFrame(view->m_sink.videoFrame());
    } else {
        m_sink.setVideoFrame({});
    }
    emit followsChanged();
}

void VideoView::take(const QVideoFrame &frame) {
    QVideoFrame next = frame;
    if (!next.isValid() && m_endOfStream.load() == KeepLastFrame)
        return;
    const bool hdr = next.isValid() && isHdr(next.surfaceFormat());
    // An HDR frame the GPU cannot sample: made SDR here, on the CPU (the
    // buffers are this thread's alone).
    if (hdr && !m_gpuPlanes.load()) {
        next = toneMapFrame(frame, &m_cpuBuffers);
        if (!next.isValid())
            next = frame;
    }
    {
        QMutexLocker locked(&m_lock);
        m_frame = next;
        m_frameHdr = hdr;
        m_frameChanged = true;
    }
    QMetaObject::invokeMethod(this, &VideoView::arrived, Qt::QueuedConnection);
}

void VideoView::arrived() {
    QVideoFrame frame;
    bool hdr = false;
    {
        QMutexLocker locked(&m_lock);
        frame = m_frame;
        hdr = m_frameHdr;
    }
    const QRect viewport = frame.surfaceFormat().viewport();
    const QSize size = frame.isValid() ? (viewport.isEmpty() ? frame.size() : viewport.size()) : QSize();
    const int turn = frame.isValid() ? qToUnderlying(qNormalizedFrameTransformation(frame).rotation) : 0;
    if (size != m_frameSize || turn != m_frameTurn) {
        m_frameSize = size;
        m_frameTurn = turn;
        updateContentRect();
    }
    if (hdr != m_hdr) {
        m_hdr = hdr;
        emit hdrChanged();
    }
    if (const int drawn = m_drawn.load(); drawn != m_drawnShown) {
        m_drawnShown = drawn;
        emit framesDrawnChanged();
    }
    update();
}

void VideoView::updateContentRect() {
    QRectF rect;
    if (!m_frameSize.isEmpty()) {
        const QSizeF shown = m_frameTurn % 180 ? m_frameSize.transposed() : m_frameSize;
        const QRectF bounds = boundingRect();
        if (m_fillMode == Stretch) {
            rect = bounds;
        } else {
            const QSizeF scaled = shown.scaled(bounds.size(), Qt::AspectRatioMode(m_fillMode));
            rect = QRectF(QPointF(), scaled);
            rect.moveCenter(bounds.center());
        }
    }
    if (rect != m_contentRect) {
        m_contentRect = rect;
        emit contentRectChanged();
    }
}

void VideoView::geometryChange(const QRectF &now, const QRectF &before) {
    QQuickItem::geometryChange(now, before);
    updateContentRect();
    update();
}

void VideoView::itemChange(ItemChange change, const ItemChangeData &data) {
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange)
        followWindow(data.window);
}

void VideoView::followWindow(QQuickWindow *window) {
    if (window == m_window)
        return;
    if (m_window) {
        disconnect(m_window, nullptr, this, nullptr);
        m_window->scheduleRenderJob(new Release(m_pool), QQuickWindow::NoStage);
    }
    m_window = window;
    m_sink.setRhi(nullptr);
    if (!window)
        return;
    // The window's graphics: given them, the player hands over a hardware
    // frame to be drawn as it is, not a copy fetched from the decoder.
    auto graphics = [this, window] {
        QRhi *rhi = window->rhi();
        m_sink.setRhi(rhi);
        if (rhi)
            m_gpuPlanes = rhi->isTextureFormatSupported(QRhiTexture::R16) && rhi->isTextureFormatSupported(QRhiTexture::RG16)
                          && rhi->isTextureFormatSupported(QRhiTexture::R8) && rhi->isTextureFormatSupported(QRhiTexture::RG8);
    };
    connect(window, &QQuickWindow::sceneGraphInitialized, this, graphics, Qt::DirectConnection);
    connect(window, &QQuickWindow::sceneGraphInvalidated, this, [this] {
        m_pool->clearTextures();
        m_sink.setRhi(nullptr);
    }, Qt::DirectConnection);
    // Once a frame is out: what its textures held let go.
    connect(window, &QQuickWindow::afterFrameEnd, this, [pool = m_pool] { pool->onFrameEndInvoked(); }, Qt::DirectConnection);
    graphics();
}

void VideoView::releaseResources() {
    if (m_window)
        m_window->scheduleRenderJob(new Release(m_pool), QQuickWindow::NoStage);
    QQuickItem::releaseResources();
}

QSGNode *VideoView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = static_cast<VideoNode *>(old);
    QVideoFrame frame;
    bool changed = false;
    {
        QMutexLocker locked(&m_lock);
        frame = m_frame;
        changed = m_frameChanged;
        m_frameChanged = false;
    }
    QRhi *rhi = window() ? window()->rhi() : nullptr;
    if (!frame.isValid() || !rhi || m_contentRect.isEmpty()) {
        delete node;
        if (!frame.isValid())
            m_pool->setCurrentFrame({});
        return nullptr;
    }
    const QVideoFrameFormat format = frame.surfaceFormat();
    const bool hdr = isHdr(format) && planesOf(format.pixelFormat()).count > 0;
    ShaderKey key;
    key.hdr = hdr;
    key.linear = m_linear;
    if (hdr) {
        key.vertex = QStringLiteral(":/shaders/hdrvideo.vert.qsb");
        key.fragment = QStringLiteral(":/shaders/hdrvideo.frag.qsb");
    } else {
        key.vertex = QVideoTextureHelper::vertexShaderFileName(format);
        key.fragment = QVideoTextureHelper::fragmentShaderFileName(
            format, rhi, m_linear ? QRhiSwapChain::HDRExtendedSrgbLinear : QRhiSwapChain::SDR);
    }
    if (node && !(node->m_key == key)) {
        delete node;
        node = nullptr;
    }
    if (!node) {
        node = new VideoNode(key, m_pool, !hdr && hasAlpha(format.pixelFormat()));
        changed = true;
    }
    if (changed) {
        m_pool->setCurrentFrame(frame);
        ++m_drawn;
    }
    if (hdr) {
        node->m_material.m_hdr = hdrUniformsFor(format, m_headroom);
        node->m_material.m_hdr.more[2] = m_linear ? 1 : 0;
    }
    node->markDirty(QSGNode::DirtyMaterial);

    // Where each corner of the picture shown is in the frame: its
    // viewport, turned and mirrored as it says, cropped as the fill mode
    // has it.
    const QRectF bounds = boundingRect();
    QRectF target = m_contentRect;
    QRectF shown(0, 0, 1, 1); // of the picture as shown, 0 to 1
    if (m_fillMode == PreserveAspectCrop) {
        target = m_contentRect.intersected(bounds);
        shown = QRectF((target.left() - m_contentRect.left()) / m_contentRect.width(),
                       (target.top() - m_contentRect.top()) / m_contentRect.height(),
                       target.width() / m_contentRect.width(), target.height() / m_contentRect.height());
    }
    const VideoTransformation turn = qNormalizedFrameTransformation(frame);
    const QSizeF size = frame.size();
    const QRect viewport = format.viewport().isEmpty() ? QRect(QPoint(), frame.size()) : format.viewport();
    auto texturePoint = [&](double u, double v) {
        if (turn.mirroredHorizontallyAfterRotation)
            u = 1 - u;
        double s = u, t = v;
        switch (turn.rotation) {
        case QtVideo::Rotation::Clockwise90:
            s = v, t = 1 - u;
            break;
        case QtVideo::Rotation::Clockwise180:
            s = 1 - u, t = 1 - v;
            break;
        case QtVideo::Rotation::Clockwise270:
            s = 1 - v, t = u;
            break;
        default:
            break;
        }
        return QPointF((viewport.x() + s * viewport.width()) / size.width(), (viewport.y() + t * viewport.height()) / size.height());
    };
    QSGGeometry::TexturedPoint2D *v = node->m_geometry.vertexDataAsTexturedPoint2D();
    const QPointF tl = texturePoint(shown.left(), shown.top()), bl = texturePoint(shown.left(), shown.bottom());
    const QPointF tr = texturePoint(shown.right(), shown.top()), br = texturePoint(shown.right(), shown.bottom());
    v[0].set(float(target.left()), float(target.top()), float(tl.x()), float(tl.y()));
    v[1].set(float(target.left()), float(target.bottom()), float(bl.x()), float(bl.y()));
    v[2].set(float(target.right()), float(target.top()), float(tr.x()), float(tr.y()));
    v[3].set(float(target.right()), float(target.bottom()), float(br.x()), float(br.y()));
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
