// PINHOLE_SELFTEST=physics: every plugin screen that scrolls or zooms moves
// as the grid does (selftest.h).

#include "selftest.h"

#include "library.h"
#include "photogrid.h"
#include "scroller.h"
#include "zoomer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QMimeData>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

void say(const QString &line) {
    std::printf("selftest: %s\n", qPrintable(line));
    std::fflush(stdout);
}

const QPointingDevice *touchpad() {
    static QPointingDevice *device = [] {
        auto *d = new QPointingDevice(QStringLiteral("physics touchpad"), 4721, QInputDevice::DeviceType::TouchPad,
                                      QPointingDevice::PointerType::Finger,
                                      QInputDevice::Capability::Position | QInputDevice::Capability::Scroll
                                          | QInputDevice::Capability::PixelScroll,
                                      5, 0);
        QWindowSystemInterface::registerInputDevice(d);
        return d;
    }();
    return device;
}

// One input at a moment of a gesture, `t` ms after it began: a wheel event
// (a notch from the mouse, phases or pixels from the touchpad), or a pinch's.
struct Input {
    double t;
    QPoint pixels, angle;
    Qt::ScrollPhase phase = Qt::NoScrollPhase;
    bool gesture = false;
    Qt::NativeGestureType type = Qt::BeginNativeGesture;
    double value = 0;
};

struct Gesture {
    QString name;
    std::vector<Input> inputs;
};

QList<Gesture> scrolls() {
    QList<Gesture> all;
    // Wayland's notch: 15 px and 120 eighths of a degree.
    Gesture notches{"three notches of the wheel", {}};
    for (int i = 0; i < 3; ++i)
        notches.inputs.push_back({i * 70.0, {0, -15}, {0, -120}});
    all << notches;
    Gesture spin{"the wheel spun, twelve notches 15 ms apart", {}};
    for (int i = 0; i < 12; ++i)
        spin.inputs.push_back({i * 15.0, {0, 15}, {0, 120}});
    all << spin;
    // Two fingers, lifted while moving: it coasts.
    Gesture swipe{"a swipe lifted while moving, and the coast", {{0, {}, {}, Qt::ScrollBegin}}};
    for (int i = 1; i <= 10; ++i)
        swipe.inputs.push_back({i * 8.0, {0, -9}, {0, -72}, Qt::ScrollUpdate});
    swipe.inputs.push_back({88, {}, {}, Qt::ScrollEnd});
    all << swipe;
    // Fingers that stop before they lift: it stays.
    Gesture placed{"a slow drag that stops before the fingers lift", {{0, {}, {}, Qt::ScrollBegin}}};
    for (int i = 1; i <= 12; ++i)
        placed.inputs.push_back({i * 16.0, {0, 3}, {0, 24}, Qt::ScrollUpdate});
    placed.inputs.push_back({400, {}, {}, Qt::ScrollEnd});
    all << placed;
    // A Mac's touchpad goes on after the fingers lift: momentum.
    Gesture momentum{"a swipe with its momentum, as a Mac's", {{0, {}, {}, Qt::ScrollBegin}}};
    for (int i = 1; i <= 6; ++i)
        momentum.inputs.push_back({i * 8.0, {0, -12}, {0, -96}, Qt::ScrollUpdate});
    momentum.inputs.push_back({56, {}, {}, Qt::ScrollEnd});
    for (int i = 0; i < 20; ++i)
        momentum.inputs.push_back({64 + i * 8.0, {0, -12 * (20 - i) / 20}, {0, -96 * (20 - i) / 20}, Qt::ScrollMomentum});
    momentum.inputs.push_back({230, {}, {}, Qt::ScrollEnd});
    all << momentum;
    // A flick, and another while it coasts: it speeds up.
    Gesture twice{"a flick, and another while it coasts", {}};
    for (const double from : {0.0, 250.0}) {
        twice.inputs.push_back({from, {}, {}, Qt::ScrollBegin});
        for (int i = 1; i <= 5; ++i)
            twice.inputs.push_back({from + i * 8, {0, 14}, {0, 112}, Qt::ScrollUpdate});
        twice.inputs.push_back({from + 48, {}, {}, Qt::ScrollEnd});
    }
    all << twice;
    return all;
}

