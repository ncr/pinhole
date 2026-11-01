// PINHOLE_SELFTEST=sources:<dir>: the window's sources as they look, a
// frame of each in <dir> (selftest.h): the first run, the side panel's
// rows and what they ask, Add a source, and signing in to iCloud through
// to the code and the card saying a source needs signing in.

#include "selftest.h"

#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

void key(QQuickWindow *window, int code, const QString &text = {}) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, Qt::NoModifier, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, Qt::NoModifier, text);
}

// Typed, a character at a time, into whatever has the keys.
void type(QQuickWindow *window, const QString &text) {
    for (const QChar c : text)
        key(window, c.toUpper().unicode(), QString(c));
}

} // namespace

void runSourcesSelfTest(QQuickWindow *window, const QString &dir) {
    auto find = [window](const char *name) { return window->findChild<QObject *>(QString::fromLatin1(name)); };
    auto grab = [window, dir](const QString &name) {
        window->grabWindow().save(dir + '/' + name + ".png");
        std::printf("selftest: %s\n", qPrintable(name));
        std::fflush(stdout);
    };
    auto show = [find](const char *name, bool shown) {
        if (QObject *o = find(name))
            o->setProperty("shown", shown);
    };
    // iCloud's sign-in: Pinhole's own panel before its plugin, then the
    // plugin's.
    auto openSignIn = [window, find] {
        if (QObject *panel = find("signIn")) {
            panel->setProperty("appleId", QString());
            panel->setProperty("shown", true);
            return;
        }
        QVariant icloud;
        QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, icloud), Q_ARG(QVariant, QStringLiteral("pinhole.icloud")));
        if (QObject *plugin = icloud.value<QObject *>())
            QMetaObject::invokeMethod(plugin, "openSignIn", Q_ARG(QVariant, QString()));
        else
            std::printf("selftest: no iCloud plugin\n");
    };
    // PINHOLE_SELFTEST_PICK: the key pressed on the first run ("Return",
    // "2"), if it shows.
    const QString pick = qEnvironmentVariable("PINHOLE_SELFTEST_PICK");
    using Step = std::pair<int, std::function<void()>>;
    auto steps = std::make_shared<std::vector<Step>>(std::vector<Step>{
        {1500, [=] {
             QObject *first = find("firstRun");
             const bool shown = first && first->property("shown").toBool();
             std::printf("selftest: first run %s\n", shown ? "shown" : "not shown");
             if (shown) {
                 grab("00-firstrun");
                 if (pick == QLatin1String("Return"))
                     key(window, Qt::Key_Return);
                 else if (pick.size() == 1)
                     type(window, pick);
             }
         }},
        {pick.isEmpty() ? 100 : 4000, [=] { grab("01-grid"); show("sidePanel", true); }},
        {1000, [=] { grab("02-side"); type(window, "o"); }},
        {400, [=] { grab("03-side-O"); key(window, Qt::Key_Escape); key(window, Qt::Key_Delete); }},
        {400, [=] { grab("04-side-remove"); key(window, Qt::Key_Escape); }},
        {300, [=] { show("sidePanel", false); show("addSource", true); }},
        {1500, [=] { grab("05-add"); show("addSource", false); }},
        {400, [=] { openSignIn(); }},
        {800, [=] { grab("06-signin"); type(window, "jane@example.com"); key(window, Qt::Key_Return); type(window, "two-factor"); }},
        {400, [=] { grab("07-typed"); key(window, Qt::Key_Return); }},
        {2500, [=] { grab("08-code"); type(window, "000000"); }},
        {2500, [=] { grab("09-wrong-code"); type(window, "123456"); }},
        {3000, [=] { grab("10-signed-in"); }},
    });
    auto run = std::make_shared<std::function<void(size_t)>>();
    *run = [=](size_t at) {
        if (at == steps->size()) {
            // Then the card saying it needs signing in, once the service
            // has found it does.
            auto waited = std::make_shared<int>(0);
            auto poll = std::make_shared<std::function<void()>>();
            *poll = [=] {
                QQuickItem *trouble = window->findChild<QQuickItem *>(QStringLiteral("trouble"));
                if ((trouble && trouble->isVisible()) || ++*waited > 40) {
                    std::printf("selftest: the card saying it needs signing in %s\n",
                                trouble && trouble->isVisible() ? "shown" : "NOT shown");
                    QTimer::singleShot(400, window, [=] {
                        grab("11-trouble");
                        QCoreApplication::quit();
                    });
                    return;
                }
                QTimer::singleShot(250, window, *poll);
            };
            (*poll)();
            return;
        }
        QTimer::singleShot((*steps)[at].first, window, [=] {
            (*steps)[at].second();
            (*run)(at + 1);
        });
    };
    (*run)(0);
}
