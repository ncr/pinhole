// PINHOLE_SELFTEST=people:<dir>: who is in the photo shown (plugins/people,
// W), as it runs (selftest.h). test-selftests puts a face index of its own
// in the plugin's cache and names <dir>/people.json's photos: `named`, with
// one face given a name, Anna; `shown`, with two faces, the left one like
// Anna's, the right one like no one's. On `shown` the button says Anna? +1;
// the card names the left one Anna (Yes), goes on to the right one, names it
// Ola (a new name) and closes; the button then says both; Ola taken back,
// it counts one again. Turned a quarter, the faces are where the photo
// turned has them. Opened from People, W shows the card over the photo
// with the keys. Pictures of each step in <dir>.

#include "selftest.h"

#include "library.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

namespace {
void press(QQuickWindow *window, int code, const QString &text = QString()) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, Qt::NoModifier, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, Qt::NoModifier, text);
}

void type(QQuickWindow *window, const QString &text) {
    for (const QChar c : text)
        press(window, c.toUpper().unicode(), QString(c));
}

QVariantList listOf(const QVariant &value) {
    return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toList() : value.toList();
}
} // namespace

void runPeopleSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &dir) {
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        std::printf("selftest: %s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
        std::fflush(stdout);
        if (!ok)
            ++*failures;
    };
    auto finish = [failures] { QCoreApplication::exit(*failures ? 1 : 0); };
    auto waitFor = [window, check, finish](std::function<bool()> ready, int ms, QString what, std::function<void()> then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto *timer = new QTimer(window);
        timer->setInterval(20);
        QObject::connect(timer, &QTimer::timeout, window, [=] {
            if (ready()) {
                timer->deleteLater();
                check(true, what);
                then();
            } else if (clock->elapsed() > ms) {
                timer->deleteLater();
                check(false, what + " (timed out)");
                finish();
            }
        });
        timer->start();
    };
    auto shoot = [window, dir](const QString &name) {
        if (!dir.isEmpty())
            window->grabWindow().save(dir + '/' + name + ".png");
    };
    auto search = std::make_shared<std::function<QQuickItem *(QQuickItem *, const QString &)>>();
    *search = [weak = std::weak_ptr(search)](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name)
            return item;
        for (QQuickItem *child : item->childItems())
            if (QQuickItem *found = (*weak.lock())(child, name))
                return found;
        return nullptr;
    };
    auto find = [window, search](const QString &name) { return (*search)(window->contentItem(), name); };
    // Inside the card: People's view beneath has a picker of its own.
    auto inCard = [find, search](const QString &name) {
        QQuickItem *card = find(QStringLiteral("peopleWhoView"));
        return card ? (*search)(card, name) : nullptr;
    };

    QFile given(dir + QStringLiteral("/people.json"));
    const QJsonObject photos = given.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(given.readAll()).object() : QJsonObject();
    const QByteArray shownId = photos.value(QStringLiteral("shown")).toString().toUtf8();

    QTimer::singleShot(2500, window, [=, &library, &viewer] {
        QVariant instance;
        QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, instance),
                                  Q_ARG(QVariant, QStringLiteral("pinhole.people")));
        QObject *plugin = instance.value<QObject *>();
        QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, instance),
                                  Q_ARG(QVariant, QStringLiteral("pinhole.edit")));
        QObject *edit = instance.value<QObject *>();
        if (!plugin || !edit || shownId.isEmpty()) {
            check(false, "People and Turn and crop are loaded, and the photos are given");
            return finish();
        }
        int at = -1;
        for (int i = 0; i < library.count(); ++i)
            if (library.at(i).id == shownId)
                at = i;
        if (at < 0) {
            check(false, "the photo with two faces is in the library");
            return finish();
        }
        const double shape = double(library.at(at).thumbW) / library.at(at).thumbH;
        auto said = [plugin] { return plugin->property("whoText").toString(); };
        auto faces = [plugin] { return listOf(plugin->property("faces")); };
        auto nameOf = [faces](int i) { return faces().value(i).toMap().value("name").toString(); };

        // Anna's photos in People, one opened from there (to come back to
        // them): W shows the card over it, with the keys, and Esc leaves
        // the photo shown; closed, People is back.
        auto fromPeople = [=, &viewer] {
            QQuickItem *people = find(QStringLiteral("peopleView"));
            if (!people) {
                check(false, "the People view is there");
                return finish();
            }
            QMetaObject::invokeMethod(people, "open", Q_ARG(QVariant, QVariant()));
            QObject *panel = people->property("item").value<QObject *>();
            QMetaObject::invokeMethod(panel, "go", Q_ARG(QVariant, QStringLiteral("person")), Q_ARG(QVariant, QStringLiteral("Anna")));
            waitFor([=] { return listOf(panel->property("faces")).size() == 2; }, 8000, "People shows Anna's two photos", [=, &viewer] {
                QMetaObject::invokeMethod(panel, "enter", Q_ARG(QVariant, listOf(panel->property("faces")).value(0)));
                waitFor([=, &viewer] { return viewer.isOpen() && plugin->property("hasFaces").toBool(); }, 8000,
                        "one opens from there, its faces read", [=, &viewer] {
                    check(!people->isVisible(), "People hides while the photo shows");
                    press(window, Qt::Key_W, QStringLiteral("w"));
                    QQuickItem *view = find(QStringLiteral("peopleWho"));
                    waitFor([=] { return view->property("shown").toBool() && view->isVisible()
                                         && window->property("coveringPlugin").toString() == QLatin1String("pinhole.people"); },
                            5000, "W shows the card over it, with the keys", [=, &viewer] {
                        QTimer::singleShot(600, window, [=, &viewer] {
                            shoot("from-people");
                            // Its keys: typed into its field, not People's 0
                            // and 1 beneath; Esc clears it first.
                            type(window, QStringLiteral("o1"));
                            QQuickItem *field = inCard(QStringLiteral("peoplePickerSearch"));
                            QObject *panel = find(QStringLiteral("peopleView"))->property("item").value<QObject *>();
                            check(field && field->property("text").toString() == QLatin1String("o1"), "keys go to the card");
                            check(panel->property("page").toString() == QLatin1String("person"), "People beneath stays as it was");
                            press(window, Qt::Key_Escape);
                            press(window, Qt::Key_Escape);
                            waitFor([=, &viewer] { return !view->property("shown").toBool() && viewer.isOpen()
                                                          && window->property("coveringPlugin").toString().isEmpty(); },
                                    5000, "Esc closes the card, the photo still shown", [=, &viewer] {
                                viewer.close();
                                waitFor([=] { return people->isVisible() && people->property("shown").toBool(); }, 5000,
                                        "closed, People is back", [=] { QTimer::singleShot(500, window, finish); });
                            });
                        });
                    });
                });
            });
        };

        viewer.openAt(at, {});
        waitFor([plugin] { return plugin->property("hasFaces").toBool(); }, 8000, "the photo's faces are read", [=, &viewer] {
            check(faces().size() == 2, QStringLiteral("two faces: %1").arg(faces().size()));
            check(said() == QLatin1String("Anna? +1"), "its button says who it may be: " + said());
            QTimer::singleShot(600, window, [=, &viewer] {
                shoot("button");
                QQuickItem *view = find(QStringLiteral("peopleWho"));
                if (!view) {
                    check(false, "the who view is there");
                    return finish();
                }
                press(window, Qt::Key_W, QStringLiteral("w"));
                waitFor([=] { return view->property("shown").toBool() && find(QStringLiteral("peopleWhoView")) != nullptr; }, 5000,
                        "W opens the card", [=, &viewer] {
                    QQuickItem *who = find(QStringLiteral("peopleWhoView"));
                    check(who->property("at").toInt() == 0, "it starts on the first face not named, the left one");
                    // The rings are on the photo: its frame is the photo's as
                    // the viewer shows it (narrower than the window, as wide
                    // as its shape makes it at the window's height or less).
                    const QRectF frame = who->property("frame").toRectF();
                    check(frame.x() > 0 && frame.width() < window->width() && std::abs(frame.width() / frame.height() - shape) < 0.02,
                          QStringLiteral("the faces are ringed over the photo: %1,%2 %3x%4")
                              .arg(frame.x()).arg(frame.y()).arg(frame.width()).arg(frame.height()));
                    QTimer::singleShot(600, window, [=, &viewer] {
                        shoot("card");
                        // ⏎ with nothing typed: Yes, Anna.
                        press(window, Qt::Key_Return, QStringLiteral("\r"));
                        waitFor([=] { return nameOf(0) == QLatin1String("Anna"); }, 8000, "⏎ (Yes) names the left face Anna", [=, &viewer] {
                            check(who->property("at").toInt() == 1, "and goes on to the right one");
                            type(window, QStringLiteral("Ola"));
                            press(window, Qt::Key_Return, QStringLiteral("\r"));
                            waitFor([=] { return nameOf(1) == QLatin1String("Ola") && !view->property("shown").toBool(); }, 8000,
                                    "a new name for the right face, and the card closes after the last", [=, &viewer] {
                                check(said() == QLatin1String("Anna, Ola"), "the button says both: " + said());
                                QTimer::singleShot(600, window, [=, &viewer] {
                                    shoot("named");
                                    // W, the right face (its ring), Not Ola.
                                    press(window, Qt::Key_W, QStringLiteral("w"));
                                    QQuickItem *who = find(QStringLiteral("peopleWhoView"));
                                    QMetaObject::invokeMethod(who, "choose", Q_ARG(QVariant, 1));
                                    waitFor([=] {
                                        QQuickItem *notButton = inCard(QStringLiteral("peopleRemove"));
                                        return notButton && notButton->isVisible() && notButton->property("text").toString() == QLatin1String("Not Ola");
                                    }, 5000, "the card offers Not Ola for the right face", [=, &viewer] {
                                    QMetaObject::invokeMethod(inCard(QStringLiteral("peopleRemove")), "tapped");
                                    waitFor([=] { return said() == QLatin1String("Anna +1"); }, 8000, "Not Ola takes the name back",
                                            [=, &viewer] {
                                        press(window, Qt::Key_Escape);
                                        check(!find(QStringLiteral("peopleWho"))->property("shown").toBool(), "Esc closes the card");
                                        check(nameOf(1).isEmpty(), "the right face has no name");
                                        const double before = faces().value(0).toMap().value("drawn").toList().value(0).toDouble();
                                        QMetaObject::invokeMethod(edit, "turn", Q_ARG(QVariant, QVariantList{QString::fromUtf8(shownId)}),
                                                                  Q_ARG(QVariant, 1));
                                        // Turned a quarter clockwise, the left face (0.1, 0.2, 0.2 × 0.25) is at the top
                                        // right: x 1 − 0.2 − 0.25.
                                        auto anna = [=] {
                                            for (const QVariant &face : faces())
                                                if (face.toMap().value("name") == QLatin1String("Anna"))
                                                    return face.toMap().value("drawn").toList();
                                            return QVariantList();
                                        };
                                        waitFor([=] { return std::abs(anna().value(0).toDouble() - 0.55) < 1e-3; }, 8000,
                                                QStringLiteral("turned, Anna's face is where the photo turned has it (x was %1)").arg(before),
                                                [=, &viewer] {
                                            check(std::abs(anna().value(1).toDouble() - 0.1) < 1e-3, "and y is 0.1");
                                            press(window, Qt::Key_W, QStringLiteral("w"));
                                            QTimer::singleShot(900, window, [=, &viewer] {
                                                shoot("turned");
                                                check(view->property("shown").toBool(), "W opens the card on the photo turned");
                                                press(window, Qt::Key_Escape);
                                                check(!view->property("shown").toBool(), "Esc closes it");
                                                QMetaObject::invokeMethod(edit, "original",
                                                                          Q_ARG(QVariant, QVariantList{QString::fromUtf8(shownId)}));
                                                QTimer::singleShot(800, window, [=, &viewer] {
                                                    viewer.close();
                                                    QTimer::singleShot(800, window, [=, &viewer] { fromPeople(); });
                                                });
                                            });
                                        });
                                    });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });
}
