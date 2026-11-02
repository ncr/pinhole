// PINHOLE_SELFTEST=hdrvideo:<dir>: HDR videos as Qt Multimedia shows them
// in SDR, against ffmpeg's tone mapping of the same frame; and
// PINHOLE_SELFTEST=hdrplay: how they play (selftest.h).

#include "selftest.h"

#include "colour.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#include <sys/resource.h>

namespace {

// As the viewer plays a video (PhotoViewer.qml), over the whole window,
// the frame whole.
const char kPlayer[] = R"(
import QtQuick
import QtMultimedia
import Pinhole
Rectangle {
    color: "black"
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    property alias source: player.source
    readonly property int status: player.mediaStatus
    readonly property rect shown: pinhole ? view.contentRect : out.contentRect
    readonly property bool hasFrame: player.hasVideo
    // Through Pinhole's VideoView, as the viewer has it, or a VideoOutput,
    // as Qt alone shows it.
    property bool pinhole: true
    MediaPlayer { id: player; videoOutput: pinhole ? view : out }
    VideoOutput { id: out; visible: !pinhole; anchors.fill: parent; fillMode: VideoOutput.PreserveAspectFit }
    VideoView { id: view; visible: pinhole; anchors.fill: parent; fillMode: VideoView.PreserveAspectFit }
    function showFirst() { player.pause() }
}
)";

// ffmpeg's tone mappings to compare with, by name: the filter before the
// frame is scaled. "hable203" was cover.rs's at first.
struct Reference {
    const char *name;
    const char *filter;
};
const Reference kReferences[] = {
    {"hable203", "zscale=t=linear:npl=203,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv"},
    {"hable100", "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv"},
    {"mobius100", "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=mobius:desat=0,zscale=t=bt709:m=bt709:r=tv"},
    {"clip100", "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=clip:desat=0,zscale=t=bt709:m=bt709:r=tv"},
    // libplacebo's BT.2390, the Dolby Vision RPU left aside (docs/hdr.md).
    {"placebo2390", "libplacebo=tonemapping=bt.2390:apply_dolbyvision=0:colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv:format=yuv420p"},
    // The same, as Pinhole's maths have it: no peak found frame by frame,
    // no contrast recovered, colours beyond sRGB clipped.
    {"placebo2390clip", "libplacebo=tonemapping=bt.2390:gamut_mode=clip:peak_detect=0:contrast_recovery=0:apply_dolbyvision=0:colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv:format=yuv420p"},
    {"asis", "null"},
};

// Mean of each channel over a grid of blocks; the largest difference of a
// block's means, and the mean difference over all.
struct Difference {
    double mean = 0, worst = 0;
};

Difference compare(const QImage &a, const QImage &b, int columns, int rows) {
    Difference d;
    int n = 0;
    for (int by = 0; by < rows; ++by)
        for (int bx = 0; bx < columns; ++bx) {
            const int x0 = a.width() * bx / columns, x1 = a.width() * (bx + 1) / columns;
            const int y0 = a.height() * by / rows, y1 = a.height() * (by + 1) / rows;
            double sa[3] = {}, sb[3] = {};
            int count = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const QRgb pa = a.pixel(x, y), pb = b.pixel(x, y);
                    sa[0] += qRed(pa), sa[1] += qGreen(pa), sa[2] += qBlue(pa);
                    sb[0] += qRed(pb), sb[1] += qGreen(pb), sb[2] += qBlue(pb);
                    ++count;
                }
            if (count == 0)
                continue;
            for (int c = 0; c < 3; ++c) {
                const double diff = std::abs(sa[c] - sb[c]) / count;
                d.worst = std::max(d.worst, diff);
                d.mean += diff;
                ++n;
            }
        }
    if (n)
        d.mean /= n;
    return d;
}

