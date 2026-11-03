#include <QAccessible>
#include <QColor>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QtTest>
#include <memory>

#include "keymap.h"

class CopyStandIn : public QObject {
    Q_OBJECT
public:
    int requests = 0;
    Q_INVOKABLE void copy(const QVariantList &, bool) { ++requests; }
signals:
    void copied(int files, int failed);
    void copyFailed(int files, int failed);
};

class AccessibilityTests : public QObject {
    Q_OBJECT
    struct Fixture {
        QQmlPropertyMap theme;
        Keymap keymap;
        CopyStandIn outgoing;
        QQmlEngine engine;
        std::unique_ptr<QObject> root;
        QStringList warnings;
        explicit Fixture(bool light = false, int fontSize = 14) {
            for (const auto &[key, color] : std::initializer_list<std::pair<const char *, const char *>>{
                     {"background", light ? "#fffcf0" : "#090e18"}, {"foreground", light ? "#100f0f" : "#cdd9ed"},
                     {"darkForeground", light ? "#66645e" : "#8da2c0"}, {"brightForeground", light ? "#100f0f" : "#e5efff"},
                     {"selection", light ? "#cecdc3" : "#22344f"}, {"accent", light ? "#205ea6" : "#42c7f5"},
                     {"muted", "#788599"}, {"red", "#e35e65"}})
                theme.insert(QString::fromLatin1(key), QColor(QString::fromLatin1(color)));
            theme.insert("font", "monospace"); theme.insert("fontSize", fontSize); theme.insert("radius", 4);
            keymap.setLayer("grid");
            engine.rootContext()->setContextProperty("theme", &theme);
            engine.rootContext()->setContextProperty("keymap", &keymap);
            engine.rootContext()->setContextProperty("outgoing", &outgoing);
            QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &errors) {
                for (const auto &error : errors) warnings << error.toString();
            });
            QQmlComponent component(&engine);
            component.setData(R"(import QtQuick
import Pinhole
import "focus.js" as Focus
Window {
    id: window
    width: 1000; height: 840; visible: true
    property int fires: 0
    property int toggles: 0
    property int globalSpace: 0
    property int globalUndo: 0
    property int underneath: 0
    property int wheels: 0
    Shortcut { sequence: "Space"; onActivated: window.globalSpace++ }
    Shortcut { sequence: "Ctrl+Z"; onActivated: window.globalUndo++ }
    MouseArea { anchors.fill: parent; onClicked: window.underneath++; onWheel: window.wheels++ }
    Item {
        id: modal
        objectName: "modal"
        FocusFence.enabled: true
        width: 720; height: parent.height
        Keys.onShortcutOverride: event => {
            if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) && !(event.modifiers & Qt.ControlModifier)) event.accepted = true
        }
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                Focus.move(modal, event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier) ? -1 : 1, window.activeFocusItem)
                event.accepted = true
            }
        }
        ShellButton { objectName: "button"; x: 24; y: 24; text: "Copy"; helpText: "Copy selected photos"; floating: true; onTapped: window.fires++ }
        Toggle { objectName: "toggle"; x: 24; y: 100; Accessible.name: "Show photo dates"; onToggled: { checked = !checked; window.toggles++ } }
        FieldInput { objectName: "field"; x: 24; y: 180; width: 500; placeholder: "Find photos" }
        ShellButton { objectName: "radio"; x: 24; y: 260; text: "Automatic"; checkable: true; active: true; Accessible.role: Accessible.RadioButton }
        ShellButton { objectName: "disabled"; x: 24; y: 340; text: "Unavailable"; floating: true; usable: false; onTapped: window.fires++ }
        ShellButton { objectName: "hidden"; x: 24; y: 420; text: "Hidden"; visible: false }
        FieldInput { objectName: "password"; x: 24; y: 500; width: 500; placeholder: "Password"; password: true; text: "private-value" }
        CopyButton { objectName: "copy"; x: 24; y: 600; floating: true; photos: () => ["one"] }
    }
    ShellButton { objectName: "outside"; x: 760; y: 24; text: "Behind the dialog" }
})", QUrl("qrc:/AccessibilityFixture.qml"));
            root.reset(component.create());
            if (!root) warnings << component.errorString();
            if (auto *window = qobject_cast<QQuickWindow *>(root.get())) window->requestActivate();
        }
        QQuickWindow *window() const { return qobject_cast<QQuickWindow *>(root.get()); }
        QQuickItem *item(const char *name) const { return root ? root->findChild<QQuickItem *>(QLatin1String(name)) : nullptr; }
        QPoint centre(QQuickItem *item) const { return item->mapToScene({item->width()/2, item->height()/2}).toPoint(); }
        QAccessibleInterface *accessible(const char *name) const { return QAccessible::queryAccessibleInterface(item(name)); }
    };

