// PINHOLE_SELFTEST=stage:<dir> (selftest.h): the stage (stage.h, docs/hdr.md
// H3) as Pinhole hands it to the compositor, read back.

#include "selftest.h"

#include "display.h"
#include "gainmap.h"
#include "library.h"
#include "photoprovider.h"
#include "stage.h"
#include "videoview.h"
#include "viewer.h"

#include <QAudioOutput>
#include <QColorSpace>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMatrix4x4>
#include <QVector3D>
#include <QMediaPlayer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QtGui/rhi/qrhi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

QObject *pluginItem(QQuickWindow *window, const QString &id) {
    QVariant item;
    QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, item), Q_ARG(QVariant, id));
    return item.value<QObject *>();
}

QRect inScene(QQuickItem *item, qreal dpr) {
    if (!item)
        return {};
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toAlignedRect();
}

// A pixel of a float image as it is: linear light, scRGB.
struct Rgb {
    float r, g, b, a;
};
Rgb at(const QImage &image, int x, int y) {
    if (image.format() == QImage::Format_RGBA16FPx4 || image.format() == QImage::Format_RGBX16FPx4
        || image.format() == QImage::Format_RGBA16FPx4_Premultiplied) {
        const qfloat16 *p = reinterpret_cast<const qfloat16 *>(image.constScanLine(y)) + 4 * x;
        return {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
    }
    if (image.format() == QImage::Format_RGBA32FPx4 || image.format() == QImage::Format_RGBA32FPx4_Premultiplied) {
        const float *p = reinterpret_cast<const float *>(image.constScanLine(y)) + 4 * x;
        return {p[0], p[1], p[2], p[3]};
    }
    const QColor c = image.pixelColor(x, y);
    return {float(c.redF()), float(c.greenF()), float(c.blueF()), float(c.alphaF())};
}

double encode(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return 255 * (v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055);
}

// A float image saved as it is: PFM, RGB, bottom row first.
void savePfm(const QImage &image, const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QStringLiteral("PF\n%1 %2\n-1.0\n").arg(image.width()).arg(image.height()).toLatin1());
    for (int y = image.height() - 1; y >= 0; --y)
        for (int x = 0; x < image.width(); ++x) {
            const Rgb p = at(image, x, y);
            const float v[3] = {p.r, p.g, p.b};
            f.write(reinterpret_cast<const char *>(v), sizeof v);
        }
}

// Over `rect` (shrunk by 3 px, past filtered edges): the stage's pixels,
// clipped and in sRGB's curve, against an 8-bit grab of the main window.
struct Difference {
    double mean = 0, worst = 0;
    double within1 = 0; // the share within one code
    int pixels = 0;
};
// Only where `clear` (the main window over the stage) is transparent, if
// given: where the stage is what shows.
Difference compare(const QImage &stage, const QImage &main, QRect rect, bool linear, const QImage &clear = QImage(),
                   const QString &differences = QString()) {
    rect = rect.adjusted(3, 3, -3, -3).intersected(stage.rect()).intersected(main.rect());
    Difference d;
    QImage marks;
    if (!differences.isEmpty()) {
        marks = main.convertToFormat(QImage::Format_RGB32);
        marks.fill(Qt::black);
    }
    long within = 0;
    for (int y = rect.top(); y <= rect.bottom(); y += 2)
        for (int x = rect.left(); x <= rect.right(); x += 2) {
            if (!clear.isNull() && qAlpha(clear.pixel(x, y)) != 0)
                continue;
            const Rgb s = at(stage, x, y);
            const QColor m = main.pixelColor(x, y);
            const double e[3] = {linear ? encode(s.r) : 255 * s.r, linear ? encode(s.g) : 255 * s.g, linear ? encode(s.b) : 255 * s.b};
            const double c[3] = {m.redF() * 255, m.greenF() * 255, m.blueF() * 255};
            double most = 0;
            for (int k = 0; k < 3; ++k) {
                const double diff = std::abs(std::round(e[k]) - std::round(c[k]));
                d.mean += diff;
                most = std::max(most, diff);
            }
            d.worst = std::max(d.worst, most);
            within += most <= 1;
            if (!marks.isNull() && most > 1)
                marks.setPixel(x, y, most > 4 ? qRgb(255, 0, 0) : qRgb(255, 255, 0));
            ++d.pixels;
        }
    if (!marks.isNull())
        marks.save(differences);
    if (d.pixels) {
        d.mean /= 3.0 * d.pixels;
        d.within1 = double(within) / d.pixels;
    }
    return d;
}

// The share of pixels with a channel outside 0 to 1, and the brightest.
struct Range {
    double outside = 0, below = 0, most = 0;
};
Range rangeOf(const QImage &image, QRect rect) {
    rect = rect.adjusted(3, 3, -3, -3).intersected(image.rect());
    Range r;
    long out = 0, below = 0, n = 0;
    for (int y = rect.top(); y <= rect.bottom(); y += 2)
        for (int x = rect.left(); x <= rect.right(); x += 2) {
            const Rgb p = at(image, x, y);
            const float lo = std::min({p.r, p.g, p.b}), hi = std::max({p.r, p.g, p.b});
            out += lo < -1e-3f || hi > 1.001f;
            below += lo < -1e-3f;
            r.most = std::max(r.most, double(hi));
            ++n;
        }
    if (n) {
        r.outside = double(out) / n;
        r.below = double(below) / n;
    }
    return r;
}

