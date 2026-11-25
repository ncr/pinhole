// PINHOLE_SELFTEST=through: nothing over the photos lets the pointer through
// to them (selftest.h).

#include "selftest.h"

#include "photogrid.h"
#include "scrollphysics.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QStyleHints>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

namespace {

// The touchpad's eventTime, which synthetic events carry.
quint64 eventTime = 5000000;

void say(const QString &line) {
    std::printf("selftest: %s\n", qPrintable(line));
    std::fflush(stdout);
}

void mouse(QQuickWindow *window, QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons held) {
    QMouseEvent event(type, at, window->mapToGlobal(at), button, held, Qt::NoModifier);
    event.setTimestamp(eventTime += 8);
    QGuiApplication::sendEvent(window, &event);
}

void click(QQuickWindow *window, QPointF at, Qt::MouseButton button = Qt::LeftButton) {
    mouse(window, QEvent::MouseButtonPress, at, button, button);
    mouse(window, QEvent::MouseButtonRelease, at, button, Qt::NoButton);
}

void doubleClick(QQuickWindow *window, QPointF at) {
    click(window, at);
    mouse(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
}

// The left button held at `at`, moved to `to` in ten steps and let go.
void pressDrag(QQuickWindow *window, QPointF at, QPointF to) {
    mouse(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 10; ++i)
        mouse(window, QEvent::MouseMove, at + (to - at) * (i / 10.0), Qt::NoButton, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

// A touchpad, as Qt names the one a scroll with phases or pixels alone
// comes from; a wheel's notches come from the mouse.
const QPointingDevice *touchpad() {
    static QPointingDevice *device = [] {
        auto *d = new QPointingDevice(QStringLiteral("selftest touchpad"), 4712, QInputDevice::DeviceType::TouchPad,
                                      QPointingDevice::PointerType::Finger,
                                      QInputDevice::Capability::Position | QInputDevice::Capability::Scroll
                                          | QInputDevice::Capability::PixelScroll,
                                      5, 0);
        QWindowSystemInterface::registerInputDevice(d);
        return d;
    }();
    return device;
}

void wheel(QQuickWindow *window, QPointF at, QPoint pixels, QPoint angle, Qt::ScrollPhase phase = Qt::NoScrollPhase,
           Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const bool pad = phase != Qt::NoScrollPhase || angle.isNull();
    QWheelEvent event(at, window->mapToGlobal(at), pixels, angle, Qt::NoButton, mods, phase, false, Qt::MouseEventNotSynthesized,
                      pad ? touchpad() : QPointingDevice::primaryPointingDevice());
    event.setTimestamp(eventTime += 8);
    QGuiApplication::sendEvent(window, &event);
}

// Two fingers on the touchpad, as Wayland reports them: a begin with no
// movement, `steps` updates `by` px 8 ms apart, an end; lifted while
// moving, so whatever follows them coasts.
void swipe(QQuickWindow *window, QPointF at, QPoint by, int steps) {
    eventTime += 1000;
    wheel(window, at, {}, {}, Qt::ScrollBegin);
    for (int i = 0; i < steps; ++i)
        wheel(window, at, by, by * 8, Qt::ScrollUpdate);
    wheel(window, at, {}, {}, Qt::ScrollEnd);
}

// As a Mac's touchpad goes on after the fingers lift: momentum, fading.
void momentum(QQuickWindow *window, QPointF at, QPoint by) {
    swipe(window, at, by, 6);
    for (int i = 0; i < 20; ++i)
        wheel(window, at, by * (20 - i) / 20, by * 8 * (20 - i) / 20, Qt::ScrollMomentum);
    wheel(window, at, {}, {}, Qt::ScrollEnd);
}

void pinch(QQuickWindow *window, QPointF at, double scale) {
    auto send = [&](Qt::NativeGestureType type, double value) {
        QNativeGestureEvent event(type, QPointingDevice::primaryPointingDevice(), 2, at, at, window->mapToGlobal(at),
                                  value, QPointF());
        QGuiApplication::sendEvent(window, &event);
    };
    send(Qt::BeginNativeGesture, 0);
    for (int i = 0; i < 10; ++i)
        send(Qt::ZoomNativeGesture, (scale - 1) / 10);
    send(Qt::EndNativeGesture, 0);
}

const QPointingDevice *touchScreen() {
    static QPointingDevice *device = [] {
        auto *d = new QPointingDevice(QStringLiteral("selftest touch screen"), 4711, QInputDevice::DeviceType::TouchScreen,
                                      QPointingDevice::PointerType::Finger,
                                      QInputDevice::Capability::Position | QInputDevice::Capability::Area, 10, 0);
        QWindowSystemInterface::registerInputDevice(d);
        return d;
    }();
    return device;
}

// One finger down at the first point, through the others, up at the last.
void touch(QQuickWindow *window, const QList<QPointF> &path) {
    for (qsizetype i = 0; i < path.size() + 1; ++i) {
        const QPointF at = path[std::min(i, path.size() - 1)];
        QWindowSystemInterface::TouchPoint p;
        p.id = 1;
        p.state = i == 0 ? QEventPoint::State::Pressed
                  : i == path.size() ? QEventPoint::State::Released : QEventPoint::State::Updated;
        const QPointF global = window->mapToGlobal(at);
        p.area = QRectF(global - QPointF(3, 3), QSizeF(6, 6));
        p.normalPosition = QPointF(at.x() / window->width(), at.y() / window->height());
        p.pressure = p.state == QEventPoint::State::Released ? 0 : 1;
        QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(window, eventTime += 16, touchScreen(),
                                                                                            {p});
    }
}

void key(QQuickWindow *window, int code) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, Qt::NoModifier);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, Qt::NoModifier);
}

// An item by its name, among all the window's: a Repeater's or a
// Loader's are not their QObject parent's children.
QQuickItem *findItem(QQuickItem *in, const QString &name) {
    if (in->objectName() == name)
        return in;
    for (QQuickItem *child : in->childItems())
        if (QQuickItem *found = findItem(child, name))
            return found;
    return nullptr;
}

// A plugin's item (its Main.qml), by the Loader Main.qml loads it in.
QObject *pluginItem(QQuickItem *in, const QString &id) {
    if (in->property("pluginId").toString() == id)
        return in->property("item").value<QObject *>();
    for (QQuickItem *child : in->childItems())
        if (QObject *found = pluginItem(child, id))
            return found;
    return nullptr;
}

// The deepest item drawn at a point, past those that only take input
// (a MouseArea, a Shield, a Scroller), which cover what they guard.
QQuickItem *deepest(QQuickItem *within, QPointF scene) {
    QQuickItem *item = within;
    for (;;) {
        QQuickItem *deeper = nullptr;
        const QList<QQuickItem *> children = item->childItems();
        for (auto it = children.crbegin(); it != children.crend() && !deeper; ++it) {
            QQuickItem *child = *it;
            const QString type = QString::fromLatin1(child->metaObject()->className());
            if (type.contains(QLatin1String("MouseArea")) || type.startsWith(QLatin1String("Shield"))
                || type.contains(QLatin1String("Scroller")))
                continue;
            if (child->isVisible() && child->opacity() > 0 && child->contains(child->mapFromScene(scene)))
                deeper = child;
        }
        if (!deeper)
            return item;
        item = deeper;
    }
}

// What is under a point, as a word or two for the log: the deepest item's
// type and its text, if it has one.
QString describe(QQuickItem *within, QPointF scene) {
    QQuickItem *item = deepest(within, scene);
    QString type = QString::fromLatin1(item->metaObject()->className());
    type = type.section(QStringLiteral("_QML"), 0, 0).remove(QStringLiteral("QQuick"));
    const QString text = item->property("text").toString().left(24);
    return text.isEmpty() ? type : QStringLiteral("%1 \"%2\"").arg(type, text);
}

// One kind of input, done at a point.
struct Kind {
    QString name;
    std::function<void(QQuickWindow *, QPointF)> act;
};

QList<Kind> scrolls() {
    return {
        {"a wheel notch down", [](QQuickWindow *w, QPointF at) { wheel(w, at, {0, -15}, {0, -120}); }},
        {"a wheel notch up", [](QQuickWindow *w, QPointF at) { wheel(w, at, {0, 15}, {0, 120}); }},
        {"a touchpad swipe down", [](QQuickWindow *w, QPointF at) { swipe(w, at, {0, -14}, 10); }},
        {"a touchpad swipe up", [](QQuickWindow *w, QPointF at) { swipe(w, at, {0, 14}, 10); }},
        {"a fast flick, and another while it coasts",
         [](QQuickWindow *w, QPointF at) {
             swipe(w, at, {0, -45}, 6);
             swipe(w, at, {0, -45}, 6);
         }},
        {"momentum after the fingers lift", [](QQuickWindow *w, QPointF at) { momentum(w, at, {0, 20}); }},
        {"smooth pixels without phases", [](QQuickWindow *w, QPointF at) { wheel(w, at, {0, -24}, {}); }},
        {"Ctrl and the wheel", [](QQuickWindow *w, QPointF at) { wheel(w, at, {0, 15}, {0, 120}, Qt::NoScrollPhase, Qt::ControlModifier); }},
        {"the sideways wheel", [](QQuickWindow *w, QPointF at) { wheel(w, at, {15, 0}, {120, 0}); }},
        {"a sideways swipe", [](QQuickWindow *w, QPointF at) { swipe(w, at, {-14, 0}, 10); }},
        {"a pinch out and in",
         [](QQuickWindow *w, QPointF at) {
             pinch(w, at, 2.2);
             pinch(w, at, 0.4);
         }},
        {"a two-finger tap", [](QQuickWindow *w, QPointF at) { click(w, at, Qt::RightButton); }},
        {"a middle click", [](QQuickWindow *w, QPointF at) { click(w, at, Qt::MiddleButton); }},
    };
}

// Clicks and taps that would press a control: only where none is.
QList<Kind> presses() {
    return {
        {"a click", [](QQuickWindow *w, QPointF at) { click(w, at); }},
        {"a double click", [](QQuickWindow *w, QPointF at) { doubleClick(w, at); }},
        {"a touch tap", [](QQuickWindow *w, QPointF at) { touch(w, {at}); }},
    };
}

} // namespace

void runThroughSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const QString &dir) {
    // What a single click does waits for a second no longer than this: each
    // click tried waits through it.
    QGuiApplication::styleHints()->setMouseDoubleClickInterval(200);
    Viewer *view = &viewer;
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    auto item = [window](const char *name) { return findItem(window->contentItem(), QString::fromLatin1(name)); };

    // Everything the grid does for the pointer, heard as it happens.
    auto heard = std::make_shared<QStringList>();
    auto hits = std::make_shared<int>(0);
    auto hear = [heard, hits](const char *what) {
        return [heard, hits, what] {
            ++*hits;
            if (!heard->contains(QString::fromLatin1(what)))
                heard->append(QString::fromLatin1(what));
        };
    };
    QObject::connect(grid, &PhotoGrid::scrolled, window, hear("scrolled"));
    QObject::connect(grid, &PhotoGrid::selectionChanged, window, hear("selected"));
    QObject::connect(grid, &PhotoGrid::selectingChanged, window, hear("select mode"));
    QObject::connect(grid, &PhotoGrid::activated, window, hear("opened a photo"));
    QObject::connect(grid, &PhotoGrid::columnsChanged, window, hear("zoomed"));
    QObject::connect(grid, &PhotoGrid::zoomingChanged, window, hear("zoomed"));
    QObject::connect(grid, &PhotoGrid::dragRequested, window, hear("dragged photos out"));
    QObject::connect(view, &Viewer::openChanged, window, hear("the viewer"));
    // The grid laid out again (a plugin's stacks or hidden photos coming
    // in, Series' and Screenshots' found after the start): it keeps the
    // photo in the middle where it was, so the first photo moves and the
    // scroll fraction changes with no input at all.
    auto relaid = std::make_shared<int>(0);
    auto quietSince = std::make_shared<QElapsedTimer>();
    quietSince->start();
    QObject::connect(grid, &PhotoGrid::cellsChanged, window, [relaid, quietSince] {
        ++*relaid;
        quietSince->restart();
    });
    QObject::connect(grid, &PhotoGrid::stacksChanged, window, [relaid, quietSince] {
        ++*relaid;
        quietSince->restart();
    });
    auto top = std::make_shared<double>(0);
    // A step not done yet (a probe tried again): the next waits for it.
    auto busy = std::make_shared<bool>(false);
    auto still = [grid, top, heard] {
        heard->clear();
        *top = grid->rectOf(0).top();
    };
    // Whatever reached the grid since `still`, by its signals and by where
    // its first photo is.
    auto untouched = [grid, top, heard] {
        QStringList all = *heard;
        if (std::abs(grid->rectOf(0).top() - *top) > 0.01 && !all.contains("scrolled"))
            all << "moved";
        return all;
    };

    // The steps, each run after the one before has settled.
    struct Step {
        QString what;
        std::function<void()> act;
        // Before the next: what it set off, once begun, is waited for as
        // the grid settles (run, below).
        int wait = 60;
    };
    auto steps = std::make_shared<QList<Step>>();
    // Longer before the next, once, after undoing what got through.
    auto extraWait = std::make_shared<int>(0);
    // A part of it alone if PINHOLE_SELFTEST_PART says it ("panel": the side
    // panel; "rest": the rest), each in a window of its own, at once.
    const QString part = qEnvironmentVariable("PINHOLE_SELFTEST_PART");
    const bool panelPart = part.isEmpty() || part == QLatin1String("panel");
    const bool restPart = part.isEmpty() || part == QLatin1String("rest");
    auto keeping = std::make_shared<bool>(true);
    auto add = [steps, keeping](const QString &what, std::function<void()> act, int wait = 60) {
        if (*keeping)
            steps->append({what, act, wait});
    };

    // Each kind of input at each point: none reaches the grid, and the
    // first point of each that does is named. Laid out again meanwhile by
    // a plugin (`relaid`), with nothing reaching it at any point, it is
    // tried once more: that moved it, not the input.
    auto probe = [=](const QString &over, std::function<QList<QPointF>()> points, QList<Kind> kinds,
                              std::function<QQuickItem *(QQuickWindow *)> within) {
        for (const Kind &kind : kinds) {
            auto attempt = std::make_shared<std::function<void(int)>>();
            *attempt = [=](int tries) {
                // The next step waits for this one's verdict.
                *busy = true;
                if (grid->animating())
                    grid->jumpToFraction(grid->scrollFraction());
                still();
                const int relaidBefore = *relaid;
                QString first;
                int reached = 0;
                const QList<QPointF> at = points();
                for (const QPointF &p : at) {
                    const int before = *hits;
                    const bool moving = grid->animating();
                    kind.act(window, p);
                    if ((*hits != before || grid->animating() != moving) && ++reached == 1)
                        first = QStringLiteral(" first at %1,%2 (%3)").arg(qRound(p.x())).arg(qRound(p.y()))
                                    .arg(describe(within ? within(window) : window->contentItem(), p));
                }
                // Long enough for what it set off to show: a few frames, or
                // for a click, past the time a second one would make it a
                // double click (what a single one does waits that long).
                const bool clicked = kind.name.contains(QLatin1String("click")) || kind.name.contains(QLatin1String("tap"));
                const int settle = clicked ? QGuiApplication::styleHints()->mouseDoubleClickInterval() + 60 : 120;
                QTimer::singleShot(settle, window, [=] {
                    const QStringList got = untouched();
                    const bool layout = *relaid != relaidBefore && reached == 0
                                        && QStringList(got).filter(QRegularExpression("^(scrolled|moved)$")).size() == got.size();
                    if (!got.isEmpty() && layout && tries < 2) {
                        say(QStringLiteral("the grid was laid out again during %1 over %2, and nothing reached it: once more")
                                .arg(kind.name, over));
                        *busy = true;
                        QTimer::singleShot(300, window, [=] { (*attempt)(tries + 1); });
                        return;
                    }
                    *busy = false;
                    check(got.isEmpty() && !at.isEmpty(),
                          QStringLiteral("%1 over %2, at %3 points: %4%5").arg(kind.name, over).arg(at.size())
                              .arg(got.isEmpty() ? QStringLiteral("the grid took none")
                                                 : QStringLiteral("the grid %1, %2 times").arg(got.join(", ")).arg(reached),
                                   first));
                    // What went through undone, for the next.
                    if (got.contains(QLatin1String("opened a photo")) && view->isOpen()) {
                        view->close();
                        *extraWait = 1000;
                    }
                });
            };
            add(QStringLiteral("%1 over %2").arg(kind.name, over), [=] { (*attempt)(0); }, 0);
        }
    };

    auto panelItem = [](QQuickWindow *w) {
        QQuickItem *scroller = w->findChild<QQuickItem *>(QStringLiteral("sidePanelScroller"));
        return scroller ? scroller->parentItem() : nullptr;
    };
    auto panelRect = [window, panelItem] {
        QQuickItem *panel = panelItem(window);
        return panel ? QRectF(panel->mapToScene(QPointF(0, 0)), panel->size()) : QRectF();
    };
    // Over all of it: a lattice 36 px apart, in from each edge by 2 px.
    auto lattice = [](QRectF r) {
        QList<QPointF> points;
        for (double y = r.top() + 2; y < r.bottom() - 1; y += 36)
            for (double x : {r.left() + 2, r.left() + r.width() * 0.25, r.center().x(), r.left() + r.width() * 0.75,
                             r.right() - 2})
                points << QPointF(x, y);
        return points;
    };
    auto panelEverywhere = [=] { return lattice(panelRect()); };
    // Where nothing is pressed: the margins, and the gaps between the
    // rows (the panel itself, or its column, under the point).
    auto panelBare = [=] {
        QList<QPointF> bare;
        QQuickItem *panel = panelItem(window);
        QQuickItem *flick = panel->findChild<QQuickItem *>(QStringLiteral("sidePanelScroller"));
        QQuickItem *content = flick->property("contentItem").value<QQuickItem *>();
        const QList<QQuickItem *> plain = {panel, flick, content, content->childItems().value(0)};
        for (const QPointF &p : lattice(panelRect()))
            if (plain.contains(deepest(panel, p)))
                bare << p;
        return bare;
    };
    auto beside = [=] {
        const QRectF r = panelRect();
        return QList<QPointF>{QPointF(r.right() + 30, r.top() + 60), QPointF(window->width() - 40.0, r.center().y()),
                              QPointF(r.right() + 30, r.bottom() - 40)};
    };
    auto scroller = [window] { return window->findChild<QQuickItem *>(QStringLiteral("sidePanelScroller")); };
    auto contentY = [scroller] { return scroller()->property("contentY").toDouble(); };
    auto most = [scroller] {
        return std::max(0.0, scroller()->property("contentHeight").toDouble() - scroller()->height());
    };

    auto cardOf = [](const char *name) {
        return [name](QQuickWindow *w) { return findItem(w->contentItem(), QString::fromLatin1(name)); };
    };
    auto rectOf = [window](const char *name) {
        QQuickItem *it = findItem(window->contentItem(), QString::fromLatin1(name));
        return it && it->isVisible() ? QRectF(it->mapToScene(QPointF(0, 0)), it->size()) : QRectF();
    };
    auto inside = [](QRectF r) {
        if (r.isEmpty())
            return QList<QPointF>();
        return QList<QPointF>{r.center(), r.topLeft() + QPointF(3, 3), r.bottomRight() - QPointF(3, 3),
                              QPointF(r.left() + 3, r.bottom() - 3), QPointF(r.right() - 3, r.top() + 3)};
    };

    // The date at the top left, and the grid in the middle of the library.
    auto settleGrid = [=] {
        key(window, Qt::Key_Escape);
        grid->setSelecting(false);
        grid->jumpToFraction(0.5);
    };

    // One width alone if PINHOLE_SELFTEST_WIDE says it (test-selftests runs
    // each in a window of its own, at once).
    QList<QSize> sizes{QSize(850, 900), QSize(1700, 1000)};
    if (const int only = qEnvironmentVariableIntValue("PINHOLE_SELFTEST_WIDE"); only > 0)
        sizes.removeIf([only](const QSize &s) { return s.width() != only; });
    for (const QSize size : sizes) {
        const QString wide = QStringLiteral("%1 wide").arg(size.width());
        *keeping = true;
        add(QStringLiteral("the window %1, the grid half way").arg(wide), [=] {
            window->resize(size);
            settleGrid();
        }, 1500);
        *keeping = restPart;
        add(QStringLiteral("the date shows, %1").arg(wide), [=] {
            check(!rectOf("dateLabel").isEmpty(), QStringLiteral("the date shows at the top left (%1)").arg(grid->property("dateLabel").toString()));
        }, 100);
        auto date = [=] { return inside(rectOf("dateLabel")); };
        // The date now opens the order menu. Close it after each isolated
        // pointer probe so the next probe still targets the date itself.
        auto datePresses = presses();
        for (auto &kind : datePresses) {
            const auto press = kind.act;
            kind.act = [=](QQuickWindow *w, QPointF at) {
                press(w, at);
                if (QObject *menu = w->findChild<QObject *>("orderMenu"))
                    menu->setProperty("open", false);
            };
        }
        probe(QStringLiteral("the date, ") + wide, date, scrolls() + datePresses, nullptr);
        probe(QStringLiteral("the date, ") + wide, date,
              {{"a touch drag down onto the photos", [](QQuickWindow *w, QPointF at) {
                    touch(w, {at, at + QPointF(4, 40), at + QPointF(8, 160), at + QPointF(10, 300)});
                }},
               {"a press dragged onto the photos",
                [](QQuickWindow *w, QPointF at) { pressDrag(w, at, at + QPointF(10, 300)); }}},
              nullptr);
        add(QStringLiteral("select mode, %1").arg(wide), [=] { grid->setSelecting(true); }, 300);
        probe(QStringLiteral("the date in select mode, ") + wide, date, datePresses, nullptr);
        add(QStringLiteral("select mode left, %1").arg(wide), [=] { grid->setSelecting(false); }, 300);

        *keeping = panelPart;
        // The side panel by its button: it opens, and the photo under the
        // button does not.
        add(QStringLiteral("the side panel's button, %1").arg(wide), [=] {
            still();
            click(window, rectOf("sourcesButton").center());
            QTimer::singleShot(600, window, [=] {
                const QStringList got = untouched();
                check(item("sidePanel")->property("shown").toBool() && got.isEmpty(),
                      QStringLiteral("a click on the side panel's button opens it, and the grid took %1")
                          .arg(got.isEmpty() ? QStringLiteral("nothing") : got.join(", ")));
            });
        }, 900);
        // Everything over all of it, at the top, in the middle and at the end.
        probe(QStringLiteral("the side panel, ") + wide, panelEverywhere, scrolls(), panelItem);
        probe(QStringLiteral("the side panel's margins and gaps, ") + wide, panelBare, presses(), panelItem);
        // What scrolls the panel lies over it all, and a click still
        // reaches its controls: a switch clicked turns, and back.
        // The switches are on the Photos tab.
        add(QStringLiteral("a switch in the panel, %1").arg(wide), [=] {
            item("sidePanel")->setProperty("section", QStringLiteral("photos"));
            scroller()->setProperty("contentY", 0);
        }, 300);
        add(QStringLiteral("a switch in the panel clicked, %1").arg(wide), [=] {
            QQuickItem *toggle = item("markSwitch:pinhole/video");
            if (!toggle) {
                check(false, QStringLiteral("the Videos switch is in the panel"));
                return;
            }
            const QPointF at = toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2));
            const bool was = toggle->property("checked").toBool();
            click(window, at);
            const bool turned = toggle->property("checked").toBool();
            click(window, at);
            const bool back = toggle->property("checked").toBool();
            check(turned != was && back == was,
                  QStringLiteral("a click on the Videos switch in the panel turns it %1 and the next back").arg(turned ? "on" : "off"));
        }, 300);
        // A click or a touch tap on a switch while the panel coasts stops
        // the panel there, and that is all: the switch does not turn. On
        // the Plugins tab, which is long enough to coast (the Photos tab
        // fits the window): a plugin's switch, of one that brings no
        // sources, in the middle half of the panel when it is clicked.
        auto switchInView = [=]() -> QQuickItem * {
            const QRectF r = panelRect();
            const QRectF middle(r.left(), r.top() + r.height() / 4, r.width(), r.height() / 2);
            for (const char *id : {"pinhole.calendar", "pinhole.favourites", "pinhole.frame",
                                   "pinhole.mosaic",
                                   "pinhole.series", "pinhole.sheet", "pinhole.theme",
                                   "pinhole.wall"}) {
                const QByteArray name = QByteArray("pluginSwitch:") + id;
                QQuickItem *toggle = item(name.constData());
                if (toggle && toggle->isVisible()
                    && middle.contains(toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2))))
                    return toggle;
            }
            return nullptr;
        };
        add(QStringLiteral("the Plugins tab, %1").arg(wide), [=] {
            item("sidePanel")->setProperty("section", QStringLiteral("plugins"));
            scroller()->setProperty("contentY", 0);
        }, 500);
        for (const bool byTouch : {false, true}) {
            const QString how = byTouch ? QStringLiteral("a touch tap") : QStringLiteral("a click");
            add(QStringLiteral("%1 on a switch while the panel coasts, %2").arg(how, wide), [=] {
                scroller()->setProperty("contentY", 0);
                still();
                swipe(window, panelRect().center(), {0, -5}, 4);
                const double lifted = contentY();
                QTimer::singleShot(60, window, [=] {
                    QQuickItem *toggle = switchInView();
                    if (!toggle) {
                        check(false, QStringLiteral("a plugin's switch in the middle of the panel (at %1 of %2)")
                                         .arg(qRound(contentY())).arg(qRound(most())));
                        return;
                    }
                    const QPointF at = toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2));
                    const bool was = toggle->property("checked").toBool();
                    const double y = contentY();
                    const bool coasting = y > lifted + 0.5;
                    if (byTouch)
                        touch(window, {at});
                    else
                        click(window, at);
                    const bool turned = toggle->property("checked").toBool() != was;
                    QTimer::singleShot(300, window, [=] {
                        const double on = contentY() - y;
                        check(coasting && panelRect().contains(at) && !turned && std::abs(on) < 1 && untouched().isEmpty(),
                              QStringLiteral("%1 on %2 while the panel coasts (%3 px from %4, at %5 of %6) stops it "
                                             "(%7 px on after) and turns nothing (the switch %8), the grid still")
                                  .arg(how, toggle->objectName()).arg(qRound(y - lifted)).arg(qRound(lifted)).arg(qRound(y))
                                  .arg(qRound(most())).arg(qRound(on))
                                  .arg(turned ? QStringLiteral("TURNED") : QStringLiteral("as it was")));
                        // Put back, for what follows.
                        if (turned)
                            click(window, toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)));
                    });
                });
            }, 900);
        }
        // Then, at rest, a switch answers the first click.
        add(QStringLiteral("a switch at rest after a coast, %1").arg(wide), [=] {
            QQuickItem *toggle = switchInView();
            if (!toggle) {
                check(false, QStringLiteral("a plugin's switch in the middle of the panel at rest"));
                return;
            }
            const QPointF at = toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2));
            const bool was = toggle->property("checked").toBool();
            click(window, at);
            const bool turned = toggle->property("checked").toBool() != was;
            if (turned)
                click(window, at);
            check(turned && toggle->property("checked").toBool() == was,
                  QStringLiteral("at rest, a click on %1 turns it, and another back").arg(toggle->objectName()));
        }, 600);
        // A plugin's settings with scrollables of their own (the test
        // plugin test.scrolls, app/tests/scrolls): the wheel and the
        // touchpad over one move it while it can go their way, the panel
        // not; at its end, the panel.
        auto nested = [window](const char *name) { return findItem(window->contentItem(), QString::fromLatin1(name)); };
        auto nestedY = [nested](const char *name) {
            QQuickItem *f = nested(name);
            return f ? f->property("contentY").toDouble() : -1.0;
        };
        auto nestedMost = [nested](const char *name) {
            QQuickItem *f = nested(name);
            return f ? std::max(0.0, f->property("contentHeight").toDouble() - f->height()) : -1.0;
        };
        auto centreOf = [nested](const char *name) {
            QQuickItem *f = nested(name);
            return f ? f->mapToScene(QPointF(f->width() / 2, f->height() / 2)) : QPointF();
        };
        add(QStringLiteral("a plugin's settings with a list and a text, %1").arg(wide), [=] {
            QObject *side = item("sidePanel");
            QMetaObject::invokeMethod(side, "showPlugin", Q_ARG(QVariant, QStringLiteral("test.scrolls")));
        }, 900);
        add(QStringLiteral("the plugin's list in view, %1").arg(wide), [=] {
            QQuickItem *list = nested("testNestedList");
            QQuickItem *text = nested("testNestedTextView");
            check(list && text, QStringLiteral("the test plugin test.scrolls shows its settings in the panel (its list %1, its text %2)")
                                    .arg(list ? "there" : "MISSING", text ? "there" : "MISSING"));
            if (!list || !text)
                return;
            // The two in the middle of the panel.
            const double inPanel = list->mapToItem(scroller()->property("contentItem").value<QQuickItem *>(), QPointF(0, 0)).y();
            scroller()->setProperty("contentY", std::max(0.0, inPanel - scroller()->height() / 3));
            list->setProperty("contentY", 0);
        }, 400);
        for (const char *name : {"testNestedList", "testNestedTextView"}) {
            const QString what = QString::fromLatin1(name) == QLatin1String("testNestedList") ? QStringLiteral("list")
                                                                                              : QStringLiteral("text");
            add(QStringLiteral("a notch over the plugin's %1, %2").arg(what, wide), [=] {
                still();
                QQuickItem *f = nested(name);
                if (!f)
                    return;
                f->setProperty("contentY", 0);
                const double panel = contentY();
                wheel(window, centreOf(name), {0, -15}, {0, -120});
                QTimer::singleShot(700, window, [=] {
                    check(nestedY(name) > 5 && std::abs(contentY() - panel) < 0.5 && untouched().isEmpty(),
                          QStringLiteral("a notch over the plugin's %1 moves it (%2 px), not the panel (%3 px), the grid still")
                              .arg(what).arg(qRound(nestedY(name))).arg(qRound(contentY() - panel)));
                });
            }, 900);
            add(QStringLiteral("a swipe over the plugin's %1, %2").arg(what, wide), [=] {
                still();
                QQuickItem *f = nested(name);
                if (!f)
                    return;
                f->setProperty("contentY", 0);
                const double panel = contentY();
                swipe(window, centreOf(name), {0, -6}, 6);
                QTimer::singleShot(900, window, [=] {
                    check(nestedY(name) > 20 && std::abs(contentY() - panel) < 0.5 && untouched().isEmpty(),
                          QStringLiteral("a swipe over the plugin's %1 moves it (%2 px), not the panel (%3 px), the grid still")
                              .arg(what).arg(qRound(nestedY(name))).arg(qRound(contentY() - panel)));
                });
            }, 1100);
            add(QStringLiteral("a notch over the plugin's %1 at its end, %2").arg(what, wide), [=] {
                still();
                QQuickItem *f = nested(name);
                if (!f)
                    return;
                f->setProperty("contentY", nestedMost(name));
                // The panel where it can still go down (the plugin has room
                // below its two): the step before may have left it at its
                // end, where no notch moves it. And the plugin's a third of
                // the way down it, still under the pointer once the notch
                // has moved the panel, for the notch back.
                const double top = f->mapToItem(scroller()->property("contentItem").value<QQuickItem *>(), QPointF(0, 0)).y();
                scroller()->setProperty("contentY", std::clamp(top - scroller()->height() / 3, 0.0, most()));
                const double panel = contentY();
                wheel(window, centreOf(name), {0, -15}, {0, -120});
                QTimer::singleShot(700, window, [=] {
                    check(std::abs(nestedY(name) - nestedMost(name)) < 0.5 && contentY() > panel + 5 && untouched().isEmpty(),
                          QStringLiteral("at the end of the plugin's %1 (%2 of %3), a notch over it moves the panel (%4 px, from %5 of %6), "
                                         "the grid still")
                              .arg(what).arg(qRound(nestedY(name))).arg(qRound(nestedMost(name))).arg(qRound(contentY() - panel))
                              .arg(qRound(panel)).arg(qRound(most())));
                });
            }, 900);
            add(QStringLiteral("a notch back over the plugin's %1 at its end, %2").arg(what, wide), [=] {
                still();
                const double panel = contentY(), was = nestedY(name);
                wheel(window, centreOf(name), {0, 15}, {0, 120});
                QTimer::singleShot(700, window, [=] {
                    check(nestedY(name) < was - 5 && std::abs(contentY() - panel) < 0.5 && untouched().isEmpty(),
                          QStringLiteral("a notch back over the plugin's %1 at its end moves it back (%2 px), not the panel, the grid still")
                              .arg(what).arg(qRound(nestedY(name) - was)));
                });
            }, 900);
        }
        // Folded again: what follows is of the panel's own rows (a touch
        // drag on the list would drag the list).
        add(QStringLiteral("the plugin's settings folded, the panel at its top, %1").arg(wide), [=] {
            QMetaObject::invokeMethod(item("sidePanel"), "unfold", Q_ARG(QVariant, QStringLiteral("test.scrolls")), Q_ARG(QVariant, false));
            scroller()->setProperty("contentY", 0);
        }, 500);
        // A notch glides: under way a frame or two later, there at once
        // after; it is the panel that moved.
        add(QStringLiteral("a notch moves the panel, %1").arg(wide), [=] {
            still();
            const QPointF at = panelRect().center();
            wheel(window, at, {0, -15}, {0, -120});
            QTimer::singleShot(40, window, [=] {
                const double soon = contentY();
                QTimer::singleShot(600, window, [=] {
                    const double later = contentY();
                    check(later > 30 && soon < later && untouched().isEmpty(),
                          QStringLiteral("a notch glides the panel down %1 px (%2 after 40 ms), the grid still").arg(qRound(later)).arg(qRound(soon)));
                });
            });
        }, 900);
        add(QStringLiteral("a touchpad swipe moves the panel and coasts, %1").arg(wide), [=] {
            scroller()->setProperty("contentY", 0);
            still();
            const QPointF at = panelRect().center();
            swipe(window, at, {0, -12}, 8);
            const double lifted = contentY();
            QTimer::singleShot(120, window, [=] {
                const double coasting = contentY();
                QTimer::singleShot(1500, window, [=] {
                    // On, or to its end and back there.
                    const bool on = contentY() >= coasting || std::abs(contentY() - most()) < 0.5;
                    check(lifted > 20 && coasting > lifted && on && untouched().isEmpty(),
                          QStringLiteral("a swipe moves the panel %1 px with the fingers, it coasts on to %2 and %3 (its end %4), "
                                         "the grid still")
                              .arg(qRound(lifted)).arg(qRound(coasting)).arg(qRound(contentY())).arg(qRound(most())));
                });
            });
        }, 1900);
        // To its end and on: it stops there, and the rest goes nowhere.
        for (const bool down : {true, false}) {
            const QString end = down ? QStringLiteral("end") : QStringLiteral("top");
            add(QStringLiteral("to its %1 and on, by the wheel, %2").arg(end, wide), [=] {
                still();
                const QPointF at = panelRect().center();
                for (int i = 0; i < 60; ++i)
                    wheel(window, at, {0, down ? -15 : 15}, {0, down ? -120 : 120});
                QTimer::singleShot(1200, window, [=] {
                    const double y = contentY(), want = down ? most() : 0;
                    check(std::abs(y - want) < 0.5 && untouched().isEmpty(),
                          QStringLiteral("the wheel spun on past the panel's %1 leaves it there (%2 of %3), the grid still")
                              .arg(end).arg(qRound(y)).arg(qRound(most())));
                    if (!dir.isEmpty())
                        window->grabWindow().save(QStringLiteral("%1/panel-%2-%3.png").arg(dir).arg(size.width()).arg(end));
                });
            }, 1500);
            // Fingers pulling on past the end, not yet lifted: how far it
            // gives, as a frame.
            if (!dir.isEmpty())
                add(QStringLiteral("pulled past its %1, %2").arg(end, wide), [=] {
                    const QPointF at = panelRect().center();
                    eventTime += 1000;
                    wheel(window, at, {}, {}, Qt::ScrollBegin);
                    for (int i = 0; i < 30; ++i)
                        wheel(window, at, {0, down ? -20 : 20}, {0, down ? -160 : 160}, Qt::ScrollUpdate);
                    QTimer::singleShot(100, window, [=] {
                        window->grabWindow().save(QStringLiteral("%1/panel-%2-%3-pulled.png").arg(dir).arg(size.width()).arg(end));
                        say(QStringLiteral("pulled on past the panel's %1: %2 px past it").arg(end).arg(qRound(down ? contentY() - most() : -contentY())));
                        eventTime += 600;
                        wheel(window, at, {}, {}, Qt::ScrollEnd);
                    });
                }, 1200);
            probe(QStringLiteral("the side panel at its %1, %2").arg(end, wide), panelEverywhere, scrolls(), panelItem);
            // Flung at it from the other end, the fingers going on past it
            // (the rubber band gives less than the panel's height, and the
            // spring carries the most speed, 40000 px/s, some 1240 px on),
            // or let go short of it to coast there and bounce as the grid
            // does (more than a few lines): back at the end, the grid still.
            for (const bool far : {true, false}) {
                add(QStringLiteral("%1 into its %2, %3").arg(far ? "flicks fast" : "a coast", end, wide), [=] {
                    // Short of it by 700 px, or as far as the panel goes.
                    const double start = far ? (down ? 0 : most()) : std::clamp(down ? most() - 700 : 700, 0.0, most());
                    scroller()->setProperty("contentY", start);
                    still();
                    const QPointF at = panelRect().center();
                    if (far) {
                        for (int i = 0; i < 4; ++i)
                            swipe(window, at, {0, down ? -50 : 50}, 6);
                    } else {
                        swipe(window, at, {0, down ? -14 : 14}, 6);
                    }
                    const double lifted = contentY();
                    auto beyond = std::make_shared<double>(0);
                    auto watch = std::make_shared<QTimer>();
                    watch->setInterval(8);
                    QObject::connect(watch.get(), &QTimer::timeout, window, [=] {
                        const double y = contentY();
                        *beyond = std::max(*beyond, down ? y - most() : -y);
                    });
                    watch->start();
                    QTimer::singleShot(2500, window, [=] {
                        watch->stop();
                        const double y = contentY(), want = down ? most() : 0;
                        const ScrollPhysics::Params grid;
                        const double spring = grid.maxVelocity * grid.springSeconds / (5 * M_E);
                        const double allowed = far ? scroller()->height() + spring : spring;
                        check(std::abs(y - want) < 0.5 && *beyond <= allowed && (far || (std::abs(lifted - want) > 50 && *beyond > 60))
                                  && untouched().isEmpty(),
                              QStringLiteral("%1 into the panel's %2 (let go at %3) stops there (%4 of %5), %6 px past it at most "
                                             "(%7 allowed), the grid still")
                                  .arg(far ? "flicks fast" : "a coast", end).arg(qRound(lifted)).arg(qRound(y))
                                  .arg(qRound(most())).arg(qRound(*beyond)).arg(qRound(allowed)));
                    });
                }, 2800);
            }
            // A finger on a touch screen: on past the end it stays there;
            // back from it the panel follows.
            for (const bool on : {true, false}) {
                add(QStringLiteral("a touch drag on the panel, %1").arg(wide), [=] {
                    still();
                    const QRectF r = panelRect();
                    const double from = contentY();
                    const QPointF a(r.left() + r.width() * 0.3, r.center().y());
                    const QPointF b(a.x(), a.y() + ((down == on) ? 1 : -1) * 260);
                    touch(window, {b, b + (a - b) * 0.25, b + (a - b) * 0.5, b + (a - b) * 0.75, a});
                    QTimer::singleShot(900, window, [=] {
                        const double moved = std::abs(contentY() - from);
                        check(untouched().isEmpty() && (on ? moved < 0.5 : moved > 100),
                              QStringLiteral("a touch drag %1 the panel's %2 (from %3 to %4): the grid still")
                                  .arg(on ? "on past" : "back from", end).arg(qRound(from)).arg(qRound(contentY())));
                    });
                }, 1100);
            }
        }
        probe(QStringLiteral("the photos dimmed beside the panel, ") + wide, beside, scrolls(), nullptr);
        add(QStringLiteral("a click beside the panel, %1").arg(wide), [=] {
            still();
            click(window, beside().first());
            QTimer::singleShot(600, window, [=] {
                const QStringList got = untouched();
                check(!item("sidePanel")->property("shown").toBool() && got.isEmpty(),
                      QStringLiteral("a click beside the panel puts it away, and the grid took %1")
                          .arg(got.isEmpty() ? QStringLiteral("nothing") : got.join(", ")));
            });
        }, 900);

        *keeping = restPart;
        // The other panels over the photos.
        struct Over {
            QString name;
            const char *card;
            std::function<void()> show;
        };
        const QList<Over> overs = {
            // Made the first time it is asked for (Main.qml's showHelp).
            {"the help", "helpCard", [=] { QMetaObject::invokeMethod(window, "showHelp", Q_ARG(QVariant, true)); }},
            {"iCloud's sign-in", "signInCard",
             [=] {
                 if (QObject *icloud = pluginItem(window->contentItem(), QStringLiteral("pinhole.icloud")))
                     QMetaObject::invokeMethod(icloud, "openSignIn", Q_ARG(QVariant, QString()));
             }},
            {"select mode's Copy menu", "selectCopyGroupCard",
             [=] {
                 grid->setSelecting(true);
                 item("selectCopyGroup")->setProperty("open", true);
             }},
        };
        for (const Over &over : overs) {
            const char *card = over.card;
            add(QStringLiteral("%1, %2").arg(over.name, wide), [=] {
                settleGrid();
                over.show();
            }, 900);
            add(QStringLiteral("%1 shows, %2").arg(over.name, wide), [=] {
                check(!rectOf(card).isEmpty(), QStringLiteral("%1 shows").arg(over.name));
            }, 50);
            probe(over.name + ", " + wide, [=] { return lattice(rectOf(card)); }, scrolls(), cardOf(card));
            probe(QStringLiteral("beside %1, %2").arg(over.name, wide), [=] {
                const QRectF r = rectOf(card);
                return QList<QPointF>{QPointF(r.left() / 2, window->height() * 0.6), QPointF(window->width() - 20.0, window->height() * 0.6)};
            }, scrolls(), nullptr);
            add(QStringLiteral("a click beside %1, %2").arg(over.name, wide), [=] {
                still();
                click(window, QPointF(window->width() - 20.0, window->height() * 0.6));
                QTimer::singleShot(600, window, [=] {
                    const QStringList got = untouched();
                    check(got.isEmpty(), QStringLiteral("a click beside %1: the grid took %2")
                                             .arg(over.name, got.isEmpty() ? QStringLiteral("nothing") : got.join(", ")));
                    key(window, Qt::Key_Escape);
                    key(window, Qt::Key_Escape);
                });
            }, 1000);
        }

        // The viewer: its bars and the photo take what they are given; the
        // grid beneath, nothing while it is open.
        add(QStringLiteral("a photo opened, %1").arg(wide), [=] {
            settleGrid();
            const int index = grid->indexAt(grid->width() / 2, grid->height() / 2);
            view->openAt(index, grid->rectOf(index));
        }, 1200);
        probe(QStringLiteral("the viewer, ") + wide,
              [=] {
                  const double w = window->width(), h = window->height();
                  return QList<QPointF>{QPointF(w / 2, 24), QPointF(40, 24), QPointF(w - 40, 24), QPointF(w / 2, h - 30),
                                        QPointF(w / 2, h / 2)};
              },
              {{"a pinch out", [](QQuickWindow *w, QPointF at) { pinch(w, at, 2.2); }},
               {"a two-finger tap", [](QQuickWindow *w, QPointF at) { click(w, at, Qt::RightButton); }},
               {"a wheel notch", [](QQuickWindow *w, QPointF at) { wheel(w, at, {0, -15}, {0, -120}); }},
               {"a touchpad swipe up", [](QQuickWindow *w, QPointF at) { swipe(w, at, {0, 3}, 4); }}},
              nullptr);
        add(QStringLiteral("the photo closed, %1").arg(wide), [=] { view->close(); }, 1200);
    }
    *keeping = true;
    add("done", [=] {
        say(QStringLiteral("done, %1 failed").arg(*failures));
        QCoreApplication::exit(*failures ? 1 : 0);
    });

    auto run = std::make_shared<std::function<void(int)>>();
    *run = [=](int k) {
        if (k >= steps->size())
            return;
        const Step &step = (*steps)[k];
        step.act();
        // Then on once the grid holds still (what got through to it may
        // have set it moving), within three seconds.
        QTimer::singleShot(step.wait, window, [=] {
            auto waited = std::make_shared<int>(0);
            auto poll = std::make_shared<std::function<void()>>();
            *poll = [=] {
                if (*extraWait > 0) {
                    const int wait = std::exchange(*extraWait, 0);
                    QTimer::singleShot(wait, window, [poll] { (*poll)(); });
                    return;
                }
                if ((grid->animating() || *busy) && ++*waited < 200) {
                    QTimer::singleShot(50, window, [poll] { (*poll)(); });
                    return;
                }
                (*run)(k + 1);
            };
            (*poll)();
        });
    };
    // Once the photos are in, the window has its size and the plugins'
    // stacks and hidden photos have come (none for two seconds), within a
    // minute.
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto settled = std::make_shared<std::function<void()>>();
    *settled = [=] {
        if (clock->elapsed() < 2500 || (quietSince->elapsed() < 2000 && clock->elapsed() < 60000)) {
            QTimer::singleShot(100, window, [settled] { (*settled)(); });
            return;
        }
        say(QStringLiteral("the grid settled after %1 ms: %2 cells, laid out %3 times").arg(clock->elapsed()).arg(grid->count()).arg(*relaid));
        (*run)(0);
    };
    (*settled)();
}