// A pinch out to 1.5 times, held still a moment, then lifted.
Gesture pinch() {
    Gesture g{"a pinch out, held, and let go", {}};
    Input begin{0, {}, {}};
    begin.gesture = true;
    begin.type = Qt::BeginNativeGesture;
    g.inputs.push_back(begin);
    for (int i = 1; i <= 10; ++i) {
        Input zoom{i * 8.0, {}, {}};
        zoom.gesture = true;
        zoom.type = Qt::ZoomNativeGesture;
        zoom.value = 0.05;
        g.inputs.push_back(zoom);
    }
    for (const double t : {130.0, 180.0, 230.0}) {
        Input still{t, {}, {}};
        still.gesture = true;
        still.type = Qt::ZoomNativeGesture;
        g.inputs.push_back(still);
    }
    Input end{240, {}, {}};
    end.gesture = true;
    end.type = Qt::EndNativeGesture;
    g.inputs.push_back(end);
    return g;
}

quint64 clockBase = 50000000;

void deliver(QQuickWindow *window, const Input &input, QPointF at, quint64 base) {
    const quint64 timestamp = base + quint64(std::lround(input.t));
    if (input.gesture) {
        QNativeGestureEvent event(input.type, QPointingDevice::primaryPointingDevice(), 2, at, at, window->mapToGlobal(at),
                                  input.value, QPointF());
        event.setTimestamp(timestamp);
        QGuiApplication::sendEvent(window, &event);
        return;
    }
    const bool pad = input.phase != Qt::NoScrollPhase;
    QWheelEvent event(at, window->mapToGlobal(at), input.pixels, input.angle, Qt::NoButton, Qt::NoModifier, input.phase,
                      false, Qt::MouseEventNotSynthesized, pad ? touchpad() : QPointingDevice::primaryPointingDevice());
    event.setTimestamp(timestamp);
    QGuiApplication::sendEvent(window, &event);
}

// What moves: where it is, one frame of it, back to where it starts, its
// ends, and where over it the input goes.
struct Mover {
    QString name;
    std::function<double()> position;
    std::function<void(double)> advance;
    std::function<void()> reset;
    std::function<std::pair<double, double>()> ends;
    QPointF at;
};

constexpr double kFrame = 0.004;
constexpr int kFrames = 400; // 1.6 s

// The gesture through it, frame by frame on one clock: its positions, from
// where it began, after each frame; and the first frame at an end, if any.
std::pair<std::vector<double>, int> drive(QQuickWindow *window, const Mover &mover, const Gesture &gesture) {
    mover.reset();
    clockBase += 10000;
    const double start = mover.position();
    std::vector<double> out;
    int atEnd = -1;
    size_t next = 0;
    for (int k = 0; k < kFrames; ++k) {
        const double t = k * kFrame * 1000;
        while (next < gesture.inputs.size() && gesture.inputs[next].t <= t)
            deliver(window, gesture.inputs[next++], mover.at, clockBase);
        mover.advance(kFrame);
        const double p = mover.position();
        // A list's ends as it has them now: one of rows of several heights
        // finds them out as it goes.
        const auto [low, high] = mover.ends();
        if (atEnd < 0 && (p <= low + 0.5 || p >= high - 0.5))
            atEnd = k;
        out.push_back(p - start);
    }
    return {out, atEnd};
}

// An item by its type among all the window's under `in`, the largest shown.
template <typename T>
T *largest(QQuickItem *in) {
    T *best = nullptr;
    std::function<void(QQuickItem *)> look = [&](QQuickItem *item) {
        if (!item->isVisible())
            return;
        if (auto *found = qobject_cast<T *>(item))
            if (!best || found->width() * found->height() > best->width() * best->height())
                best = found;
        for (QQuickItem *child : item->childItems())
            look(child);
    };
    look(in);
    return best;
}

QQuickItem *loaderOf(QQuickItem *item, const QString &id) {
    if (item->property("pluginId").toString() == id)
        return item;
    for (QQuickItem *child : item->childItems())
        if (QQuickItem *found = loaderOf(child, id))
            return found;
    return nullptr;
}

QQuickItem *named(QQuickItem *in, const QString &objectName) {
    if (in->objectName() == objectName)
        return in;
    for (QQuickItem *child : in->childItems())
        if (QQuickItem *found = named(child, objectName))
            return found;
    return nullptr;
}

