#include "stage.h"

#include "display.h"
#include "photoprovider.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QColorSpace>
#include <QSettings>
#include <QSurfaceFormat>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtGui/rhi/qrhi.h>
#include <QtWaylandClient/private/qwaylandsubsurface_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kFullHdr = "display/fullHdr";

QString windowConf() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/window.conf";
}

// One decode at a time: the viewer's own decode of the same file runs
// beside it, on the photo provider's two threads.
QThreadPool *decoding() {
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(1);
        return p;
    }();
    return pool;
}

// photo.frag's block, std140.
struct PhotoUniforms {
    float matrix[16];
    float toSrgb[12]; // by rows
    float more[4];    // opacity, gamut, linear, mipmapped
    float size[4];    // the texture's, and its inverse
    // The gain map (H4): its kind (0 none, 1 Apple's, 2 ISO's), weight,
    // Apple's headroom (linear), multi-channel; ISO's per channel: the
    // gain's range (log2), 1 / gamma, the offsets; linear sRGB to the
    // photo's primaries, by rows.
    float gain[4];
    float gainMin[4];
    float gainMax[4];
    float gainGamma[4];
    float baseOffset[4];
    float alternateOffset[4];
    float fromSrgb[12];
    float linear[256][4];
};
static_assert(sizeof(PhotoUniforms) == 64 + 48 + 16 + 16 + 6 * 16 + 48 + 4096);

// A 3x3 matrix's inverse, by rows; the identity for one that has none.
std::array<double, 9> inverse(const float m[9]) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12)
        return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    return {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
            (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
            (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
}

class PhotoMaterial : public QSGMaterial {
public:
    PhotoMaterial() { setFlag(Blending, false); }
    ~PhotoMaterial() override {
        delete m_texture;
        delete m_gainTexture;
    }
    QSGMaterialType *type() const override {
        static QSGMaterialType type;
        return &type;
    }
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *other) const override { return this == other ? 0 : (this < other ? -1 : 1); }

    QSGTexture *m_texture = nullptr;
    // The gain map, or a texel of nothing where there is none.
    QSGTexture *m_gainTexture = nullptr;
    PhotoUniforms m_uniforms{};
};

class PhotoShader : public QSGMaterialShader {
public:
    PhotoShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/photo.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/photo.frag.qsb"));
    }
    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override {
        auto *m = static_cast<PhotoMaterial *>(newMaterial);
        PhotoUniforms &u = m->m_uniforms;
        std::memcpy(u.matrix, state.combinedMatrix().constData(), sizeof u.matrix);
        u.more[0] = state.opacity();
        QByteArray *buffer = state.uniformData();
        std::memcpy(buffer->data(), &u, std::min<size_t>(sizeof u, size_t(buffer->size())));
        return true;
    }
    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture, QSGMaterial *newMaterial,
                            QSGMaterial *) override {
        if (binding != 1 && binding != 2)
            return;
        auto *m = static_cast<PhotoMaterial *>(newMaterial);
        QSGTexture *t = binding == 1 ? m->m_texture : m->m_gainTexture;
        if (!t)
            return;
        t->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *texture = t;
    }
};

QSGMaterialShader *PhotoMaterial::createShader(QSGRendererInterface::RenderMode) const { return new PhotoShader; }

class PhotoNode : public QSGGeometryNode {
public:
    PhotoNode() : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4) {
        m_geometry.setDrawingMode(QSGGeometry::DrawTriangleStrip);
        setGeometry(&m_geometry);
        setMaterial(&m_material);
    }
    QSGGeometry m_geometry;
    PhotoMaterial m_material;
};

} // namespace

QString Stage::mode() {
    return qEnvironmentVariable("PINHOLE_STAGE");
}