// The video's first frame, tone mapped by `filter`, at `size`; null if
// ffmpeg cannot.
QImage reference(const QString &path, const QString &filter, QSize size) {
    QProcess ffmpeg;
    const QStringList vulkan = filter.contains(QLatin1String("libplacebo")) ? QStringList{"-init_hw_device", "vulkan"} : QStringList();
    ffmpeg.start("ffmpeg", QStringList{"-v", "error", "-nostdin"} + vulkan + QStringList{"-i", path, "-frames:v", "1", "-vf",
                            QStringLiteral("%1,scale=%2:%3:flags=area,format=rgb24").arg(filter).arg(size.width()).arg(size.height()),
                            "-f", "rawvideo", "-"});
    if (!ffmpeg.waitForFinished(60000))
        return {};
    const QByteArray bytes = ffmpeg.readAllStandardOutput();
    if (bytes.size() != qsizetype(size.width()) * size.height() * 3)
        return {};
    QImage image(size, QImage::Format_RGB888);
    for (int y = 0; y < size.height(); ++y)
        std::memcpy(image.scanLine(y), bytes.constData() + qsizetype(y) * size.width() * 3, size_t(size.width()) * 3);
    return image;
}

// The video's curve, as ffprobe says it: "arib-std-b67", "smpte2084"...
QString transferOf(const QString &path) {
    QProcess ffprobe;
    ffprobe.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=color_transfer", "-of",
                              "default=noprint_wrappers=1:nokey=1", path});
    ffprobe.waitForFinished(30000);
    return QString::fromUtf8(ffprobe.readAllStandardOutput()).trimmed();
}

// The video's picture size, turned as it is shown.
QSize sizeOf(const QString &path) {
    QProcess ffprobe;
    ffprobe.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries",
                              "stream=width,height:stream_side_data=rotation", "-of", "default=noprint_wrappers=1", path});
    ffprobe.waitForFinished(30000);
    int width = 0, height = 0, rotation = 0;
    for (const QString &line : QString::fromUtf8(ffprobe.readAllStandardOutput()).split('\n')) {
        if (line.startsWith(QLatin1String("width=")))
            width = line.mid(6).toInt();
        else if (line.startsWith(QLatin1String("height=")))
            height = line.mid(7).toInt();
        else if (line.startsWith(QLatin1String("rotation=")))
            rotation = line.mid(9).toInt();
    }
    return std::abs(rotation) % 180 == 90 ? QSize(height, width) : QSize(width, height);
}

// The first frame made SDR by the formulas themselves (colour.h, in
// double precision, no tables): ffmpeg's swscale gives its R'G'B' at its
// own size (BT.2020's matrix), and each pixel is made linear (HLG's inverse
// OETF and OOTF at 1000 nits; PQ's EOTF), put through BT.2390's EETF to
// SDR white on its largest channel, made sRGB and clipped; then scaled to
// `size`, as the window scales the frame. Null for SDR.
QImage formulas(const QString &path, QSize shown) {
    const QString transfer = transferOf(path);
    const bool hlg = transfer == QLatin1String("arib-std-b67");
    if (!hlg && transfer != QLatin1String("smpte2084"))
        return {};
    const QSize size = sizeOf(path);
    if (size.isEmpty())
        return {};
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-frames:v", "1", "-vf",
                            QStringLiteral("scale=%1:%2:flags=bilinear+full_chroma_int+accurate_rnd:in_color_matrix=bt2020:in_range=tv:out_range=pc,format=gbrp16le")
                                .arg(size.width()).arg(size.height()),
                            "-f", "rawvideo", "-"});
    if (!ffmpeg.waitForFinished(60000))
        return {};
    const QByteArray bytes = ffmpeg.readAllStandardOutput();
    const qsizetype plane = qsizetype(size.width()) * size.height();
    if (bytes.size() != plane * 6)
        return {};
    const auto *g = reinterpret_cast<const uint16_t *>(bytes.constData());
    const uint16_t *b = g + plane, *r = b + plane;
    const double peak = hlg ? kHlgPeakNits : 1000;
    static const double toSrgb[9] = {1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187};
    QImage out(size, QImage::Format_RGB888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *o = out.scanLine(y);
        for (int x = 0; x < size.width(); ++x, o += 3) {
            const qsizetype i = qsizetype(y) * size.width() + x;
            double c[3] = {r[i] / 65535.0, g[i] / 65535.0, b[i] / 65535.0};
            for (double &v : c)
                v = hlg ? hlgSceneLight(v) : pqToNits(v);
            if (hlg) {
                const double ys = 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2];
                const double gain = ys > 0 ? kHlgPeakNits * std::pow(ys, 0.2) : 0;
                for (double &v : c)
                    v *= gain;
            }
            const double most = std::max({c[0], c[1], c[2]});
            const double f = most > 1e-6 ? eetfToSdr(most, peak) / most : 1 / kSdrWhiteNits;
            for (int k = 0; k < 3; ++k) {
                double v = toSrgb[k * 3] * c[0] * f + toSrgb[k * 3 + 1] * c[1] * f + toSrgb[k * 3 + 2] * c[2] * f;
                v = std::clamp(v, 0.0, 1.0);
                v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
                o[k] = uchar(std::lround(v * 255));
            }
        }
    }
    return out.scaled(shown, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

} // namespace