QQuickItem *withProperty(QQuickItem *in, const char *name) {
    if (in->metaObject()->indexOfProperty(name) >= 0)
        return in;
    for (QQuickItem *child : in->childItems())
        if (QQuickItem *found = withProperty(child, name))
            return found;
    return nullptr;
}

void press(QQuickWindow *window, int key) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, key, Qt::NoModifier);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, key, Qt::NoModifier);
}

} // namespace

void runPhysicsSelfTest(QQuickWindow *window, PhotoGrid *grid, const QString &dir) {
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    // The grid's own, at its default columns.
    const Mover gridMover{
        QStringLiteral("the grid"),
        [grid] { return grid->scrollPosition(); },
        [grid](double dt) { grid->advance(dt); },
        [grid] { grid->jumpToFraction(0.5); },
        [grid] { return std::pair<double, double>(0, grid->scrollRange()); },
        grid->mapToScene(QPointF(grid->width() / 2, grid->height() / 2)),
    };
    auto gridCurves = std::make_shared<QList<std::vector<double>>>();
    auto gridPinch = std::make_shared<std::vector<double>>();

    // A plugin's Scroller as a Mover: its Flickable from the middle.
    auto scrollerMover = [window](const QString &name, Scroller *scroller) {
        QQuickItem *f = scroller->flickable();
        auto ends = [f] {
            const double low = f->property("originY").toDouble() - f->property("topMargin").toDouble();
            const double high = f->property("originY").toDouble() + f->property("contentHeight").toDouble()
                                + f->property("bottomMargin").toDouble() - f->height();
            return std::pair<double, double>(low, std::max(low, high));
        };
        return Mover{
            name,
            [f] { return f->property("contentY").toDouble(); },
            [scroller](double dt) { scroller->advance(dt); },
            [f, ends] {
                const auto [low, high] = ends();
                f->setProperty("contentY", (low + high) / 2);
            },
            ends,
            scroller->mapToScene(QPointF(scroller->width() / 2, scroller->height() / 2)),
        };
        Q_UNUSED(window);
    };

    // Each gesture through the grid, then through the view: the same
    // curve, within a pixel and a half, while neither is at an end.
    auto compare = [=](const Mover &view) {
        const QList<Gesture> gestures = scrolls();
        const auto [low, high] = view.ends();
        say(QStringLiteral("%1: %2 px to scroll, from the middle").arg(view.name).arg(high - low, 0, 'f', 0));
        for (qsizetype g = 0; g < gestures.size(); ++g) {
            const auto [curve, atEnd] = drive(window, view, gestures[g]);
            const std::vector<double> &grids = (*gridCurves)[g];
            double worst = 0, travel = 0;
            int compared = 0;
            for (int k = 0; k < kFrames && (atEnd < 0 || k < atEnd); ++k, ++compared) {
                worst = std::max(worst, std::abs(curve[size_t(k)] - grids[size_t(k)]));
                travel = std::max(travel, std::abs(grids[size_t(k)]));
            }
            check(worst <= 1.5 && compared >= 50,
                  QStringLiteral("%1, %2: as the grid within %3 px over %4 ms (the grid went %5 px)%6")
                      .arg(view.name, gestures[g].name)
                      .arg(worst, 0, 'f', 2)
                      .arg(compared * kFrame * 1000, 0, 'f', 0)
                      .arg(travel, 0, 'f', 0)
                      .arg(atEnd >= 0 ? QStringLiteral(", then at its end") : QString()));
        }
    };

    // The pinch through a Zoomer: the size drawn, as a share of where it
    // began.
    auto pinchCurve = [=](Zoomer *zoom, QPointF at) {
        std::vector<double> out;
        const Gesture g = pinch();
        clockBase += 10000;
        zoom->setLevel(zoom->level());
        const double start = zoom->shown();
        size_t next = 0;
        for (int k = 0; k < 200; ++k) {
            const double t = k * kFrame * 1000;
            while (next < g.inputs.size() && g.inputs[next].t <= t)
                deliver(window, g.inputs[next++], at, clockBase);
            zoom->advance(kFrame);
            out.push_back(zoom->shown() / start);
        }
        return out;
    };
    auto gridPinchCurve = [=] {
        std::vector<double> out;
        const Gesture g = pinch();
        clockBase += 10000;
        grid->setColumnsNow(PhotoGrid::columnsAt(2));
        const QPointF at = gridMover.at;
        const double start = grid->zoomStride();
        size_t next = 0;
        for (int k = 0; k < 200; ++k) {
            const double t = k * kFrame * 1000;
            while (next < g.inputs.size() && g.inputs[next].t <= t)
                deliver(window, g.inputs[next++], at, clockBase);
            grid->advance(kFrame);
            out.push_back(grid->zoomStride() / start);
        }
        // Back as it was.
        grid->setColumnsNow(PhotoGrid::columnsAt(2));
        return out;
    };
    // While the fingers are on it, the same size; let go, the same easing
    // to where each lands, as a share of the way.
    auto comparePinch = [=](const QString &name, const std::vector<double> &mine, const std::vector<double> &grids) {
        // The frames before the fingers lift (at 240 ms), and from the last
        // of them on.
        const int lifted = int(240 / (kFrame * 1000)) - 1;
        double worst = 0;
        for (int k = 0; k <= lifted; ++k)
            worst = std::max(worst, std::abs(mine[size_t(k)] - grids[size_t(k)]));
        check(worst < 0.002, QStringLiteral("%1, a pinch: the size under the fingers as the grid's, within %2 %")
                                 .arg(name)
                                 .arg(worst * 100, 0, 'f', 3));
        const auto share = [&](const std::vector<double> &c, int k) {
            const double from = std::log(c[size_t(lifted)]), to = std::log(c.back());
            return std::abs(to - from) < 1e-6 ? 1.0 : (std::log(c[size_t(k)]) - from) / (to - from);
        };
        double worstShare = 0;
        for (int k = lifted; k < int(mine.size()); ++k)
            worstShare = std::max(worstShare, std::abs(share(mine, k) - share(grids, k)));
        check(worstShare < 0.03, QStringLiteral("%1, let go: it lands as the grid does, the same easing within %2 % "
                                                "(lands at %3 times, the grid at %4)")
                                     .arg(name)
                                     .arg(worstShare * 100, 0, 'f', 2)
                                     .arg(mine.back(), 0, 'f', 3)
                                     .arg(grids.back(), 0, 'f', 3));
    };

    // A hard flick into the top, let go short of it, from 3000 px below it
    // (or the bottom): how far past the end the coast bounces, at most a
    // quarter of the view (`extent`); a frame of it at its furthest, named
    // `frame`, in `dir`.
    auto bounce = [=](const QString &name, const Mover &mover, const std::function<void(double)> &jump, double extent,
                      const QString &frame) {
        Gesture flick{"a hard flick", {{0, {}, {}, Qt::ScrollBegin}}};
        for (int i = 1; i <= 3; ++i)
            flick.inputs.push_back({i * 8.0, {0, 60}, {0, 480}, Qt::ScrollUpdate});
        flick.inputs.push_back({32, {}, {}, Qt::ScrollEnd});
        clockBase += 10000;
        const double low = mover.ends().first;
        jump(std::min(low + 3000, mover.ends().second));
        double furthest = 0;
        bool grabbed = false;
        size_t next = 0;
        for (int k = 0; k < kFrames; ++k) {
            const double t = k * kFrame * 1000;
            while (next < flick.inputs.size() && flick.inputs[next].t <= t)
                deliver(window, flick.inputs[next++], mover.at, clockBase);
            mover.advance(kFrame);
            const double past = low - mover.position();
            // Past its furthest: the frame before was it, near enough.
            if (!grabbed && furthest > 1 && past < furthest) {
                grabbed = true;
                if (!dir.isEmpty())
                    window->grabWindow().save(QStringLiteral("%1/%2.png").arg(dir, frame));
            }
            if (t > 32)
                furthest = std::max(furthest, past);
        }
        check(furthest <= 0.25 * extent + 2, QStringLiteral("%1, a hard flick into its top: %2 px past it at most, of a "
                                                           "view of %3 px (a quarter: %4)")
                                                 .arg(name)
                                                 .arg(furthest, 0, 'f', 0)
                                                 .arg(extent, 0, 'f', 0)
                                                 .arg(0.25 * extent, 0, 'f', 0));
    };

    // The steps: the grid's curves, then each plugin's screen.
    struct Step {
        QString what;
        std::function<void()> act;
        int wait;
    };
    auto steps = std::make_shared<QList<Step>>();
    auto plugin = [window](const char *id) {
        QQuickItem *loader = loaderOf(window->contentItem(), QString::fromLatin1(id));
        return loader ? loader->property("item").value<QQuickItem *>() : nullptr;
    };
    steps->append({"the grid", [=] {
                       grid->setColumnsNow(PhotoGrid::columnsAt(2));
                       for (const Gesture &g : scrolls()) {
                           gridCurves->append(drive(window, gridMover, g).first);
                           say(QStringLiteral("the grid, %1: %2 px in 1.6 s").arg(g.name).arg(gridCurves->last().back(), 0, 'f', 1));
                       }
                       *gridPinch = gridPinchCurve();
                       // Its top's thumbnails come, for the bounce's frame.
                       grid->jumpToFraction(0);
                   },
                   2000});
    steps->append({"the grid bounced", [=] {
                       bounce(QStringLiteral("the grid"), gridMover,
                              [grid](double at) { grid->jumpToFraction(at / std::max(1.0, grid->scrollRange())); },
                              grid->height(), QStringLiteral("grid-bounce"));
                       grid->jumpToFraction(0.5);
                   },
                   300});
    // A list of a few screens (On this day of a small library, the side
    // panel, the help) the grid's spin crosses in a moment, and a notch's
    // glide eases in short of an end: given room (4000 px of margin
    // before and after, then taken back), it is compared on the way, as
    // the grid is in its middle.
    auto compareWithRoom = [=](const QString &name, Scroller *scroller) {
        QQuickItem *f = scroller->flickable();
        const QVariant before = f->property("topMargin"), after = f->property("bottomMargin");
        f->setProperty("topMargin", before.toDouble() + 4000);
        f->setProperty("bottomMargin", after.toDouble() + 4000);
        compare(scrollerMover(name, scroller));
        f->setProperty("topMargin", before);
        f->setProperty("bottomMargin", after);
        f->setProperty("contentY", -before.toDouble());
    };
    auto view = [=](const QString &name, const char *id, int key) {
        steps->append({QStringLiteral("%1 opened").arg(name), [=] { press(window, key); }, 1500});
        steps->append({name, [=] {
                           QQuickItem *item = plugin(id);
                           Scroller *scroller = item ? largest<Scroller>(item) : nullptr;
                           check(scroller != nullptr, QStringLiteral("%1 shows a list that scrolls (a Scroller in it)").arg(name));
                           if (scroller)
                               compareWithRoom(name, scroller);
                       },
                       300});
    };
    // Pinhole's own: the side panel and the help, shown, their Scroller
    // given the same gestures, with room.
    auto own = [=](const QString &name, const char *objectName) {
        auto shown = [=](bool on) {
            // The help is made the first time it is asked for (Main.qml's
            // showHelp), as the window does.
            if (QLatin1String(objectName) == QLatin1String("help"))
                QMetaObject::invokeMethod(window, "showHelp", Q_ARG(QVariant, on));
            else if (QQuickItem *item = named(window->contentItem(), QString::fromLatin1(objectName)))
                item->setProperty("shown", on);
        };
        steps->append({QStringLiteral("%1 shown").arg(name), [=] { shown(true); }, 1200});
        steps->append({name, [=] {
                           QQuickItem *item = named(window->contentItem(), QString::fromLatin1(objectName));
                           Scroller *scroller = item ? largest<Scroller>(item) : nullptr;
                           check(scroller != nullptr, QStringLiteral("%1 scrolls (a Scroller over it)").arg(name));
                           QQuickItem *f = scroller ? scroller->flickable() : nullptr;
                           if (f)
                               compareWithRoom(name, scroller);
                           if (f && name == QLatin1String("the side panel")) {
                               const Mover mover = scrollerMover(name, scroller);
                               bounce(name, mover, [f](double at) { f->setProperty("contentY", at); }, f->height(),
                                      QStringLiteral("panel-bounce"));
                               f->setProperty("contentY", -f->property("topMargin").toDouble());
                           }
                           shown(false);
                       },
                       800});
    };
    own(QStringLiteral("the side panel"), "sidePanel");
    own(QStringLiteral("the help"), "help");
    view(QStringLiteral("the Year calendar"), "pinhole.calendar", Qt::Key_Y);
    steps->append({"the calendar closed", [=] { press(window, Qt::Key_Escape); }, 800});
    view(QStringLiteral("On this day"), "pinhole.onthisday", Qt::Key_D);
    steps->append({"On this day closed", [=] { press(window, Qt::Key_Escape); }, 800});
    view(QStringLiteral("Colours' photos of a colour"), "pinhole.colour", Qt::Key_H);
    steps->append({"Colours' wall", [=] {
                       if (QQuickItem *item = plugin("pinhole.colour"))
                           if (QQuickItem *finder = withProperty(item, "mode"))
                               finder->setProperty("mode", QStringLiteral("wall"));
                   },
                   1500});
    steps->append({"Colours' wall pinched", [=] {
                       QQuickItem *item = plugin("pinhole.colour");
                       Zoomer *zoom = item ? largest<Zoomer>(item) : nullptr;
                       check(zoom != nullptr, QStringLiteral("Colours' wall is in a PinchZoom"));
                       if (!zoom)
                           return;
                       const QPointF at = zoom->mapToScene(QPointF(zoom->width() / 2, zoom->height() / 2));
                       comparePinch(QStringLiteral("Colours' wall"), pinchCurve(zoom, at), *gridPinch);
                   },
                   300});
    steps->append({"Colours closed", [=] { press(window, Qt::Key_Escape); }, 800});

    auto run = std::make_shared<std::function<void(int)>>();
    *run = [=](int k) {
        if (k >= steps->size()) {
            say(QStringLiteral("done, %1 failed").arg(*failures));
            QCoreApplication::exit(*failures ? 1 : 0);
            return;
        }
        say(QStringLiteral("— %1").arg((*steps)[k].what));
        (*steps)[k].act();
        QTimer::singleShot((*steps)[k].wait, window, [=] { (*run)(k + 1); });
    };
    // Once the plugins are in and On this day has years to show.
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto wait = std::make_shared<QTimer>();
    wait->setInterval(250);
    QObject::connect(wait.get(), &QTimer::timeout, window, [=] {
        QObject *days = plugin("pinhole.onthisday");
        const bool ready = clock->elapsed() > 2500 && days && days->property("tourReady").toBool() && plugin("pinhole.colour")
                           && plugin("pinhole.calendar");
        if (!ready && clock->elapsed() < 60000)
            return;
        wait->stop();
        if (!ready) {
            say(QStringLiteral("FAIL On this day, Colours and the Calendar not all ready in 60 s (no photos on this day?)"));
            QCoreApplication::exit(1);
            return;
        }
        (*run)(0);
    });
    wait->start();
}

void runExamplesSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, const QString &dir) {
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    auto plugin = [window](const QString &id) {
        QQuickItem *loader = loaderOf(window->contentItem(), id);
        return loader ? loader->property("item").value<QQuickItem *>() : nullptr;
    };
    auto grab = [window, dir](const QString &name) {
        if (!dir.isEmpty())
            window->grabWindow().save(dir + '/' + name + ".png");
    };
    struct Step {
        QString what;
        std::function<void()> act;
        int wait;
    };
    auto steps = std::make_shared<QList<Step>>();
    auto add = [steps](const QString &what, std::function<void()> act, int wait = 1000) { steps->append({what, act, wait}); };
    // Each loaded, and over the photos on its key.
    auto open = [=](const QString &name, int key) {
        add(name + " (its F key)", [=] {
            check(plugin("example." + name) != nullptr, QStringLiteral("example.%1 loaded").arg(name));
            press(window, key);
        });
        add(name + " shows", [=] {
            QQuickItem *item = plugin("example." + name);
            check(item && item->property("covering").toBool(), QStringLiteral("example.%1 covers the photos").arg(name));
            grab(name);
        }, 300);
    };
    open(QStringLiteral("years"), Qt::Key_F5);
    add("years listed", [=] {
        QQuickItem *item = plugin(QStringLiteral("example.years"));
        Scroller *list = item ? largest<Scroller>(item) : nullptr;
        check(item && item->property("years").toList().size() > 0 && list,
              QStringLiteral("its years are listed (%1) in a ScrollList").arg(item ? item->property("years").toList().size() : 0));
        press(window, Qt::Key_Escape);
    }, 500);
    open(QStringLiteral("tiles"), Qt::Key_F6);
    add("tiles pinched out", [=] {
        QQuickItem *item = plugin(QStringLiteral("example.tiles"));
        Zoomer *zoom = item ? largest<Zoomer>(item) : nullptr;
        check(zoom && zoom->level() == 1, QStringLiteral("its tiles are 48 px"));
        if (zoom) {
            const Gesture g = pinch();
            clockBase += 10000;
            for (const Input &input : g.inputs)
                deliver(window, input, zoom->mapToScene(QPointF(zoom->width() / 2, zoom->height() / 2)), clockBase);
        }
    }, 800);
    add("tiles landed", [=] {
        QQuickItem *item = plugin(QStringLiteral("example.tiles"));
        Zoomer *zoom = item ? largest<Zoomer>(item) : nullptr;
        check(zoom && zoom->level() == 2 && !zoom->zooming() && std::abs(zoom->factor() - 1) < 1e-6,
              QStringLiteral("pinched out to 1.5 times, they land at 96 px (level %1)").arg(zoom ? zoom->level() : -1));
        grab(QStringLiteral("tiles-pinched"));
        press(window, Qt::Key_Escape);
    }, 500);
    // Under the pointer, a photo whose full size is here: its copy needs
    // no network.
    add("a photo here under the pointer", [=, &library] {
        const QList<int> here = findHere(library, QStringLiteral("full"), QStringLiteral("NOT a.is_video"), 1);
        check(!here.isEmpty(), QStringLiteral("a photo whose full size is here"));
        if (here.isEmpty())
            return;
        grid->reveal(here.first());
        QTimer::singleShot(700, window, [=] {
            const QPointF at = grid->mapToScene(grid->rectOf(here.first()).center());
            QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QGuiApplication::sendEvent(window, &move);
        });
    }, 1000);
    open(QStringLiteral("ask"), Qt::Key_F7);
    add("ask: 1 chosen", [=] { press(window, Qt::Key_1); }, 300);
    add("ask: the card gone", [=] {
        QQuickItem *item = plugin(QStringLiteral("example.ask"));
        check(item && !item->property("open").toBool(), QStringLiteral("1 chose the first size, and the card went"));
    }, 100);
    auto waited = std::make_shared<int>(0);
    add("ask: the copy made", [=] {
        const QStringList made = QDir(picturesLocation() + "/Pinhole").entryList({"*.png"}, QDir::Files);
        QQuickItem *item = plugin(QStringLiteral("example.ask"));
        QObject *host = item && qmlContext(item) ? qmlContext(item)->contextProperty("pinhole").value<QObject *>() : nullptr;
        check(made.size() == 1, QStringLiteral("a copy kept in Pictures/Pinhole: %1%2")
                                    .arg(made.join(", "), host && !host->property("problem").toString().isEmpty()
                                                              ? " (" + host->property("problem").toString() + ")" : QString()));
        check(clipboardNow() && !clipboardNow()->formats().isEmpty(),
              QStringLiteral("and put on the clipboard (%1)").arg(clipboardNow() ? clipboardNow()->formats().join(", ") : "none"));
    }, 300);
    open(QStringLiteral("places"), Qt::Key_F8);
    add("places asked", [=] {
        QQuickItem *item = plugin(QStringLiteral("example.places"));
        const QString report = item ? item->property("report").toString() : QString();
        check(report.contains(QLatin1String("photo")), QStringLiteral("the index answered: %1").arg(QString(report).replace('\n', "; ")));
        grab(QStringLiteral("places-answered"));
        press(window, Qt::Key_Escape);
    }, 500);

    auto run = std::make_shared<std::function<void(int)>>();
    *run = [=](int k) {
        if (k >= steps->size()) {
            say(QStringLiteral("done, %1 failed").arg(*failures));
            QCoreApplication::exit(*failures ? 1 : 0);
            return;
        }
        // The copy, made by magick, waited for up to 20 s.
        if ((*steps)[k].what == QLatin1String("ask: the copy made")
            && QDir(picturesLocation() + "/Pinhole").entryList({"*.png"}, QDir::Files).isEmpty() && ++*waited < 80) {
            QTimer::singleShot(250, window, [=] { (*run)(k); });
            return;
        }
        say(QStringLiteral("— %1").arg((*steps)[k].what));
        (*steps)[k].act();
        QTimer::singleShot((*steps)[k].wait, window, [=] { (*run)(k + 1); });
    };
    QTimer::singleShot(4000, window, [=] { (*run)(0); });
}