// The window's next frame as it goes to the compositor: its swapchain's
// image read back once drawn, in the swapchain's format (RGBA16F for
// scRGB). grabWindow() is no use here: Qt 6.12 reads an fp16 swapchain
// back as 8-bit codes.
void readBack(QQuickWindow *window, std::function<void(const QImage &)> done) {
    // The next frame whose state is synchronised from now on: armed on its
    // synchronisation, read once rendered.
    auto armed = std::make_shared<std::atomic<int>>(0);
    auto synced = std::make_shared<QMetaObject::Connection>();
    auto rendered = std::make_shared<QMetaObject::Connection>();
    *synced = QObject::connect(window, &QQuickWindow::afterSynchronizing, window, [armed, synced] {
        QObject::disconnect(*synced);
        *armed = 1;
    }, Qt::DirectConnection);
    *rendered = QObject::connect(window, &QQuickWindow::afterRendering, window, [window, armed, rendered, done] {
        if (*armed != 1)
            return;
        *armed = 2;
        QObject::disconnect(*rendered);
        QRhi *rhi = window->rhi();
        QRhiSwapChain *chain = window->swapChain();
        if (!rhi || !chain) {
            QMetaObject::invokeMethod(qApp, [done] { done({}); }, Qt::QueuedConnection);
            return;
        }
        auto *result = new QRhiReadbackResult;
        result->completed = [result, done, armed] {
            QImage::Format format = QImage::Format_Invalid;
            switch (result->format) {
            case QRhiTexture::RGBA16F: format = QImage::Format_RGBA16FPx4_Premultiplied; break;
            case QRhiTexture::RGBA32F: format = QImage::Format_RGBA32FPx4_Premultiplied; break;
            case QRhiTexture::RGBA8: format = QImage::Format_RGBA8888_Premultiplied; break;
            case QRhiTexture::BGRA8: format = QImage::Format_ARGB32_Premultiplied; break;
            default: break;
            }
            QImage image;
            if (format != QImage::Format_Invalid)
                image = QImage(reinterpret_cast<const uchar *>(result->data.constData()), result->pixelSize.width(),
                               result->pixelSize.height(), format).copy();
            delete result;
            *armed = 3;
            QMetaObject::invokeMethod(qApp, [done, image] { done(image); }, Qt::QueuedConnection);
        };
        QRhiResourceUpdateBatch *batch = rhi->nextResourceUpdateBatch();
        batch->readBackTexture(QRhiReadbackDescription(), result);
        chain->currentFrameCommandBuffer()->resourceUpdate(batch);
        // Done once a later frame begins: one asked for.
        QMetaObject::invokeMethod(window, "update", Qt::QueuedConnection);
    }, Qt::DirectConnection);
    // Asked for until it comes (a window the compositor has not given a
    // frame callback lately draws nothing on update() alone).
    auto *ask = new QTimer(window);
    ask->setInterval(50);
    auto tries = std::make_shared<int>(0);
    QObject::connect(ask, &QTimer::timeout, window, [window, armed, ask, tries] {
        if (*armed == 3) {
            ask->deleteLater();
            return;
        }
        if (++*tries % 10 == 0)
            std::printf("selftest: (waiting for the stage's frame %d: exposed %d, visible %d)\n", armed->load(), window->isExposed(),
                        window->isVisible());
        window->update();
        if (window->contentItem())
            window->contentItem()->update();
    });
    ask->start();
    window->update();
}

QString formatName(const QImage &image) {
    switch (image.format()) {
    case QImage::Format_RGBA16FPx4: return QStringLiteral("RGBA16F");
    case QImage::Format_RGBA16FPx4_Premultiplied: return QStringLiteral("RGBA16F premultiplied");
    case QImage::Format_RGBX16FPx4: return QStringLiteral("RGBX16F");
    default: return QStringLiteral("8-bit (QImage format %1)").arg(int(image.format()));
    }
}

} // namespace

void runStageSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &dir) {
    auto failures = std::make_shared<int>(0);
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto check = [say, failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    // Waits for `ready`, polling, then `then`; at `ms`, says it timed out and
    // goes on.
    auto waitFor = [window, check](std::function<bool()> ready, int ms, const QString &what, std::function<void()> then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto *poll = new QTimer(window);
        poll->setInterval(5);
        QObject::connect(poll, &QTimer::timeout, window, [=] {
            if (ready()) {
                poll->deleteLater();
                then();
            } else if (clock->elapsed() > ms) {
                poll->deleteLater();
                check(false, what + QStringLiteral(" (timed out)"));
                then();
            }
        });
        poll->start();
    };
    auto finish = [failures, say] {
        say(QStringLiteral("done, %1 failed").arg(*failures));
        QCoreApplication::exit(*failures ? 1 : 0);
    };
    QObject *stage = qmlEngine(window)->rootContext()->contextProperty("stage").value<QObject *>();
    auto *display = qmlEngine(window)->rootContext()->contextProperty("display").value<ColourDisplay *>();
    auto stageWindow = [window] { return qobject_cast<QQuickWindow *>(window->property("stageWindow").value<QObject *>()); };
    auto photoViewer = [window] { return window->findChild<QQuickItem *>(QStringLiteral("photoViewer")); };
    const qreal dpr = window->devicePixelRatio();

    // PINHOLE_SELFTEST_PHOTOS: the photo, the Live Photo and the video, by
    // ids; else the newest whose medium (and motion, and video) is here.
    const QStringList ids = qEnvironmentVariable("PINHOLE_SELFTEST_PHOTOS").split(',', Qt::SkipEmptyParts);
    auto byId = [&library](const QString &id) {
        for (int i = 0; i < library.count(); ++i)
            if (library.at(i).id == id.toUtf8())
                return i;
        return -1;
    };
    // PINHOLE_SELFTEST_VIDEOS: an HLG or PQ clip, played in the stage.
    const QString hdrClip = qEnvironmentVariable("PINHOLE_SELFTEST_VIDEOS").section(':', 0, 0);

    struct State {
        int photo = -1, live = -1, video = -1;
        QImage mainSdr;
        QRect frame;
        std::function<void()> opened, handedOver, faded, green, zoomed, held, liveMotion, playVideo, hdrVideo, leaving;
    };
    auto s = std::make_shared<State>();
    auto tag = [](QObject *o) { return o ? o->property("handedOver").toBool() : false; };
    // Another photo full window: the viewer closed, then opened on it.
    auto reopen = [=, &viewer](int index) {
        viewer.close();
        waitFor([=] { return !photoViewer()->property("shown").toBool(); }, 5000, "the viewer closes", [=, &viewer] {
            QMetaObject::invokeMethod(&viewer, "openAt", Q_ARG(int, index), Q_ARG(QRectF, QRectF(100, 100, 100, 100)));
        });
    };

    auto start = [=, &library, &viewer] {
        s->photo = ids.size() > 0 ? byId(ids[0]) : -1;
        s->live = ids.size() > 1 ? byId(ids[1]) : -1;
        s->video = ids.size() > 2 ? byId(ids[2]) : -1;
        if (s->photo < 0) {
            const QList<int> here = findHere(library, QStringLiteral("medium"), QStringLiteral("NOT a.is_video"), 1);
            s->photo = here.isEmpty() ? library.count() - 1 : here[0];
        }
        say(QStringLiteral("display: headroom %1 stops, %2; stage wanted %3, usable %4; PINHOLE_STAGE=%5")
                .arg(display->headroom(), 0, 'f', 2)
                .arg(display->wide() ? "wide" : "sRGB's gamut")
                .arg(stage->property("wanted").toBool())
                .arg(stage->property("usable").toBool())
                .arg(Stage::mode()));
        check(stage->property("wanted").toBool() && stageWindow(), "the stage is made, its window under the main one's");
        if (!stageWindow())
            return finish();
        say(QStringLiteral("the photo: %1 (%2)").arg(QString::fromUtf8(library.at(s->photo).id)).arg(s->photo));
        // Held back until the viewer has its sharpest and is still, so that
        // the main window's SDR photo is grabbed as it is at the hand-over.
        stageWindow()->setProperty("paused", true);
        QMetaObject::invokeMethod(&viewer, "openAt", Q_ARG(int, s->photo), Q_ARG(QRectF, QRectF(100, 100, 100, 100)));
        waitFor([=, &viewer] { return photoViewer()->property("progress").toDouble() == 1 && !viewer.loading(); }, 15000,
                "the viewer has the photo at its sharpest", [=] { QTimer::singleShot(500, window, s->opened); });
    };

    auto handover = std::make_shared<QImage>();
    auto handoverFade = std::make_shared<double>(-1);
    s->opened = [=] {
        s->mainSdr = window->grabWindow();
        s->mainSdr.save(dir + "/main-sdr.png");
        s->frame = inScene(photoViewer()->property("frameItem").value<QQuickItem *>(), dpr);
        // Its first frame with the photo, the one the main window turns
        // transparent over: read as soon as it is placed.
        auto context = new QObject(window);
        onChange(stageWindow(), "placed", context, [=] {
            if (!stageWindow()->property("placed").toBool() || *handoverFade >= 0)
                return;
            *handoverFade = stageWindow()->property("fade").toDouble();
            context->deleteLater();
            readBack(stageWindow(), [=](const QImage &image) {
                *handover = image;
                waitFor([=] { return tag(stageWindow()); }, 5000, "the stage takes over", s->handedOver);
            });
        });
        stageWindow()->setProperty("paused", false);
    };

    s->handedOver = [=] {
        QQuickWindow *sw = stageWindow();
        const double fade = *handoverFade;
        const QImage grabbed = *handover;
        savePfm(grabbed, dir + "/stage-handover.pfm");
        QRhiSwapChain *chain = sw->swapChain();
        const bool scrgb = stage->property("scrgb").toBool();
        say(QStringLiteral("the stage: swapchain %1, read back as %2; a subsurface of the main window: %3; fade %4")
                .arg(chain && chain->format() == QRhiSwapChain::HDRExtendedSrgbLinear ? "scRGB (fp16, extended linear sRGB)" : "SDR")
                .arg(formatName(grabbed))
                .arg(sw->parent() == window ? "yes" : "no")
                .arg(fade, 0, 'f', 3));
        check(sw->isVisible() && sw->parent() == window, "the stage's window is shown, a child of the main window");
        check(stage->property("usable").toBool(), "placed under the main window (place_below)");
        // The main window over it, as the compositor has them: where it is
        // transparent, the stage shows instead of the SDR photo it drew.
        const QImage main = window->grabWindow();
        main.save(dir + "/main-staged.png");
        const Difference d = compare(grabbed, s->mainSdr, s->frame, scrgb, main, dir + "/handover-differences.png");
        say(QStringLiteral("the hand-over: the stage's first frame (fade %1) where the main window went transparent, against the "
                           "SDR photo it drew there, %2 pixels: mean %3, worst %4 codes of 255, %5 % within one")
                .arg(fade, 0, 'f', 3).arg(d.pixels).arg(d.mean, 0, 'f', 3).arg(d.worst).arg(100 * d.within1, 0, 'f', 2));
        check(fade == 0 && d.pixels > 1000 && d.worst <= 1,
              "the stage takes over at SDR: the main window's pixels");
        // The backdrop, beside the photo: the theme's, in linear light.
        const QColor theme = qmlEngine(window)->rootContext()->contextProperty("theme").value<QObject *>()->property("darkerBackground").value<QColor>();
        const Rgb back = at(grabbed, 2, grabbed.height() / 2);
        auto lin = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        const double want[3] = {scrgb ? lin(theme.redF()) : theme.redF(), scrgb ? lin(theme.greenF()) : theme.greenF(),
                                scrgb ? lin(theme.blueF()) : theme.blueF()};
        say(QStringLiteral("the backdrop: (%1, %2, %3), the theme's %4 (%5, %6, %7)")
                .arg(back.r, 0, 'f', 5).arg(back.g, 0, 'f', 5).arg(back.b, 0, 'f', 5)
                .arg(scrgb ? "in linear light" : "as it is").arg(want[0], 0, 'f', 5).arg(want[1], 0, 'f', 5).arg(want[2], 0, 'f', 5));
        check(std::abs(back.r - want[0]) < 2e-3 && std::abs(back.g - want[1]) < 2e-3 && std::abs(back.b - want[2]) < 2e-3,
              "the backdrop is the theme's");
        // The main window: transparent where the stage shows, the date over it.
        const QPoint centre = s->frame.center();
        const QQuickItem *date = photoViewer()->findChild<QQuickItem *>(QStringLiteral("loadingSpinner"));
        Q_UNUSED(date);
        say(QStringLiteral("the main window over the stage: alpha %1 at the photo's middle, %2 at the backdrop's edge, %3 at the top left "
                           "(the date)")
                .arg(qAlpha(main.pixel(centre))).arg(qAlpha(main.pixel(2, main.height() / 2))).arg(qAlpha(main.pixel(int(24 * dpr), int(26 * dpr)))));
        check(qAlpha(main.pixel(centre)) == 0 && qAlpha(main.pixel(2, main.height() / 2)) == 0,
              "the main window is transparent where the stage shows");
        check(qAlpha(main.pixel(int(24 * dpr), int(26 * dpr))) > 200, "the main window still draws its chrome over it");
        waitFor([=] { return stageWindow()->property("fade").toDouble() >= 1; }, 2000, "the fade ends", s->faded);
    };

    s->faded = [=] { readBack(stageWindow(), [=](const QImage &grabbed) {
        QQuickWindow *sw = stageWindow();
        savePfm(grabbed, dir + "/stage-faded.pfm");
        const Range r = rangeOf(grabbed, s->frame);
        say(QStringLiteral("faded in: %1 % of the photo's pixels beyond sRGB (%2 % below 0), the brightest %3; gamut %4, HDR %5")
                .arg(100 * r.outside, 0, 'f', 2).arg(100 * r.below, 0, 'f', 2).arg(r.most, 0, 'f', 4)
                .arg(sw->property("gamutWeight").toDouble()).arg(sw->property("hdrWeight").toDouble()));
        if (stage->property("scrgb").toBool())
            check(r.outside > 0, "the photo's colours beyond sRGB are kept (a Display P3 photo)");
        // PINHOLE_SELFTEST_PAUSE: ms to hold the faded-in stage, then as
        // long with the stage held back (the main window's SDR photo), to
        // look at what the compositor shows of each (grim).
        const int pause = qEnvironmentVariableIntValue("PINHOLE_SELFTEST_PAUSE");
        if (pause <= 0)
            return s->green();
        say(QStringLiteral("(paused: the stage shows)"));
        QTimer::singleShot(pause, window, [=] {
            stageWindow()->setProperty("paused", true);
            QTimer::singleShot(300, window, [=] {
                say(QStringLiteral("(paused: the main window shows)"));
                QTimer::singleShot(pause, window, [=] {
                    stageWindow()->setProperty("paused", false);
                    waitFor([=] { return tag(stageWindow()) && stageWindow()->property("fade").toDouble() >= 1; }, 5000,
                            "the stage takes over again", s->green);
                });
            });
        });
    }); };

    // A Display P3 green, decoded and drawn by the stage's own photo item:
    // the value Pinhole hands the compositor.
    s->green = [=] {
        QQuickWindow *sw = stageWindow();
        const QString path = dir + "/p3-green.png";
        QImage green(64, 64, QImage::Format_RGB32);
        green.fill(QColor(0, 255, 0));
        QFile icc(qEnvironmentVariable("PINHOLE_SELFTEST_P3", QStringLiteral(":/sRGB.icc")));
        QByteArray profile;
        if (icc.open(QIODevice::ReadOnly))
            profile = icc.readAll();
        green.setColorSpace(QColorSpace::fromIccProfile(profile));
        green.save(path);
        auto *photo = new StagePhoto(sw->contentItem());
        photo->setObjectName("greenProbe");
        photo->setSize(QSizeF(48, 48));
        photo->setPosition(QPointF(4, 4));
        photo->setZ(100);
        photo->setLinear(stage->property("scrgb").toBool());
        photo->setGamut(1);
        photo->setSource(FileProvider::urlOf(path));
        waitFor([photo] { return !photo->shown().isEmpty(); }, 5000, "the P3 green decoded", [=] {
            QTimer::singleShot(100, window, [=] { readBack(sw, [=](const QImage &grabbed) {
                const Rgb p = at(grabbed, int(28 * dpr), int(28 * dpr));
                say(QStringLiteral("Display P3 green (0, 255, 0) in the stage: (%1, %2, %3); Display P3's green in linear sRGB is "
                                   "(-0.2249, 1.0421, -0.0786)")
                        .arg(p.r, 0, 'f', 4).arg(p.g, 0, 'f', 4).arg(p.b, 0, 'f', 4));
                if (stage->property("scrgb").toBool())
                    check(std::abs(p.r + 0.2249) < 2e-3 && std::abs(p.g - 1.0421) < 2e-3 && std::abs(p.b + 0.0786) < 2e-3,
                          "a P3 green reaches the compositor as P3's green, beyond sRGB");
                delete photo;
                s->zoomed();
            }); });
        });
    };

    // Zoomed and panned: the stage's photo where the viewer's is.
    s->zoomed = [=, &viewer] {
        const QRect frame = s->frame;
        viewer.zoomBy(2.2, frame.center().x() / dpr, frame.center().y() / dpr);
        auto steps = std::make_shared<int>(0);
        auto apart = std::make_shared<double>(0);
        auto *follow = new QTimer(window);
        follow->setInterval(8);
        QObject::connect(follow, &QTimer::timeout, window, [=, &viewer] {
            QQuickItem *viewerFrame = photoViewer()->property("frameItem").value<QQuickItem *>();
            QQuickItem *stagePhoto = stageWindow()->findChild<QQuickItem *>(QStringLiteral("stagePhoto"));
            if (viewerFrame && stagePhoto) {
                const QRectF a = viewerFrame->mapRectToScene(QRectF(0, 0, viewerFrame->width(), viewerFrame->height()));
                const QRectF b = stagePhoto->mapRectToScene(QRectF(0, 0, stagePhoto->width(), stagePhoto->height()));
                *apart = std::max({*apart, std::abs(a.x() - b.x()), std::abs(a.y() - b.y()), std::abs(a.width() - b.width()),
                                   std::abs(a.height() - b.height())});
                ++*steps;
            }
            if (*steps == 20)
                viewer.panBy(-180, 90);
        });
        follow->start();
        QTimer::singleShot(1500, window, [=, &viewer] {
            follow->deleteLater();
            say(QStringLiteral("zoomed 2.2× and panned: %1 steps, the stage's photo and the viewer's frame at most %2 px apart")
                    .arg(*steps).arg(*apart));
            check(*steps > 50 && *apart < 0.01, "zoom and pan move both windows' photo alike");
            check(tag(stageWindow()), "zoomed and panned, the stage still shows");
            readBack(stageWindow(), [=, &viewer](const QImage &grabbed) {
                savePfm(grabbed, dir + "/stage-zoomed.pfm");
                window->grabWindow().save(dir + "/main-zoomed.png");
                viewer.resetZoom();
                QTimer::singleShot(800, window, s->held);
            });
        });
    };

    // A plugin holds SDR: the stage fades its gamut out, and in again.
    s->held = [=] {
        QObject *info = pluginItem(window, QStringLiteral("pinhole.info"));
        if (!info) {
            say("no Info plugin to hold SDR with");
            return s->liveMotion();
        }
        info->setProperty("holdSdr", true);
        QTimer::singleShot(600, window, [=] { readBack(stageWindow(), [=](const QImage &grabbed) {
            QQuickWindow *sw = stageWindow();
            const Range r = rangeOf(grabbed, s->frame);
            say(QStringLiteral("a plugin holds SDR: gamut %1, HDR %2, %3 % beyond sRGB").arg(sw->property("gamutWeight").toDouble())
                    .arg(sw->property("hdrWeight").toDouble()).arg(100 * r.outside, 0, 'f', 3));
            check(sw->property("gamutWeight").toDouble() == 0 && r.outside == 0, "held, the stage is at SDR");
            info->setProperty("holdSdr", false);
            QTimer::singleShot(600, window, [=] {
                check(stageWindow()->property("gamutWeight").toDouble() == (stage->property("scrgb").toBool() ? 1 : 0),
                      "let go, it fades back in");
                s->liveMotion();
            });
        }); });
    };

    // Live Photos' motion: SDR over the photo, the stage at SDR meanwhile.
    s->liveMotion = [=, &viewer] {
        QObject *live = pluginItem(window, QStringLiteral("pinhole.live"));
        if (s->live < 0 || !live) {
            say("no Live Photo given (PINHOLE_SELFTEST_PHOTOS), or Live Photos off");
            return s->playVideo();
        }
        reopen(s->live);
        waitFor([=, &viewer] { return viewer.index() == s->live && tag(stageWindow()) && stageWindow()->property("fade").toDouble() >= 1
                                   && live->property("liveLayer").value<QObject *>(); },
                15000, "a Live Photo on the stage", [=] {
            QObject *layer = live->property("liveLayer").value<QObject *>();
            if (layer)
                QMetaObject::invokeMethod(layer, "play");
            auto heldSeen = std::make_shared<bool>(false);
            auto lowest = std::make_shared<double>(1);
            auto *watch = new QTimer(window);
            watch->setInterval(10);
            QObject::connect(watch, &QTimer::timeout, window, [=] {
                if (live->property("holdSdr").toBool() && stageWindow()->property("holdSdr").toBool())
                    *heldSeen = true;
                *lowest = std::min(*lowest, stageWindow()->property("gamutWeight").toDouble());
            });
            watch->start();
            waitFor([=] { return *heldSeen && !live->property("holdSdr").toBool() && stageWindow()->property("fade").toDouble() >= 1
                                 && stageWindow()->property("hold").toDouble() >= 1; },
                    20000, "the motion plays and ends", [=] {
                watch->deleteLater();
                check(*heldSeen, "while the motion plays, Live Photos holds SDR and the stage hears it");
                check(*lowest == 0 || !stage->property("scrgb").toBool(), QStringLiteral("the stage went to SDR meanwhile (gamut at least %1)").arg(*lowest));
                s->playVideo();
            });
        });
    };

    // A video of the viewer's, played in the stage too.
    s->playVideo = [=, &viewer] {
        if (s->video < 0) {
            say("no video given (PINHOLE_SELFTEST_PHOTOS)");
            return s->hdrVideo();
        }
        reopen(s->video);
        waitFor([=] {
            QQuickItem *v = stageWindow()->findChild<QQuickItem *>(QStringLiteral("stageVideo"));
            return tag(stageWindow()) && v && v->isVisible() && v->property("framesDrawn").toInt() > 5;
        }, 20000, "the viewer's video plays in the stage", [=] {
            QQuickItem *v = stageWindow()->findChild<QQuickItem *>(QStringLiteral("stageVideo"));
            say(QStringLiteral("the stage's video: %1 frames drawn, HDR %2").arg(v->property("framesDrawn").toInt()).arg(v->property("hdr").toBool()));
            readBack(stageWindow(), [=](const QImage &grabbed) {
                savePfm(grabbed, dir + "/stage-video.pfm");
                s->hdrVideo();
            });
        });
    };

    // An HDR clip in the stage, by the viewer's shader: at SDR and at the
    // display's headroom, beside the same in the main window.
    s->hdrVideo = [=] {
        if (hdrClip.isEmpty()) {
            say("no HDR clip given (PINHOLE_SELFTEST_VIDEOS)");
            return s->leaving();
        }
        QQuickWindow *sw = stageWindow();
        auto *player = new QMediaPlayer(window);
        auto *inStage = new VideoView(sw->contentItem());
        inStage->setObjectName("hdrProbe");
        inStage->setSize(QSizeF(480, 270));
        inStage->setZ(100);
        inStage->setLinear(stage->property("scrgb").toBool());
        const double headroom = std::pow(2.0, display->headroom());
        inStage->setHeadroom(headroom);
        auto *inMain = new VideoView(window->contentItem());
        inMain->setSize(QSizeF(480, 270));
        inMain->setZ(1000);
        inMain->setFollows(inStage);
        player->setVideoOutput(inStage);
        player->setSource(QUrl::fromLocalFile(hdrClip));
        player->play();
        waitFor([=] { return inStage->framesDrawn() > 3 && inMain->framesDrawn() > 3; }, 10000, "the HDR clip plays", [=] {
            player->pause();
            QTimer::singleShot(300, window, [=] { readBack(sw, [=](const QImage &atHeadroom) {
                savePfm(atHeadroom, dir + "/stage-hdr-video.pfm");
                const Range r = rangeOf(atHeadroom, QRect(0, 0, int(480 * dpr), int(270 * dpr)));
                inStage->setHeadroom(1);
                QTimer::singleShot(200, window, [=] { readBack(sw, [=](const QImage &atSdr) {
                    const QImage main = window->grabWindow();
                    main.save(dir + "/main-hdr-video.png");
                    const Difference d = compare(atSdr, main, QRect(0, 0, int(480 * dpr), int(270 * dpr)), inStage->linear());
                    say(QStringLiteral("an HLG clip in the stage (HDR %1): at %2× SDR white its brightest is %3; at 1×, against the main "
                                       "window's: mean %4, worst %5 codes")
                            .arg(inStage->hdr()).arg(headroom, 0, 'f', 2).arg(r.most, 0, 'f', 3).arg(d.mean, 0, 'f', 3).arg(d.worst));
                    check(inStage->hdr(), "the clip is drawn by Pinhole's HDR shader in the stage");
                    if (inStage->linear() && headroom > 1.05)
                        check(r.most > 1.05, "above SDR white at the display's headroom");
                    check(d.worst <= 2, "at SDR, the stage's frame is the main window's");
                    player->stop();
                    delete inMain;
                    delete inStage;
                    player->deleteLater();
                    s->leaving();
                }); });
            }); });
        });
    };

    // Going back to the grid: the main window draws the photo again, then
    // the stage goes.
    s->leaving = [=, &viewer] {
        if (!viewer.isOpen() || !tag(stageWindow()))
            reopen(s->photo);
        waitFor([=] { return tag(stageWindow()); }, 10000, "the photo on the stage again", [=, &viewer] {
            viewer.close();
            auto order = std::make_shared<QString>();
            auto *watch = new QTimer(window);
            watch->setInterval(2);
            QObject::connect(watch, &QTimer::timeout, window, [=] {
                if (order->isEmpty() && !tag(stageWindow()))
                    *order = stageWindow()->isVisible() ? "main first" : "stage first";
            });
            watch->start();
            waitFor([=] { return !stageWindow()->isVisible(); }, 3000, "the stage goes", [=] {
                watch->deleteLater();
                check(*order == QLatin1String("main first"), "the main window draws the photo again before the stage goes (" + *order + ")");
                finish();
            });
        });
    };

    // Once the library and the viewer are there.
    waitFor([=, &library] { return library.count() > 0 && photoViewer(); }, 60000, "the library and the viewer", start);
}

// PINHOLE_SELFTEST=gainmap:<dir> (selftest.h): each photo of the library
// with a gain map, as the stage draws it at the display's headroom.
void runGainMapSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &dir) {
    auto failures = std::make_shared<int>(0);
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto check = [say, failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    auto waitFor = [window, check](std::function<bool()> ready, int ms, const QString &what, std::function<void()> then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto *poll = new QTimer(window);
        poll->setInterval(5);
        QObject::connect(poll, &QTimer::timeout, window, [=] {
            if (ready()) {
                poll->deleteLater();
                then();
            } else if (clock->elapsed() > ms) {
                poll->deleteLater();
                check(false, what + QStringLiteral(" (timed out)"));
                then();
            }
        });
        poll->start();
    };
    QObject *stage = qmlEngine(window)->rootContext()->contextProperty("stage").value<QObject *>();
    auto *display = qmlEngine(window)->rootContext()->contextProperty("display").value<ColourDisplay *>();
    auto stageWindow = [window] { return qobject_cast<QQuickWindow *>(window->property("stageWindow").value<QObject *>()); };
    auto photoViewer = [window] { return window->findChild<QQuickItem *>(QStringLiteral("photoViewer")); };
    auto stagePhoto = [stageWindow] {
        return stageWindow() ? stageWindow()->findChild<StagePhoto *>(QStringLiteral("stagePhoto")) : nullptr;
    };
    // The display's headroom, as PINHOLE_HEADROOM says it, changed live.
    auto setHeadroom = [display](double stops) {
        qputenv("PINHOLE_HEADROOM", QByteArray::number(stops, 'f', 6));
        emit display->changed();
    };
    const qreal dpr = window->devicePixelRatio();

    // Each photo with a map, then an iPhone's HEIC again with Apple's map.
    struct Run {
        int index;
        bool apple;
    };
    struct State {
        QList<Run> runs;
        int at = -1;
        QImage a, b, c, d;
        QRectF frame;
        QSize imageSize;
        QUrl shown;
        QMatrix4x4 toSrgb;
        int tries = 0;
        double weightAtOwn = 0;
        GainMap map;
        QString path;
        double own = 0;
        std::function<void()> next;
    };
    auto s = std::make_shared<State>();

    auto finish = [=] {
        qunsetenv("PINHOLE_GAINMAP");
        say(QStringLiteral("done, %1 failed").arg(*failures));
        QCoreApplication::exit(*failures ? 1 : 0);
    };

    // What the stage drew at headroom 0 (A), 1 (C) and the photo's own
    // (D), against its SDR base (B, the map off) and the maths on the CPU.
    auto analyse = [=] {
        // The viewer took a sharper image meanwhile: the photo again.
        if ((stagePhoto()->shown() != s->shown || stagePhoto()->imageSize() != s->imageSize) && ++s->tries < 3) {
            say(QStringLiteral("  (the stage's photo changed while it was read: again)"));
            --s->at;
            return s->next();
        }
        s->tries = 0;
        const GainMap &map = s->map;
        check(std::abs(s->weightAtOwn - 1) < 1e-6, QStringLiteral("at its own headroom, the map's whole weight (%1)").arg(s->weightAtOwn));
        const QString name = QFileInfo(s->path).completeBaseName() + (s->runs[s->at].apple ? "-apple" : "");
        savePfm(s->b, dir + "/" + name + "-base.pfm");
        savePfm(s->c, dir + "/" + name + "-1.pfm");
        savePfm(s->d, dir + "/" + name + "-own.pfm");
        const QRect pixels = s->frame.toAlignedRect().adjusted(3, 3, -3, -3).intersected(s->b.rect());
        // Headroom 0 (x1): the SDR base, to the bit.
        long differ = 0;
        for (int y = pixels.top(); y <= pixels.bottom(); ++y)
            for (int x = pixels.left(); x <= pixels.right(); ++x) {
                const Rgb p = at(s->a, x, y), q = at(s->b, x, y);
                differ += p.r != q.r || p.g != q.g || p.b != q.b;
            }
        check(differ == 0, QStringLiteral("%1 at headroom 0 (x1): the SDR base to the bit (%2 of %3 pixels differ)")
                               .arg(name).arg(differ).arg(pixels.width() * pixels.height()));
        // Where in the map each pixel's middle is: the item filled as
        // PreserveAspectCrop has it (stage.cpp), by the photo's pixels'
        // shape, the map over the whole photo.
        const QSizeF image(map.image.size());
        const double frameAspect = s->frame.width() / s->frame.height();
        const double imageAspect = double(s->imageSize.width()) / s->imageSize.height();
        const double sw = frameAspect > imageAspect ? 1 : frameAspect / imageAspect;
        const double sh = frameAspect > imageAspect ? imageAspect / frameAspect : 1;
        auto codesAt = [&](int x, int y, double codes[3]) {
            const double u = (1 - sw) / 2 + (x + 0.5 - s->frame.left()) / s->frame.width() * sw;
            const double v = (1 - sh) / 2 + (y + 0.5 - s->frame.top()) / s->frame.height() * sh;
            const double fx = u * image.width() - 0.5, fy = v * image.height() - 0.5;
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const double tx = fx - x0, ty = fy - y0;
            auto texel = [&](int px, int py, int c) {
                const int cx = std::clamp(px, 0, map.image.width() - 1), cy = std::clamp(py, 0, map.image.height() - 1);
                const uchar *line = map.image.constScanLine(cy);
                return map.multichannel() ? line[3 * cx + c] / 255.0 : line[cx] / 255.0;
            };
            for (int c = 0; c < 3; ++c)
                codes[c] = (1 - ty) * ((1 - tx) * texel(x0, y0, c) + tx * texel(x0 + 1, y0, c))
                         + ty * ((1 - tx) * texel(x0, y0 + 1, c) + tx * texel(x0 + 1, y0 + 1, c));
        };
        const double weight = stageWindow()->property("hdrWeight").toDouble();
        const QMatrix4x4 fromSrgb = s->toSrgb.inverted();
        struct Error {
            double mean = 0, worst = 0, p99 = 0, brightest = 0;
        };
        // Relative to the brighter, and to 1/64 of SDR white in the darks.
        auto against = [&](const QImage &got, double stops) {
            const double w = weight * map.weight(stops);
            Error e;
            std::vector<double> all;
            for (int y = pixels.top(); y <= pixels.bottom(); y += 2)
                for (int x = pixels.left(); x <= pixels.right(); x += 2) {
                    const Rgb base = at(s->b, x, y), g = at(got, x, y);
                    double rgb[3] = {base.r, base.g, base.b}, codes[3];
                    codesAt(x, y, codes);
                    if (map.multichannel()) {
                        const QVector3D own = fromSrgb.map(QVector3D(float(rgb[0]), float(rgb[1]), float(rgb[2])));
                        double o[3] = {own.x(), own.y(), own.z()};
                        map.apply(o, codes, w);
                        const QVector3D back = s->toSrgb.map(QVector3D(float(o[0]), float(o[1]), float(o[2])));
                        rgb[0] = back.x(), rgb[1] = back.y(), rgb[2] = back.z();
                    } else {
                        map.apply(rgb, codes, w);
                    }
                    const double out[3] = {g.r, g.g, g.b};
                    double most = 0;
                    for (int c = 0; c < 3; ++c)
                        most = std::max(most, std::abs(rgb[c] - out[c]) / std::max({std::abs(rgb[c]), std::abs(out[c]), 1.0 / 64}));
                    e.brightest = std::max({e.brightest, out[0], out[1], out[2]});
                    e.mean += most;
                    all.push_back(most);
                }
            if (!all.empty()) {
                e.mean /= double(all.size());
                std::sort(all.begin(), all.end());
                e.worst = all.back();
                e.p99 = all[size_t(double(all.size() - 1) * 0.99)];
            }
            return e;
        };
        for (const auto &[got, stops] : {std::pair{&s->c, 1.0}, std::pair{&s->d, s->own}}) {
            const Error e = against(*got, stops);
            say(QStringLiteral("%1 at %2 stops (weight %3): the brightest %4; against the maths, mean %5, 99 % within %6, worst %7")
                    .arg(name).arg(stops, 0, 'f', 2).arg(weight * map.weight(stops), 0, 'f', 3).arg(e.brightest, 0, 'f', 3)
                    .arg(e.mean, 0, 'g', 3).arg(e.p99, 0, 'g', 3).arg(e.worst, 0, 'g', 3));
            check(e.mean < 2e-3 && e.p99 < 1e-2, QStringLiteral("%1 at %2 stops is the maths").arg(name).arg(stops, 0, 'f', 2));
        }
        // In the log domain the weight scales the gain: log2(C/B) is
        // log2(D/B) times w(1) / w(own).
        std::vector<double> ratios;
        auto luminance = [](const Rgb &p) { return 0.2126 * p.r + 0.7152 * p.g + 0.0722 * p.b; };
        for (int y = pixels.top(); y <= pixels.bottom(); y += 2)
            for (int x = pixels.left(); x <= pixels.right(); x += 2) {
                const double lb = luminance(at(s->b, x, y)), lc = luminance(at(s->c, x, y)), ld = luminance(at(s->d, x, y));
                if (lb > 0.02 && ld / lb > std::exp2(0.5))
                    ratios.push_back(std::log2(lc / lb) / std::log2(ld / lb));
            }
        const double want = map.weight(1) / map.weight(s->own);
        if (ratios.size() > 100) {
            std::sort(ratios.begin(), ratios.end());
            const double median = ratios[ratios.size() / 2];
            const double low = ratios[ratios.size() / 10], high = ratios[ratios.size() * 9 / 10];
            say(QStringLiteral("%1 in the log domain: log2(C/B) / log2(D/B) over %2 pixels half a stop or more above their base: "
                               "median %3, 80 % within %4 to %5; w(1) / w(own) is %6")
                    .arg(name).arg(ratios.size()).arg(median, 0, 'f', 4).arg(low, 0, 'f', 4).arg(high, 0, 'f', 4).arg(want, 0, 'f', 4));
            check(std::abs(median - want) < 0.01 && high - low < 0.05,
                  QStringLiteral("%1: the weight scales the gain in the log domain").arg(name));
        } else {
            check(false, QStringLiteral("%1: too few pixels above their base to see the weight (%2)").arg(name).arg(ratios.size()));
        }
        // To look at: the gain the stage drew, log2(D/B) over the photo's
        // own headroom, in grey; and D at a quarter, two stops above SDR
        // white then white.
        QImage gainImage(pixels.size(), QImage::Format_Grayscale8), shown(pixels.size(), QImage::Format_RGB32);
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x) {
                const Rgb b = at(s->b, pixels.left() + x, pixels.top() + y), d = at(s->d, pixels.left() + x, pixels.top() + y);
                const double lb = luminance(b), ld = luminance(d);
                const double g = lb > 1e-3 ? std::log2(ld / lb) / std::max(s->own, 0.1) : 0;
                gainImage.scanLine(y)[x] = uchar(std::clamp(g * 255, 0.0, 255.0));
                shown.setPixel(x, y, qRgb(int(encode(d.r / 4)), int(encode(d.g / 4)), int(encode(d.b / 4))));
            }
        gainImage.save(dir + "/" + name + "-gain.png");
        shown.save(dir + "/" + name + "-own-quarter.png");
        s->next();
    };

    // One photo: opened at headroom 0, settled, faded in, its map decoded;
    // read back with its map and without, then at 1 stop and its own.
    auto runOne = [=, &viewer, &library] {
        const Run run = s->runs[s->at];
        if (run.apple)
            qputenv("PINHOLE_GAINMAP", "apple");
        else
            qunsetenv("PINHOLE_GAINMAP");
        setHeadroom(0);
        viewer.close();
        waitFor([=] { return !photoViewer()->property("shown").toBool() && !stageWindow()->isVisible(); }, 5000, "the viewer closes",
                [=, &viewer, &library] {
            QMetaObject::invokeMethod(&viewer, "openAt", Q_ARG(int, run.index), Q_ARG(QRectF, QRectF(100, 100, 100, 100)));
            say(QStringLiteral("the photo: %1%2").arg(library.at(run.index).filename).arg(run.apple ? " (Apple's map)" : ""));
            auto ready = [=, &viewer] {
                StagePhoto *photo = stagePhoto();
                const QVariant sharpest = photoViewer()->property("sharpest");
                return photo && stage->property("scrgb").toBool() && stageWindow()->property("handedOver").toBool() && stageWindow()->property("fade").toDouble() >= 1
                    && !viewer.loading() && sharpest.isValid()
                    && photo->shown() == sharpest.toMap().value("source").toUrl() && !photo->gainMapKind().isEmpty();
            };
            // And still so for a while: the viewer's sharper image, should
            // one come, is what the stage draws.
            auto since = std::make_shared<QElapsedTimer>();
            auto settled = [=] {
                if (!ready()) {
                    since->invalidate();
                    return false;
                }
                if (!since->isValid())
                    since->start();
                return since->elapsed() > 600;
            };
            waitFor(settled, 30000, "the stage shows the photo with its map", [=] {
                StagePhoto *photo = stagePhoto();
                s->path = StagePhoto::pathOf(photo->shown());
                s->map = readGainMap(s->path);
                s->imageSize = photo->imageSize();
                s->shown = photo->shown();
                // The photo's primaries (its profile's matrix to linear
                // sRGB), where a three-channel map applies.
                QByteArray profile;
                decodePhoto(s->path, QSize(64, 64), nullptr, &profile);
                StageColour colour;
                stageColourOf(profile, colour);
                s->toSrgb = QMatrix4x4(colour.matrix[0], colour.matrix[1], colour.matrix[2], 0, colour.matrix[3], colour.matrix[4],
                                       colour.matrix[5], 0, colour.matrix[6], colour.matrix[7], colour.matrix[8], 0, 0, 0, 0, 1);
                s->own = s->map.kind == QLatin1String("apple") ? std::log2(s->map.appleHeadroom) : s->map.alternateHeadroom;
                const QQuickItem *frame = photoViewer()->property("frameItem").value<QQuickItem *>();
                const QRectF r = frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height()));
                s->frame = QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr);
                say(QStringLiteral("  %1: %2's map, %3x%4 for %5x%6, %7 channel(s), headroom %8 stops; drawn at weight %9")
                        .arg(QFileInfo(s->path).fileName()).arg(photo->gainMapKind()).arg(s->map.image.width())
                        .arg(s->map.image.height()).arg(photo->shownSize().width()).arg(photo->shownSize().height())
                        .arg(s->map.multichannel() ? 3 : 1).arg(s->own, 0, 'f', 3).arg(photo->gainMapWeight(), 0, 'f', 3));
                check(s->map.kind == photo->gainMapKind() && (!run.apple || s->map.kind == QLatin1String("apple")),
                      QStringLiteral("its map is %1's").arg(photo->gainMapKind()));
                QTimer::singleShot(200, window, [=] { readBack(stageWindow(), [=](const QImage &a) {
                    s->a = a;
                    stagePhoto()->setGainMapOn(false);
                    QTimer::singleShot(100, window, [=] { readBack(stageWindow(), [=](const QImage &b) {
                        s->b = b;
                        stagePhoto()->setGainMapOn(true);
                        setHeadroom(1);
                        QTimer::singleShot(100, window, [=] { readBack(stageWindow(), [=](const QImage &c) {
                            s->c = c;
                            setHeadroom(s->own);
                            QTimer::singleShot(100, window, [=] { readBack(stageWindow(), [=](const QImage &d) {
                                s->d = d;
                                s->weightAtOwn = stagePhoto()->gainMapWeight();
                                analyse();
                            }); });
                        }); });
                    }); });
                }); });
            });
        });
    };
    s->next = [=] {
        if (++s->at >= s->runs.size())
            return finish();
        runOne();
    };

    auto start = [=, &library] {
        say(QStringLiteral("display: headroom %1 stops; stage wanted %2, usable %3, scRGB %4; PINHOLE_STAGE=%5")
                .arg(display->headroom(), 0, 'f', 2).arg(stage->property("wanted").toBool()).arg(stage->property("usable").toBool())
                .arg(stage->property("scrgb").toBool()).arg(Stage::mode()));
        if (!stageWindow()) {
            check(false, "a stage (run it in an HDR output, or PINHOLE_STAGE=force where it can be scRGB)");
            return finish();
        }
        for (int i = 0; i < library.count(); ++i) {
            if (library.at(i).video)
                continue;
            s->runs.append({i, false});
            if (library.at(i).filename.endsWith(".HEIC", Qt::CaseInsensitive))
                s->runs.append({i, true});
        }
        s->next();
    };
    waitFor([=, &library] { return library.count() > 0 && photoViewer() && stageWindow(); }, 60000,
            "the library, the viewer and the stage", start);
}