void runHdrVideoSelfTest(QQuickWindow *window, const QString &dir, const QStringList &videos) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto *engine = qmlEngine(window->contentItem());
    auto *component = new QQmlComponent(engine, window);
    component->setData(kPlayer, QUrl());
    auto failed = std::make_shared<int>(0);
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [=](int i) {
        // PINHOLE_SELFTEST_HDRMODES: "qt", "pinhole", or both (the default).
        static const QString modes = qEnvironmentVariable("PINHOLE_SELFTEST_HDRMODES");
        if (i < videos.size() * 2 && ((modes == QLatin1String("qt") && i % 2 == 1) || (modes == QLatin1String("pinhole") && i % 2 == 0))) {
            (*next)(i + 1);
            return;
        }
        if (i >= videos.size() * 2) {
            say(QStringLiteral("done, %1 failed").arg(*failed));
            QCoreApplication::exit(*failed ? 1 : 0);
            return;
        }
        // Each video as Qt alone shows it, then through VideoView.
        const QString path = videos[i / 2];
        const bool pinhole = i % 2 == 1;
        auto *player = qobject_cast<QQuickItem *>(component->create(qmlContext(window->contentItem())));
        if (!player) {
            say(QStringLiteral("FAIL the player: %1").arg(component->errorString()));
            QCoreApplication::exit(1);
            return;
        }
        player->setParentItem(window->contentItem());
        player->setZ(1e6);
        player->setProperty("pinhole", pinhole);
        player->setProperty("source", QUrl::fromLocalFile(path));
        QMetaObject::invokeMethod(player, "showFirst");
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto *poll = new QTimer(player);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, player, [=] {
            const QRectF shown = player->property("shown").toRectF();
            if ((shown.isEmpty() || !player->property("hasFrame").toBool()) && clock->elapsed() < 15000)
                return;
            poll->stop();
            // A moment for the frame to be drawn; then the player alone,
            // rendered into an image of its own: a window grab reads the
            // swapchain, which on a tiled workspace not shown need not be
            // the window's size.
            QTimer::singleShot(800, player, [=] {
                const QSharedPointer<QQuickItemGrabResult> grab = player->grabToImage();
                if (!grab) {
                    say(QStringLiteral("FAIL no grab of %1").arg(path));
                    ++*failed;
                    player->deleteLater();
                    (*next)(i + 1);
                    return;
                }
                QObject::connect(grab.data(), &QQuickItemGrabResult::ready, player, [=] {
                    const QString name = QFileInfo(path).completeBaseName();
                    const QString how = pinhole ? QStringLiteral("pinhole") : QStringLiteral("qt");
                    const QImage whole = grab->image();
                    const qreal scale = qreal(whole.width()) / player->width();
                    const QRectF shown = player->property("shown").toRectF();
                    const QRect rect(QPointF(shown.topLeft() * scale).toPoint() + QPoint(2, 2),
                                     QSizeF(shown.size() * scale).toSize() - QSize(4, 4));
                    const QImage qt = whole.copy(rect).convertToFormat(QImage::Format_RGB888);
                    qt.save(QStringLiteral("%1/%2-%3.png").arg(dir, name, how));
                    QString line = QStringLiteral("%1 (%2x%3), %4: against ffmpeg's").arg(name).arg(qt.width()).arg(qt.height()).arg(how);
                    QString best;
                    double bestWorst = 1e9;
                    for (const Reference &r : kReferences) {
                        const QImage ref = reference(path, QString::fromLatin1(r.filter), qt.size());
                        if (ref.isNull()) {
                            line += QStringLiteral(" %1: none;").arg(QLatin1String(r.name));
                            continue;
                        }
                        ref.save(QStringLiteral("%1/%2-%3.png").arg(dir, name, QLatin1String(r.name)));
                        const Difference d = compare(qt, ref, 16, 9);
                        line += QStringLiteral(" %1 mean %2 worst %3;").arg(QLatin1String(r.name)).arg(d.mean, 0, 'f', 1).arg(d.worst, 0, 'f', 1);
                        if (d.worst < bestWorst) {
                            bestWorst = d.worst;
                            best = QLatin1String(r.name);
                        }
                    }
                    // Through VideoView: an HDR frame within 6 of 255 of the
                    // formulas, block by block; an SDR one of its codes.
                    if (pinhole) {
                        QImage want = formulas(path, qt.size());
                        const bool hdr = !want.isNull();
                        if (!hdr)
                            want = reference(path, QStringLiteral("null"), qt.size());
                        if (!want.isNull()) {
                            want.save(QStringLiteral("%1/%2-formulas.png").arg(dir, name));
                            const Difference d = compare(qt, want, 16, 9);
                            const bool ok = d.worst <= 6;
                            line += QStringLiteral(" %1 %2 mean %3 worst %4%5;")
                                        .arg(hdr ? "the formulas" : "its codes", ok ? "" : "FAIL")
                                        .arg(d.mean, 0, 'f', 1).arg(d.worst, 0, 'f', 1)
                                        .arg(ok ? "" : " (over 6)");
                            if (!ok)
                                ++*failed;
                        }
                    }
                    say(line + QStringLiteral(" nearest %1").arg(best));
                    player->deleteLater();
                    (*next)(i + 1);
                });
                // A frame drawn, which the grab is made in: a window on a
                // workspace not shown draws none of its own.
                window->grabWindow();
            });
        });
        poll->start();
    };
    QTimer::singleShot(2000, window, [next] { (*next)(0); });
}

