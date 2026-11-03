#include <QAccessible>
#include <QCoreApplication>
#include <QDir>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>
#include <cstdio>
#include <memory>

namespace {
QQuickItem *namedItem(QQuickItem *root, const QString &name) {
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems()) if (auto *found = namedItem(child, name)) return found;
    return nullptr;
}
}

// Run only through the ordinary guarded self-test entry point: no real
// Pictures, clipboard, settings, or network credentials are used.
void runAccessibilitySelfTest(QQuickWindow *window, const QString &folder) {
    QAccessible::setActive(true);
    window->requestActivate();
    if (!folder.isEmpty()) QDir().mkpath(folder);
    auto *timer = new QTimer(window);
    auto phase = std::make_shared<int>(0);
    const auto key = [window](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = {}) {
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, modifiers, text);
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, modifiers, text);
    };
    const auto inside = [](QQuickItem *item, QQuickItem *root) {
        for (; item; item = item->parentItem()) if (item == root) return true;
        return false;
    };
    const auto click = [window](QQuickItem *item) {
        const auto at = item->mapToScene({item->width()/2, item->height()/2});
        QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &release);
    };
    const auto fail = [timer](const char *message) {
        std::fprintf(stderr, "selftest: accessibility: %s\n", message);
        timer->stop(); QCoreApplication::exit(1);
    };
    QObject::connect(timer, &QTimer::timeout, window, [=] {
        auto *side = window->findChild<QQuickItem *>("sidePanel");
        auto *guide = window->findChild<QQuickItem *>("pluginGuide");
        const auto save = [window, folder](const QString &name) {
            if (!folder.isEmpty()) window->grabWindow().save(folder + '/' + name + ".png");
        };
        if (!side) return fail("missing settings panel");
        switch ((*phase)++) {
        case 0:
            if (guide) return fail("workshop was constructed during startup");
            if (window->findChild<QQuickItem *>("selectBar")) return fail("selection bar was constructed during startup");
            if (side->property("prepared").toBool()) return fail("settings rows were constructed during startup");
            if (auto *grid = window->findChild<QQuickItem *>("grid")) grid->forceActiveFocus();
            std::fprintf(stderr, "selftest: accessibility: initial layer=%s active=%d\n", qPrintable(window->property("keyLayer").toString()), window->isActive());
            key(Qt::Key_Tab);
            return;
        case 1: {
            if (!side->property("shown").toBool()) return fail("Tab did not open settings");
            if (!side->property("prepared").toBool()) return fail("settings rows did not load on Tab");
            auto *accessible = QAccessible::queryAccessibleInterface(side);
            if (!accessible || accessible->role() != QAccessible::Dialog || accessible->text(QAccessible::Name).isEmpty())
                return fail("settings has no named accessible dialog");
            auto *before = window->activeFocusItem();
            key(Qt::Key_Tab);
            if (!side->property("shown").toBool() || before == window->activeFocusItem() || !inside(window->activeFocusItem(), side))
                return fail("Tab closed settings or did not move focus inside it");
            key(Qt::Key_Backtab, Qt::ShiftModifier);
            if (before != window->activeFocusItem()) return fail("Shift+Tab did not reverse focus traversal");
            QMetaObject::invokeMethod(side, "selectSection", Q_ARG(QVariant, QStringLiteral("photos")), Q_ARG(QVariant, true));
            return;
        }
        case 2: {
            auto *tab = namedItem(side, "sideSection-photos");
            auto *accessible = QAccessible::queryAccessibleInterface(tab);
            if (!tab || !accessible || accessible->role() != QAccessible::PageTab || !accessible->state().selected)
                return fail("section tab has no accessible selected state");
            save("settings-photos");
            key(Qt::Key_Right);
            return;
        }
        case 3:
            if (side->property("section").toString() != "plugins") return fail("Right did not navigate section tabs");
            save("settings-plugins");
            window->setProperty("guideShown", true);
            return;
        case 4: {
            if (!guide) return fail("workshop did not load on demand");
            auto *accessible = QAccessible::queryAccessibleInterface(guide);
            if (!accessible || accessible->role() != QAccessible::Dialog || accessible->text(QAccessible::Name).isEmpty())
                return fail("workshop has no named accessible dialog");
            key(Qt::Key_F, Qt::ControlModifier);
            auto *search = guide->findChild<QObject *>("guideSearch");
            auto *input = search ? search->property("input").value<QQuickItem *>() : nullptr;
            if (!input || window->activeFocusItem() != input) return fail("Ctrl+F did not focus document search");
            key(Qt::Key_C, Qt::NoModifier, "c");
            if (search->property("text").toString() != "c") return fail("typing in search was intercepted by a global shortcut");
            search->setProperty("text", "");
            key(Qt::Key_Tab);
            if (window->activeFocusItem() == input || !inside(window->activeFocusItem(), guide)) return fail("workshop Tab focus escaped or stalled");
            key(Qt::Key_Tab, Qt::ControlModifier);
            return;
        }
        case 5:
            if (guide->property("document").toString() != "plugins") return fail("Ctrl+Tab did not change documents");
            key(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
            return;
        case 6:
            if (guide->property("document").toString() != "plugin-quickstart") return fail("Ctrl+Shift+Tab did not return to previous document");
            save("workshop-wide");
            window->resize(560, 520);
            return;
        case 7: {
            if (!guide->property("compact").toBool()) return fail("workshop did not adapt to a narrow window");
            auto *body = guide->findChild<QQuickItem *>("guideBody");
            auto *brief = guide->findChild<QQuickItem *>("guideCopyAgent");
            if (!body || body->height() < 70 || !brief || !brief->isVisible()) return fail("compact workshop hid the reader or agent brief");
            for (int i = 0; i < 35; ++i) {
                key(Qt::Key_Tab);
                if (!inside(window->activeFocusItem(), guide)) {
                    std::fprintf(stderr, "selftest: accessibility: compact Tab %d focused %s\n", i, window->activeFocusItem() ? qPrintable(window->activeFocusItem()->objectName()) : "null");
                    return fail("compact workshop focus escaped");
                }
            }
            save("workshop-compact");
            key(Qt::Key_Escape);
            return;
        }
        case 8:
            if (window->property("guideShown").toBool() || !side->property("shown").toBool() || !inside(window->activeFocusItem(), side))
                return fail("closing workshop did not restore settings focus");
            save("settings-compact");
            key(Qt::Key_Escape);
            return;
        case 9: {
            if (side->property("shown").toBool()) return fail("Escape did not close settings");
            auto *picker = window->findChild<QQuickItem *>("addSource");
            if (!picker) return fail("missing source picker");
            picker->setProperty("shown", true);
            return;
        }
        case 10: {
            auto *picker = window->findChild<QQuickItem *>("addSource");
            auto *accessible = QAccessible::queryAccessibleInterface(picker);
            if (!accessible || accessible->role() != QAccessible::Dialog || accessible->text(QAccessible::Name).isEmpty())
                return fail("source picker has no named accessible dialog");
            for (int i = 0; i < 12; ++i) {
                key(Qt::Key_Tab);
                if (!inside(window->activeFocusItem(), picker)) return fail("source picker Tab focus escaped");
            }
            save("source-picker-compact");
            key(Qt::Key_Escape);
            return;
        }
        case 11: {
            if (window->findChild<QQuickItem *>("addSource")->property("shown").toBool()) return fail("Escape did not close source picker");
            window->resize(1400, 900);
            auto *date = QAccessible::queryAccessibleInterface(window->findChild<QQuickItem *>("dateLabel"));
            if (!date || date->role() != QAccessible::Button || date->text(QAccessible::Name).isEmpty() || !date->actionInterface())
                return fail("grid date label has no named accessible Photo order button");
            key(Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
            return;
        }
        case 12: {
            auto *order = window->findChild<QQuickItem *>("orderMenu");
            if (!order || !order->property("open").toBool()) return fail("photo order did not open on its shortcut");
            auto *accessible = QAccessible::queryAccessibleInterface(order);
            if (!accessible || accessible->role() != QAccessible::PopupMenu) return fail("photo order has no accessible menu");
            for (int i = 0; i < 7; ++i) {
                key(Qt::Key_Tab);
                if (!inside(window->activeFocusItem(), order)) return fail("photo order Tab focus escaped");
                const int focused = QStringList{"orderOption_smart", "orderOption_taken", "orderOption_added"}.indexOf(window->activeFocusItem()->objectName());
                if (focused < 0 || order->property("current").toInt() != focused) return fail("photo order Tab did not synchronize arrow-navigation position");
            }
            order->setProperty("pointerInput", false);
            order->setProperty("current", 0);
            return;
        }
        case 13:
            key(Qt::Key_Down);
            return;
        case 14: {
            auto *order = window->findChild<QQuickItem *>("orderMenu");
            auto *option = namedItem(order, "orderOption_taken");
            auto *accessible = QAccessible::queryAccessibleInterface(option);
            if (!accessible || accessible->role() != QAccessible::RadioButton || !accessible->state().focused || window->activeFocusItem() != option)
                return fail("photo order arrows did not update native radio focus");
            save("photo-order-menu");
            key(Qt::Key_Return);
            return;
        }
        case 15: {
            auto *order = window->findChild<QQuickItem *>("orderMenu");
            if (order->property("open").toBool() || order->property("value").toString() != "taken") return fail("photo order Return chose the wrong option");
            auto *grid = window->findChild<QQuickItem *>("grid");
            key(Qt::Key_A, Qt::ControlModifier);
            if (!grid->property("selecting").toBool() || grid->property("selectionCount").toInt() == 0)
                return fail("Ctrl+A did not enter select mode and select photos");
            auto *copy = window->findChild<QQuickItem *>("selectCopyButton");
            auto *accessible = QAccessible::queryAccessibleInterface(copy);
            if (!accessible || accessible->role() != QAccessible::Button || accessible->state().disabled) return fail("selection Copy has no usable accessible button");
            auto *group = window->findChild<QQuickItem *>("selectCopyGroup");
            group->setProperty("open", true);
            return;
        }
        case 16: {
            auto *group = window->findChild<QQuickItem *>("selectCopyGroup");
            auto *menu = namedItem(group, "selectCopyGroupCard");
            auto *accessible = QAccessible::queryAccessibleInterface(menu);
            if (!accessible || accessible->role() != QAccessible::PopupMenu || accessible->text(QAccessible::Name).isEmpty())
                return fail("copy group has no named accessible popup");
            save("export-menu");
            for (int i = 0; i < 6; ++i) {
                key(Qt::Key_Tab);
                if (!inside(window->activeFocusItem(), menu)) return fail("copy menu Tab focus escaped");
                if (window->activeFocusItem()->property("index").toInt() != group->property("current").toInt()) return fail("copy menu Tab did not synchronize arrow-navigation position");
            }
            key(Qt::Key_Escape);
            return;
        }
        case 17:
            if (window->findChild<QQuickItem *>("selectCopyGroup")->property("open").toBool()) return fail("Escape did not close Copy menu");
            window->findChild<QQuickItem *>("grid")->setProperty("selecting", false);
            window->resize(560, 640);
            return;
        case 18: {
            auto *more = window->findChild<QQuickItem *>("gridMore");
            auto *button = more ? namedItem(more, "moreButton") : nullptr;
            if (!more || !more->isVisible() || !button) return fail("narrow grid did not expose More");
            auto *date = window->findChild<QQuickItem *>("dateLabel");
            if (date->mapToScene({date->width(), 0}).x() > button->mapToScene({0, 0}).x())
                return fail("long date overlaps More's pointer area");
            click(button);
            return;
        }
        case 19: {
            auto *more = window->findChild<QQuickItem *>("gridMore");
            if (!more->property("open").toBool() || more->property("current").toInt() != -1)
                return fail("pointer-opened More focused an arbitrary action");
            save("more-menu");
            click(namedItem(more, "moreButton"));
            return;
        }
        default:
            if (window->findChild<QQuickItem *>("gridMore")->property("open").toBool()) return fail("second More click did not close it");
            std::puts("selftest: accessibility: settings/workshop/source dialogs, consistent action/radio menus, More pointer toggle, Tab containment, document/search keys, compact layout and focus restoration: PASS");
            timer->stop(); QCoreApplication::quit();
        }
    });
    timer->start(500);
}