Stage::Stage(ColourDisplay *display, QObject *parent) : QObject(parent) {
    const QString how = mode();
    m_forced = how == QLatin1String("force");
    m_wanted = m_forced || (how != QLatin1String("off") && display && (display->hdr() || display->wide()));
    m_usable = m_wanted;
    m_fullHdr = QSettings(windowConf(), QSettings::IniFormat).value(kFullHdr, true).toBool();
    if (m_wanted)
        std::fprintf(stderr, "pinhole: a stage for wide gamut and HDR (%s)\n",
                     m_forced ? "PINHOLE_STAGE=force" : "the display can show more than sRGB");
}

void Stage::setFullHdr(bool on) {
    if (on == m_fullHdr)
        return;
    m_fullHdr = on;
    QSettings settings(windowConf(), QSettings::IniFormat);
    settings.setValue(kFullHdr, on);
    settings.sync();
    emit fullHdrChanged();
}

void Stage::setUsable(bool usable) {
    if (usable == m_usable)
        return;
    m_usable = usable;
    emit usableChanged();
}

void Stage::make(QQmlEngine *engine, QQuickWindow *main) {
    if (!m_wanted || !engine || !main || m_window)
        return;
    // Whether the display's Vulkan surface offers scRGB, asked of the main
    // window's swapchain once it has one (on its render thread): the
    // stage's window is made for what it will get.
    auto asked = std::make_shared<QMetaObject::Connection>();
    *asked = connect(main, &QQuickWindow::frameSwapped, this, [this, engine, main, asked] {
        QRhiSwapChain *chain = main->swapChain();
        if (!chain)
            return;
        disconnect(*asked);
        const bool scrgb = chain->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear);
        QMetaObject::invokeMethod(this, [this, engine, main, scrgb] { create(engine, main, scrgb); }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    main->update();
}

void Stage::create(QQmlEngine *engine, QQuickWindow *main, bool scrgb) {
    if (m_window)
        return;
    if (!scrgb) {
        if (!m_forced) {
            std::fprintf(stderr, "pinhole: the display offers no scRGB swapchain (fp16, extended linear sRGB): no stage\n");
            setUsable(false);
            return;
        }
        std::fprintf(stderr, "pinhole: the display offers no scRGB swapchain: the stage forced on, in SDR\n");
    }
    QQmlComponent component(engine, QUrl(QStringLiteral("qrc:/Stage.qml")));
    QObject *made = component.createWithInitialProperties({{QStringLiteral("mainWindow"), QVariant::fromValue(main)}});
    auto *window = qobject_cast<QQuickWindow *>(made);
    if (!window) {
        std::fprintf(stderr, "pinhole: the stage did not load: %s\n", qPrintable(component.errorString()));
        delete made;
        setUsable(false);
        return;
    }
    // A child of the main window, its subsurface; deleted with it.
    window->setParent(main);
    if (scrgb) {
        // Read by Qt Quick once, as the swapchain is made
        // (qsgrhisupport.cpp).
        window->setProperty("_qt_sg_hdr_format", QStringLiteral("scrgb"));
        // And its surface said to be linear sRGB, values beyond 0 to 1
        // kept: Mesa (26.3) makes the swapchain but does not tag the
        // surface, which the compositor would then take as sRGB's curve.
        // Qt tags it (wp_color_management_surface_v1) from the format.
        QSurfaceFormat format = window->requestedFormat();
        format.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
        window->setFormat(format);
    }
    // What it got, once it has drawn: on its render thread, then here.
    connect(window, &QQuickWindow::frameSwapped, this, [this, window] {
        if (m_checked)
            return;
        QRhiSwapChain *swapChain = window->swapChain();
        if (!swapChain)
            return;
        const bool got = swapChain->format() == QRhiSwapChain::HDRExtendedSrgbLinear;
        QMetaObject::invokeMethod(this, [this, got] {
            if (m_checked)
                return;
            m_checked = true;
            if (got != m_scrgb) {
                m_scrgb = got;
                emit scrgbChanged();
            }
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    m_window = window;
    emit windowChanged();
}

bool Stage::placeBelow(QQuickWindow *window) {
    if (placeSubsurfaceBelowParent(window))
        return true;
    if (m_usable)
        std::fprintf(stderr, "pinhole: the stage cannot be put under the window (Qt %s, built with %s): no stage\n", qVersion(),
                     QT_VERSION_STR);
    setUsable(false);
    return false;
}

bool placeSubsurfaceBelowParent(QWindow *child) {
    // Qt Wayland's private classes are those of the Qt built with only.
    if (std::strcmp(qVersion(), QT_VERSION_STR) != 0 || !child || !child->parent())
        return false;
    auto *window = dynamic_cast<QtWaylandClient::QWaylandWindow *>(child->handle());
    auto *parent = dynamic_cast<QtWaylandClient::QWaylandWindow *>(child->parent()->handle());
    if (!window || !parent || !window->subSurfaceWindow() || !parent->wlSurface())
        return false;
    QtWaylandClient::QWaylandSubSurface *subsurface = window->subSurfaceWindow();
    // Its own frames as they come, not held for the main window's: the
    // stage fades and plays video while the main window draws nothing new.
    subsurface->setDeSync();
    subsurface->place_below(parent->wlSurface());
    return true;
}

StagePhoto::StagePhoto(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
}

StagePhoto::~StagePhoto() {
    if (m_cancel)
        *m_cancel = true;
}

void StagePhoto::setSource(const QUrl &source) {
    if (source == m_source)
        return;
    m_source = source;
    load();
    emit sourceChanged();
}

void StagePhoto::setSourceSize(const QSize &size) {
    if (size == m_sourceSize)
        return;
    m_sourceSize = size;
    load();
    emit sourceChanged();
}

void StagePhoto::setMipmap(bool on) {
    if (on == m_mipmap)
        return;
    m_mipmap = on;
    emit mipmapChanged();
}

void StagePhoto::setGamut(qreal gamut) {
    if (qFuzzyCompare(gamut, m_gamut))
        return;
    m_gamut = gamut;
    update();
    emit gamutChanged();
}

void StagePhoto::setLinear(bool linear) {
    if (linear == m_linear)
        return;
    m_linear = linear;
    // An scRGB stage draws the gain map, decoded with the photo.
    if (linear && !m_source.isEmpty())
        load();
    update();
    emit linearChanged();
}

void StagePhoto::setHeadroom(qreal stops) {
    if (qFuzzyCompare(stops, m_headroom))
        return;
    m_headroom = stops;
    update();
    emit weightChanged();
}

void StagePhoto::setHdrWeight(qreal weight) {
    if (qFuzzyCompare(weight, m_hdrWeight))
        return;
    m_hdrWeight = weight;
    update();
    emit weightChanged();
}

void StagePhoto::setGainMapOn(bool on) {
    if (on == m_gainMapOn)
        return;
    m_gainMapOn = on;
    update();
    emit weightChanged();
}

qreal StagePhoto::gainMapWeight() const {
    if (!m_gainMapOn || m_gainMap.kind.isEmpty() || !m_linear)
        return 0;
    // The fade multiplies the weight (docs/hdr.md, the maths).
    return m_hdrWeight * m_gainMap.weight(m_headroom);
}

QString StagePhoto::pathOf(const QUrl &url, Adjustment *adjust) {
    const QString s = url.toString(QUrl::FullyEncoded);
    for (const QLatin1String prefix : {QLatin1String("image://photo/"), QLatin1String("image://file/")})
        if (s.startsWith(prefix)) {
            QString path;
            const Adjustment a = Adjustment::ofUrl(QUrl::fromPercentEncoding(s.mid(prefix.size()).toUtf8()), &path);
            if (adjust)
                *adjust = a;
            return path;
        }
    return url.isLocalFile() ? url.toLocalFile() : QString();
}

QImage StagePhoto::decode(const QString &path, QSize size, StageColour &colour, const std::atomic<bool> *cancelled,
                          GainMap *map, const Adjustment &adjust) {
    QByteArray profile;
    // Turned and cut as the person did, its gain map alike.
    QImage image = adjust.applied(decodePhoto(path, size.isValid() && !size.isEmpty() ? size : QSize(), cancelled, &profile));
    if (image.isNull())
        return {};
    // A profile of tables (rare): made sRGB here, as the window's is.
    if (!stageColourOf(profile, colour))
        toSrgb(image, profile);
    // Its gain map: a quarter to a half of its size, decoded after it.
    if (map && !(cancelled && *cancelled)) {
        *map = readGainMap(path, cancelled);
        map->image = adjust.applied(map->image);
    }
    return image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBX8888);
}

void StagePhoto::load() {
    if (m_cancel)
        *m_cancel = true;
    m_cancel.reset();
    const quint64 serial = ++m_serial;
    Adjustment adjust;
    const QString path = pathOf(m_source, &adjust);
    if (path.isEmpty()) {
        // Nothing to show: what it held let go.
        const bool had = !m_shown.isEmpty() || m_colour;
        m_shown = QUrl();
        m_shownSize = QSize();
        m_imageSize = QSize();
        m_image = QImage();
        m_imageChanged = false;
        m_colour.reset();
        m_gainMap = GainMap();
        update();
        if (had)
            emit shownChanged();
        return;
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_cancel = cancel;
    QPointer<StagePhoto> self(this);
    const QUrl source = m_source;
    const QSize size = m_sourceSize;
    const bool withMap = m_linear;
    decoding()->start([self, serial, path, source, size, cancel, withMap, adjust] {
        if (*cancel)
            return;
        auto colour = std::make_shared<StageColour>();
        auto map = std::make_shared<GainMap>();
        const QImage image = decode(path, size, *colour, cancel.get(), withMap ? map.get() : nullptr, adjust);
        if (*cancel || image.isNull())
            return;
        QMetaObject::invokeMethod(qApp, [self, serial, image, colour, map, source, size] {
            if (self)
                self->decoded(serial, image, *colour, *map, source, size);
        }, Qt::QueuedConnection);
    });
}

void StagePhoto::decoded(quint64 serial, const QImage &image, const StageColour &colour, const GainMap &map,
                         const QUrl &source, QSize size) {
    if (serial != m_serial)
        return;
    m_image = image;
    m_imageChanged = true;
    m_colour = std::make_unique<StageColour>(colour);
    m_imageSize = image.size();
    m_gainMap = map;
    m_gainMulti = map.multichannel();
    emit weightChanged();
    m_shown = source;
    m_shownSize = size;
    update();
    emit shownChanged();
}

QSGNode *StagePhoto::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = static_cast<PhotoNode *>(old);
    if (!m_colour || width() <= 0 || height() <= 0) {
        delete node;
        return nullptr;
    }
    if (!node)
        node = new PhotoNode;
    PhotoMaterial &material = node->m_material;
    if (m_imageChanged || !material.m_texture) {
        if (m_image.isNull()) {
            // Its texture gone with the window's graphics, and its pixels
            // let go once uploaded: decoded again.
            delete node;
            QMetaObject::invokeMethod(this, &StagePhoto::load, Qt::QueuedConnection);
            return nullptr;
        }
        delete material.m_texture;
        // Its own texture, not in the atlas, as the viewer's full size.
        material.m_texture = window()->createTextureFromImage(
            m_image, m_mipmap ? QQuickWindow::TextureHasMipmaps : QQuickWindow::CreateTextureOptions());
        material.m_texture->setFiltering(QSGTexture::Linear);
        material.m_texture->setMipmapFiltering(m_mipmap ? QSGTexture::Linear : QSGTexture::None);
        material.setFlag(QSGMaterial::Blending, m_image.hasAlphaChannel());
        // The gain map's codes, filtered by the texture (the upscale), or
        // a texel of nothing.
        delete material.m_gainTexture;
        QImage gain = m_gainMap.image.convertToFormat(QImage::Format_RGBX8888);
        const bool mapped = !gain.isNull();
        if (!mapped) {
            gain = QImage(1, 1, QImage::Format_RGBX8888);
            gain.fill(Qt::black);
        }
        material.m_gainTexture = window()->createTextureFromImage(
            gain, m_mipmap && mapped ? QQuickWindow::TextureHasMipmaps : QQuickWindow::CreateTextureOptions());
        material.m_gainTexture->setFiltering(QSGTexture::Linear);
        material.m_gainTexture->setMipmapFiltering(m_mipmap && mapped ? QSGTexture::Linear : QSGTexture::None);
        m_gainMap.image = QImage();
        m_image = QImage();
        m_imageChanged = false;
        node->markDirty(QSGNode::DirtyMaterial);
    }
    PhotoUniforms &u = material.m_uniforms;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            u.toSrgb[row * 4 + col] = m_colour->matrix[row * 3 + col];
        u.toSrgb[row * 4 + 3] = 0;
    }
    u.more[1] = float(m_gamut);
    u.more[2] = m_linear ? 1 : 0;
    u.more[3] = m_mipmap ? 1 : 0;
    for (int v = 0; v < 256; ++v) {
        for (int k = 0; k < 3; ++k)
            u.linear[v][k] = m_colour->linear[k][v];
        u.linear[v][3] = 0;
    }
    // The gain map at its weight now (docs/hdr.md, the maths).
    const GainMap &g = m_gainMap;
    const bool apple = g.kind == QLatin1String("apple");
    u.gain[0] = g.kind.isEmpty() ? 0 : apple ? 1 : 2;
    u.gain[1] = float(gainMapWeight());
    u.gain[2] = float(g.appleHeadroom);
    u.gain[3] = m_gainMulti ? 1 : 0;
    for (int c = 0; c < 3; ++c) {
        u.gainMin[c] = float(g.gainMin[c]);
        u.gainMax[c] = float(g.gainMax[c]);
        u.gainGamma[c] = float(1 / g.gamma[c]);
        u.baseOffset[c] = apple ? 0 : float(g.baseOffset[c]);
        u.alternateOffset[c] = apple ? 0 : float(g.alternateOffset[c]);
    }
    // A multi-channel map applies in the photo's own primaries: linear
    // sRGB taken back to them.
    const std::array<double, 9> fromSrgb = inverse(m_colour->matrix);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            u.fromSrgb[row * 4 + col] = float(fromSrgb[size_t(row * 3 + col)]);
        u.fromSrgb[row * 4 + 3] = 0;
    }
    node->markDirty(QSGNode::DirtyMaterial);

    // Filling the item, cropped as PreserveAspectCrop has it.
    const QSizeF image = material.m_texture->textureSize();
    u.size[0] = float(image.width());
    u.size[1] = float(image.height());
    u.size[2] = image.width() > 0 ? float(1 / image.width()) : 0;
    u.size[3] = image.height() > 0 ? float(1 / image.height()) : 0;
    const QRectF bounds = boundingRect();
    QRectF shown(0, 0, 1, 1);
    if (!image.isEmpty()) {
        const qreal scale = std::max(bounds.width() / image.width(), bounds.height() / image.height());
        const qreal w = bounds.width() / scale / image.width(), h = bounds.height() / scale / image.height();
        shown = QRectF((1 - w) / 2, (1 - h) / 2, w, h);
    }
    QSGGeometry::TexturedPoint2D *v = node->m_geometry.vertexDataAsTexturedPoint2D();
    v[0].set(float(bounds.left()), float(bounds.top()), float(shown.left()), float(shown.top()));
    v[1].set(float(bounds.left()), float(bounds.bottom()), float(shown.left()), float(shown.bottom()));
    v[2].set(float(bounds.right()), float(bounds.top()), float(shown.right()), float(shown.top()));
    v[3].set(float(bounds.right()), float(bounds.bottom()), float(shown.right()), float(shown.bottom()));
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}

void StagePhoto::releaseResources() {
    QQuickItem::releaseResources();
}