private slots:
    void initTestCase() { QAccessible::setActive(true); }

    void controlsExposeNamesStatesAndActions_data() {
        QTest::addColumn<bool>("light"); QTest::addColumn<int>("fontSize");
        QTest::newRow("dark") << false << 14;
        QTest::newRow("light") << true << 14;
        QTest::newRow("large-type") << false << 24;
    }
    void controlsExposeNamesStatesAndActions() {
        QFETCH(bool, light); QFETCH(int, fontSize);
        Fixture f(light, fontSize);
        QVERIFY2(f.root, qPrintable(f.warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowActive(f.window()));
        auto *button = f.accessible("button"); QVERIFY(button);
        QCOMPARE(button->role(), QAccessible::Button);
        QCOMPARE(button->text(QAccessible::Name), QString("Copy selected photos"));
        QVERIFY(button->state().focusable); QVERIFY(!button->state().disabled);
        QVERIFY(button->actionInterface());
        button->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(f.root->property("fires").toInt(), 1);
        auto *disabled = f.accessible("disabled"); QVERIFY(disabled);
        QVERIFY(disabled->state().disabled); QVERIFY(!disabled->state().focusable);
        disabled->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(f.root->property("fires").toInt(), 1);
        auto *toggle = f.accessible("toggle"); QVERIFY(toggle);
        QCOMPARE(toggle->role(), QAccessible::CheckBox);
        QCOMPARE(toggle->text(QAccessible::Name), QString("Show photo dates"));
        QVERIFY(toggle->state().checkable); QVERIFY(!toggle->state().checked);
        toggle->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
        QVERIFY(toggle->state().checked); QCOMPARE(f.root->property("toggles").toInt(), 1);
        f.item("toggle")->setProperty("usable", false);
        QVERIFY(toggle->state().disabled);
        toggle->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
        QCOMPARE(f.root->property("toggles").toInt(), 1);
        auto *radio = f.accessible("radio"); QVERIFY(radio);
        QCOMPARE(radio->role(), QAccessible::RadioButton);
        QVERIFY(radio->state().checked);
        auto *field = f.item("field")->property("input").value<QQuickItem *>(); QVERIFY(field);
        auto *editable = QAccessible::queryAccessibleInterface(field); QVERIFY(editable);
        QCOMPARE(editable->role(), QAccessible::EditableText);
        QCOMPARE(editable->text(QAccessible::Name), QString("Find photos"));
        auto *password = f.item("password")->property("input").value<QQuickItem *>(); QVERIFY(password);
        auto *protectedField = QAccessible::queryAccessibleInterface(password); QVERIFY(protectedField);
        QVERIFY(protectedField->state().passwordEdit);
        QVERIFY(!protectedField->text(QAccessible::Value).contains("private-value"));
        f.item("button")->setProperty("helpText", "");
        f.item("button")->setProperty("text", "✕");
        QCOMPARE(button->text(QAccessible::Name), QString("Close"));
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void nativeKeyboardActivationWinsOverGlobalShortcuts() {
        Fixture f; QVERIFY2(f.root, qPrintable(f.warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowActive(f.window()));
        auto *button = f.item("button"); button->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyPress(f.window(), Qt::Key_Space);
        QVERIFY(button->property("keyboardPressed").toBool());
        QVERIFY(f.accessible("button")->state().pressed);
        QCOMPARE(f.root->property("fires").toInt(), 0);
        QTest::keyRelease(f.window(), Qt::Key_Space);
        QCOMPARE(f.root->property("fires").toInt(), 1);
        QCOMPARE(f.root->property("globalSpace").toInt(), 0);
        QTest::keyClick(f.window(), Qt::Key_Return);
        QCOMPARE(f.root->property("fires").toInt(), 2);
        auto *input = f.item("field")->property("input").value<QQuickItem *>(); input->forceActiveFocus();
        QTest::keyClick(f.window(), Qt::Key_A);
        QTest::keyClick(f.window(), Qt::Key_Space);
        QTest::keyClick(f.window(), Qt::Key_C);
        QCOMPARE(input->property("text").toString(), QString("a c"));
        QCOMPARE(f.root->property("globalSpace").toInt(), 0);
        QTest::keyClick(f.window(), Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(f.root->property("globalUndo").toInt(), 0);
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void tabFocusStaysInsideTheModalAndSkipsUnavailableControls() {
        Fixture f; QVERIFY2(f.root, qPrintable(f.warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowActive(f.window()));
        f.item("button")->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(f.window(), Qt::Key_Tab); QCOMPARE(f.window()->activeFocusItem(), f.item("toggle"));
        QTest::keyClick(f.window(), Qt::Key_Tab);
        QCOMPARE(f.window()->activeFocusItem(), f.item("field")->property("input").value<QQuickItem *>());
        QTest::keyClick(f.window(), Qt::Key_Tab); QCOMPARE(f.window()->activeFocusItem(), f.item("radio"));
        QTest::keyClick(f.window(), Qt::Key_Tab);
        QCOMPARE(f.window()->activeFocusItem(), f.item("password")->property("input").value<QQuickItem *>());
        QTest::keyClick(f.window(), Qt::Key_Tab); QCOMPARE(f.window()->activeFocusItem(), f.item("copy"));
        QTest::keyClick(f.window(), Qt::Key_Tab); QCOMPARE(f.window()->activeFocusItem(), f.item("button"));
        QTest::keyClick(f.window(), Qt::Key_Backtab); QCOMPARE(f.window()->activeFocusItem(), f.item("copy"));
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void copyButtonKeepsItsFeedbackAndAccessibleKeyboardAction() {
        Fixture f; QVERIFY2(f.root, qPrintable(f.warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowActive(f.window()));
        auto *copy = f.item("copy"); auto *accessible = f.accessible("copy"); QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::Button);
        QCOMPARE(accessible->text(QAccessible::Name), QString("Copy"));
        copy->forceActiveFocus();
        QTest::keyClick(f.window(), Qt::Key_Space);
        QCOMPARE(f.outgoing.requests, 1);
        QCOMPARE(copy->property("phase").toString(), QString("working"));
        QVERIFY(accessible->state().disabled);
        QCOMPARE(accessible->text(QAccessible::Description), QString("Copying…"));
        accessible->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(f.outgoing.requests, 1);
        emit f.outgoing.copied(1, 0);
        QCOMPARE(copy->property("phase").toString(), QString("done"));
        QVERIFY(!accessible->state().disabled);
        QCOMPARE(accessible->text(QAccessible::Description), QString("Copied"));
        copy->forceActiveFocus();
        QTest::keyClick(f.window(), Qt::Key_Return);
        QCOMPARE(f.outgoing.requests, 2);
        QCOMPARE(copy->property("phase").toString(), QString("working"));
        emit f.outgoing.copyFailed(0, 1);
        QCOMPARE(copy->property("phase").toString(), QString("failed"));
        QCOMPARE(f.root->property("globalSpace").toInt(), 0);
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void pointerStatesAndDisabledFloatingInputAreContained() {
        Fixture f; QVERIFY2(f.root, qPrintable(f.warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowActive(f.window()));
        auto *button = f.item("button");
        const QColor rest = button->property("color").value<QColor>();
        QTest::mouseMove(f.window(), QPoint(700, 700));
        QTest::qWait(20);
        QTest::mouseMove(f.window(), f.centre(button));
        QTRY_VERIFY(button->property("hovered").toBool());
        const QColor hover = button->property("color").value<QColor>(); QVERIFY(hover != rest);
        QTest::mousePress(f.window(), Qt::LeftButton, Qt::NoModifier, f.centre(button));
        const QColor pressed = button->property("color").value<QColor>(); QVERIFY(pressed != hover);
        QVERIFY(f.accessible("button")->state().pressed);
        QTest::mouseRelease(f.window(), Qt::LeftButton, Qt::NoModifier, f.centre(button));
        QCOMPARE(f.root->property("fires").toInt(), 1);
        QCOMPARE(f.root->property("underneath").toInt(), 0);
        const QPoint at = f.centre(f.item("disabled"));
        const QColor disabledColor = f.item("disabled")->property("color").value<QColor>();
        QTest::mousePress(f.window(), Qt::LeftButton, Qt::NoModifier, at);
        QVERIFY(!f.accessible("disabled")->state().pressed);
        QCOMPARE(f.item("disabled")->property("color").value<QColor>(), disabledColor);
        QTest::mouseRelease(f.window(), Qt::LeftButton, Qt::NoModifier, at);
        QWheelEvent wheel(at, f.window()->mapToGlobal(at), {}, {0, 120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(f.window(), &wheel);
        QCOMPARE(f.root->property("fires").toInt(), 1);
        QCOMPARE(f.root->property("underneath").toInt(), 0);
        QCOMPARE(f.root->property("wheels").toInt(), 0);
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }
};
QTEST_MAIN(AccessibilityTests)
#include "accessibility_tests.moc"