namespace {

// As the viewer plays a video, the player and its output named.
const char kPlayback[] = R"(
import QtQuick
import QtMultimedia
import Pinhole
Rectangle {
    color: "black"
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    property alias source: player.source
    property bool pinhole: true
    readonly property alias player: player
    readonly property alias view: view
    MediaPlayer { id: player; videoOutput: pinhole ? view : out; loops: MediaPlayer.Infinite; audioOutput: AudioOutput { volume: 0 } }
    VideoOutput { id: out; visible: !pinhole; anchors.fill: parent }
    VideoView { id: view; visible: pinhole; anchors.fill: parent }
}
)";

double cpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
           + double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

} // namespace

void runHdrPlaySelfTest(QQuickWindow *window, const QStringList &videos) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    window->resize(960, 540);
    auto *component = new QQmlComponent(qmlEngine(window->contentItem()), window);
    component->setData(kPlayback, QUrl());
    auto failed = std::make_shared<int>(0);
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [=](int i) {
        if (i >= videos.size() * 2) {
            say(QStringLiteral("done, %1 failed").arg(*failed));
            QCoreApplication::exit(*failed ? 1 : 0);
            return;
        }
        // Each as Qt alone shows it, then through VideoView.
        const QString path = videos[i / 2];
        const bool pinhole = i % 2 == 1;
        const QString name = QFileInfo(path).completeBaseName()
                             + (pinhole ? QStringLiteral(", VideoView") : QStringLiteral(", VideoOutput"));
        auto *item = qobject_cast<QQuickItem *>(component->create(qmlContext(window->contentItem())));
        if (!item) {
            say(QStringLiteral("FAIL the player: %1").arg(component->errorString()));
            QCoreApplication::exit(1);
            return;
        }
        item->setParentItem(window->contentItem());
        item->setZ(1e6);
        item->setProperty("pinhole", pinhole);
        auto *player = item->property("player").value<QMediaPlayer *>();
        auto *view = item->property("view").value<QObject *>();
        // Each frame given, and its time in the stream.
        struct Given {
            std::atomic<int> frames{0};
            std::atomic<qint64> at{-1};
        };
        auto given = std::make_shared<Given>();
        QObject::connect(player->videoSink(), &QVideoSink::videoFrameChanged, item, [given](const QVideoFrame &frame) {
            ++given->frames;
            if (frame.isValid())
                given->at = frame.startTime() / 1000;
        }, Qt::DirectConnection);
        item->setProperty("source", QUrl::fromLocalFile(path));
        player->play();
        struct Measure {
            int given = 0, drawn = 0, renders = 0, lags = 0;
            double cpu = 0, lagSum = 0, lagMost = 0;
            QElapsedTimer clock;
            bool on = false;
        };
        auto m = std::make_shared<Measure>();
        // A frame drawn every 16 ms; the lag of the frame given behind the
        // player's clock (the audio's) as it is.
        auto *frames = new QTimer(item);
        frames->setInterval(16);
        QObject::connect(frames, &QTimer::timeout, item, [=] {
            window->grabWindow();
            if (!m->on)
                return;
            ++m->renders;
            const qint64 at = given->at.load();
            if (at >= 0 && player->playbackState() == QMediaPlayer::PlayingState) {
                const double lag = std::abs(double(player->position() - at));
                // Not as a loop wraps round.
                if (lag < 1000) {
                    m->lagSum += lag;
                    m->lagMost = std::max(m->lagMost, lag);
                    ++m->lags;
                }
            }
        });
        frames->start();
        QTimer::singleShot(1500, item, [=] {
            m->given = given->frames;
            m->drawn = view->property("framesDrawn").toInt();
            m->cpu = cpuSeconds();
            m->clock.start();
            m->on = true;
        });
        QTimer::singleShot(5500, item, [=] {
            const double seconds = m->clock.elapsed() / 1000.0;
            const double perSecond = (given->frames - m->given) / seconds;
            const double drawn = (view->property("framesDrawn").toInt() - m->drawn) / seconds;
            const double cpu = 100 * (cpuSeconds() - m->cpu) / seconds;
            const double lag = m->lags ? m->lagSum / m->lags : 1e9;
            m->on = false;
            QString line = QStringLiteral("%1: %2 frames a second given").arg(name).arg(perSecond, 0, 'f', 1);
            if (pinhole)
                line += QStringLiteral(", %1 drawn").arg(drawn, 0, 'f', 1);
            line += QStringLiteral(" (%1 renders a second), CPU %2 %, lag mean %3 ms, most %4 ms")
                        .arg(m->renders / seconds, 0, 'f', 1)
                        .arg(cpu, 0, 'f', 0)
                        .arg(lag, 0, 'f', 1)
                        .arg(m->lagMost, 0, 'f', 0);
            const double rate = player->metaData().value(QMediaMetaData::VideoFrameRate).toDouble();
            const bool ok = !pinhole || ((rate < 50 || drawn >= 55) && lag <= 50);
            say(line + (ok ? QString() : QStringLiteral(" FAIL")));
            if (!ok)
                ++*failed;
            player->setPosition(1000);
            const int before = given->frames;
            QTimer::singleShot(1000, item, [=] {
                const qint64 at = player->position();
                const int after = given->frames - before;
                const bool on = at >= 1500 && at <= 2600 && after >= 0.6 * (rate > 0 ? rate : 25);
                say(QStringLiteral("%1: a second after a seek to 1 s, at %2 ms, %3 frames given%4")
                        .arg(name)
                        .arg(at)
                        .arg(after)
                        .arg(on ? QString() : QStringLiteral(" FAIL")));
                if (!on && pinhole)
                    ++*failed;
                frames->stop();
                player->stop();
                item->deleteLater();
                QTimer::singleShot(300, window, [=] { (*next)(i + 1); });
            });
        });
    };
    QTimer::singleShot(2000, window, [next] { (*next)(0); });
}
