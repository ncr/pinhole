#include "commands.h"

#include "adjustment.h"
#include "library.h"
#include "outgoing.h"
#include "recent.h"
#include "serviceclient.h"
#include "storage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QLocale>
#include <QRegularExpression>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <termios.h>
#include <unistd.h>

namespace {

const char *kUsage = R"(Usage: pinhole [command]

With no command, the window opens. A command asks the Pinhole service
(pinholed), which is started if it is not running:

  status                 the source shown, its library, and how it is doing
  sources                every source added; * marks the one shown
  show <source>          show another source: an id or a name from `sources`,
                         a folder, or an Apple ID (added if it is new)
  remove <source>        remove a source and its library (a folder's photos
                         stay where they are)
  sign-in [kind] [account]
                         sign in to a source that needs it and show its
                         photos: iCloud (an Apple ID, or icloudpd's session)
                         unless another kind is named
  sign-out <source>      forget a source's session; its library stays
  sync                   look for changes now, and follow until that is done
  add <file>... [--json] add photos and videos, or folders of them, to the
                         source shown
  recent [--count <n>] [--follow]
                         the photos added most lately, newest first, as JSON
  export [--to <folder>] [--json] [--] <id>...
                         photos of the library shown (ids from `recent`) as
                         files to hand over, as a drag out of the window
                         makes them: a HEIC as a JPEG, an edit as it looks;
                         prints each file's path. After --, ids only
  storage [--limit <GB>] [--move-to <folder>]
                         where Pinhole keeps its files, and how much room
                         they may take
  display                what the compositor says of each display: its
                         colours, its brightness, and any HDR headroom
  plugin new <id>        begin a plugin of your own (docs/plugins.md)
  plugin check [folder]  look a plugin over as Pinhole would load it
  help                   this
)";

const QStringList kCommands = {"status", "sources", "show", "remove", "sign-in", "sign-out", "sync", "add", "storage",
                               "recent", "export", "help"};

void say(const QString &line) {
    std::fputs(qPrintable(line + '\n'), stdout);
    std::fflush(stdout);
}

// Progress and questions: on stderr, so what `say` prints can be piped.
void note(const QString &line) {
    std::fputs(qPrintable(line + '\n'), stderr);
    std::fflush(stderr);
}

QString ask(const QString &prompt, bool hidden = false) {
    std::fputs(qPrintable(prompt), stderr);
    std::fflush(stderr);
    // Not echoed while a password is typed.
    termios before{};
    const bool masked = hidden && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &before) == 0;
    if (masked) {
        termios quiet = before;
        quiet.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    char buffer[1024];
    const QString line = std::fgets(buffer, sizeof buffer, stdin) ? QString::fromLocal8Bit(buffer).trimmed() : QString();
    if (masked) {
        tcsetattr(STDIN_FILENO, TCSANOW, &before);
        std::fputs("\n", stderr);
    }
    return line;
}

QString count(qint64 n) { return QLocale(QLocale::English).toString(n); }
QString size(double bytes) { return QLocale(QLocale::English).formattedDataSize(qint64(bytes), 1, QLocale::DataSizeSIFormat); }
QString clock(const QJsonValue &ms) { return QDateTime::fromMSecsSinceEpoch(qint64(ms.toDouble())).toString("HH:mm:ss"); }

// One command's run: its requests, and what it says of them.
class Command {
public:
    explicit Command(const QStringList &args) : m_args(args) {}

    void start() {
        const QString name = m_args.value(0);
        const QStringList rest = m_args.mid(1);
        if (name == "status" && rest.isEmpty())
            status();
        else if (name == "sources" && rest.isEmpty())
            sources();
        else if (name == "show" && rest.size() == 1)
            show(rest.first());
        else if (name == "remove" && rest.size() == 1)
            remove(rest.first());
        else if (name == "sign-in" && rest.size() <= 2)
            signIn(rest);
        else if (name == "sign-out" && rest.size() == 1)
            signOut(rest.first());
        else if (name == "sync" && rest.isEmpty())
            sync();
        else if (name == "add" && !rest.isEmpty())
            add(rest);
        else if (name == "storage")
            storage(rest);
        else if (name == "export" && !rest.isEmpty())
            exportPhotos(rest);
        else
            usage();
    }

private:
    using Then = std::function<void(const QJsonObject &)>;

    // Asked of the service; `then` with its answer, or the run ends saying
    // what went wrong.
    void call(const QString &method, const QJsonObject &params, Then then) {
        m_service.call(method, params, [this, then](bool ok, const QJsonObject &result, const QString &code) {
            if (ok)
                then(result);
            else
                fail(code, result.value("message").toString());
        });
    }

    void fail(const QString &code, const QString &message = {}) {
        if (m_json)
            say(QString::fromUtf8(QJsonDocument(QJsonObject{{"error", code}, {"message", message}}).toJson(QJsonDocument::Compact)));
        if (code == QLatin1String("no_service"))
            note("pinhole: the Pinhole service is not running, and pinholed could not be started");
        else if (code == QLatin1String("incompatible"))
            note("pinhole: the Pinhole service running is of another version; stop it (pkill pinholed) and try again");
        else
            note(QStringLiteral("pinhole: %1").arg(message.isEmpty() ? code : QStringLiteral("%1 (%2)").arg(message, code)));
        QCoreApplication::exit(1);
    }

    void done(int code = 0) { QCoreApplication::exit(code); }

    void usage() {
        std::fputs(kUsage, stderr);
        done(2);
    }

    // The source `what` names among those added: its id, Apple ID, folder
    // or name; empty if none.
    static QJsonObject find(const QJsonArray &all, const QString &what) {
        const QString path = QFileInfo(what).absoluteFilePath();
        for (const QJsonValue &v : all) {
            const QJsonObject s = v.toObject();
            if (s.value("id").toString() == what || s.value("account").toString() == what
                || s.value("path").toString() == QDir::cleanPath(path)
                || s.value("name").toString().compare(what, Qt::CaseInsensitive) == 0)
                return s;
        }
        return {};
    }

    void status() {
        call("status", {}, [this](const QJsonObject &status) {
            call("sources", {}, [this, status](const QJsonObject &list) {
                const QJsonObject source = status.value("source").toObject();
                if (source.value("kind").isNull()) {
                    say("Source:   none yet (pinhole sign-in, or pinhole show <folder>)");
                    done();
                    return;
                }
                const QJsonObject shown = find(list.value("sources").toArray(), list.value("current").toString());
                say(QStringLiteral("Source:   %1 (%2)").arg(shown.value("name").toString(source.value("name").toString()),
                                                              shown.value("id").toString()));
                say(QStringLiteral("Health:   %1").arg(status.value("health").toString()));
                say(QStringLiteral("Library:  %1").arg(status.value("library").toString()));
                say(QStringLiteral("Photos:   %1, %2 with thumbnails")
                        .arg(count(shown.value("count").toInteger()), count(shown.value("thumbs").toInteger())));
                if (const QJsonObject progress = status.value("progress").toObject(); !progress.isEmpty())
                    say(QStringLiteral("Building: %1 %2 of %3").arg(progress.value("phase").toString(),
                                                                    count(progress.value("done").toInteger()),
                                                                    count(progress.value("total").toInteger())));
                const QJsonObject rounds = status.value("rounds").toObject();
                QString round = rounds.value("last").isNull() ? QStringLiteral("none yet") : "last " + clock(rounds.value("last"));
                round += rounds.value("next").isNull() ? QStringLiteral(", one under way") : ", next by " + clock(rounds.value("next"));
                if (rounds.value("push").toBool())
                    round += QStringLiteral("; told of changes as they happen");
                say(QStringLiteral("Rounds:   %1").arg(round));
                say(QStringLiteral("Cache:    %1").arg(size(status.value("cache_bytes").toDouble())));
                // Not counting this one.
                say(QStringLiteral("Windows:  %1 open").arg(std::max(0, status.value("windows").toInt() - 1)));
                done();
            });
        });
    }

    void sources() {
        call("sources", {}, [this](const QJsonObject &list) {
            const QString current = list.value("current").toString();
            const QJsonArray all = list.value("sources").toArray();
            if (all.isEmpty())
                say("No sources yet: pinhole sign-in, or pinhole show <folder>");
            for (const QJsonValue &v : all) {
                const QJsonObject s = v.toObject();
                say(QStringLiteral("%1 %2  %3  (%4 photos)")
                        .arg(s.value("id").toString() == current ? "*" : " ", s.value("name").toString(), s.value("id").toString(),
                             count(s.value("count").toInteger())));
            }
            done();
        });
    }

    void show(const QString &what) {
        call("sources", {}, [this, what](const QJsonObject &list) {
            QJsonObject params;
            if (const QJsonObject known = find(list.value("sources").toArray(), what); !known.isEmpty())
                params = {{"id", known.value("id")}};
            else if (QFileInfo(what).isDir())
                params = {{"kind", "folder"}, {"path", QDir::cleanPath(QFileInfo(what).absoluteFilePath())}};
            else if (what.contains('@'))
                params = {{"kind", "icloud"}, {"account", what}};
            else
                return fail("not_found", QStringLiteral("no source or folder %1; pinhole sources lists them").arg(what));
            call("set_source", params, [this](const QJsonObject &source) {
                say(QStringLiteral("Showing %1").arg(source.value("name").toString()));
                done();
            });
        });
    }

    void remove(const QString &what) {
        call("sources", {}, [this, what](const QJsonObject &list) {
            const QJsonObject known = find(list.value("sources").toArray(), what);
            if (known.isEmpty())
                return fail("not_found", QStringLiteral("no source %1; pinhole sources lists them").arg(what));
            call("remove_source", {{"id", known.value("id")}}, [this, known](const QJsonObject &) {
                say(QStringLiteral("Removed %1").arg(known.value("name").toString()));
                done();
            });
        });
    }

    void signOut(const QString &what) {
        call("sources", {}, [this, what](const QJsonObject &list) {
            const QJsonObject known = find(list.value("sources").toArray(), what);
            if (known.isEmpty())
                return fail("not_found", QStringLiteral("no source %1 among the sources (pinhole sources)").arg(what));
            call("sign_out", {{"id", known.value("id")}}, [this, known](const QJsonObject &) {
                const QString who = known.value("account").toString();
                say(QStringLiteral("Signed out of %1; its library stays until it is signed in to again")
                        .arg(who.isEmpty() ? known.value("name").toString() : who));
                done();
            });
        });
    }

    void signIn(const QStringList &given) {
        // Each kind is a plugin's (pinhole.icloud's): turned off, there is
        // nothing to sign in to, and no password is asked for.
        // The switches as plugins.json says them now, edited by hand too
        // (the service reads it again); then which kinds are on.
        m_service.call("plugins_changed", {}, [this, given](bool, const QJsonObject &, const QString &) {
            m_service.call("kinds", {}, [this, given](bool, const QJsonObject &kinds, const QString &) {
                QMap<QString, QJsonObject> byName;
                for (const QJsonValue &kind : kinds.value("kinds").toArray())
                    byName.insert(kind.toObject().value("kind").toString(), kind.toObject());
                // A kind named first, else iCloud's, an Apple ID after it
                // as before.
                const bool named = !given.isEmpty() && byName.contains(given.first());
                const QString kind = named ? given.first() : QStringLiteral("icloud");
                const QString account = named ? given.value(1) : given.value(0);
                if (!named && given.size() > 1)
                    return fail("bad_request", QStringLiteral("no kind %1: pinhole sign-in [kind] [account]").arg(given.first()));
                const QJsonObject known = byName.value(kind);
                if (!known.isEmpty() && !known.value("on").toBool()) {
                    note(QStringLiteral("pinhole: %1 is turned off. Turn its plugin on in Pinhole's side panel (Tab), under "
                                        "Plugins, then sign in.").arg(kind == QLatin1String("icloud") ? "iCloud" : kind));
                    return done(1);
                }
                if (!known.isEmpty() && !known.value("needs_sign_in").toBool())
                    return fail("bad_request", QStringLiteral("%1 needs no signing in: pinhole show adds one").arg(kind));
                if (kind == QLatin1String("icloud"))
                    return askAppleId(account);
                signInTo(kind, account);
            });
        });
    }

    // Another kind's: its account and password, and a code if it
    // asks for one, as its provider takes them (docs/source-providers.md).
    void signInTo(const QString &kind, QString account) {
        if (account.isEmpty())
            account = ask("Account: ");
        if (account.isEmpty())
            return usage();
        const QString password = ask("Password: ", true);
        call("sign_in", {{"kind", kind}, {"account", account}, {"password", password}},
             [this, kind, account](const QJsonObject &result) { signedInTo(kind, account, result); });
    }

    void signedInTo(const QString &kind, const QString &account, const QJsonObject &result, int tries = 0) {
        if (result.value("state").toString() != QLatin1String("needs_code")) {
            say(QStringLiteral("Signed in to %1 as %2. Pinhole shows its photos now; pinhole status says how far it is.").arg(kind, account));
            return done();
        }
        const QString code = ask("The code it sent you: ");
        m_service.call("submit_code", {{"kind", kind}, {"account", account}, {"code", code}},
                       [this, kind, account, tries](bool ok, const QJsonObject &result, const QString &error) {
            if (ok)
                return signedInTo(kind, account, result);
            if (error == QLatin1String("wrong_code") && tries < 2) {
                note("That code is not the one; try again");
                return signedInTo(kind, account, QJsonObject{{"state", "needs_code"}}, tries + 1);
            }
            fail(error, result.value("message").toString());
        });
    }

    void askAppleId(QString appleId) {
        if (appleId.isEmpty())
            appleId = ask("Apple ID: ");
        if (appleId.isEmpty())
            return usage();
        // icloudpd signed in to it already: its session, with no password.
        call("discover", {}, [this, appleId](const QJsonObject &found) {
            static const QRegularExpression notWord(QStringLiteral("[^A-Za-z0-9_]"));
            QString stem = appleId;
            stem.remove(notWord);
            if (found.value("icloudpd").toArray().contains(stem)) {
                note(QStringLiteral("Taking over icloudpd's session for %1").arg(appleId));
                m_service.call("sign_in", {{"kind", "icloud"}, {"apple_id", appleId}}, [this, appleId](bool ok, const QJsonObject &result, const QString &code) {
                    if (ok)
                        return signedIn(appleId, result);
                    if (code != QLatin1String("needs_sign_in"))
                        return fail(code, result.value("message").toString());
                    note("icloudpd's session is no longer good; signing in with the password");
                    withPassword(appleId);
                });
            } else {
                withPassword(appleId);
            }
        });
    }

    void withPassword(const QString &appleId) {
        const QString password = ask("Password: ", true);
        if (password.isEmpty())
            return fail("bad_request", "no password");
        call("sign_in", {{"kind", "icloud"}, {"apple_id", appleId}, {"password", password}},
             [this, appleId](const QJsonObject &result) { signedIn(appleId, result); });
    }

    void signedIn(const QString &appleId, const QJsonObject &result, int tries = 0) {
        if (result.value("state").toString() != QLatin1String("needs_code")) {
            say(QStringLiteral("Signed in as %1. Pinhole shows its photos and builds their library now; pinhole status says how far it is.").arg(appleId));
            return done();
        }
        const QString code = ask("The six-digit code Apple sent to your devices: ");
        m_service.call("submit_code", {{"kind", "icloud"}, {"apple_id", appleId}, {"code", code}},
                       [this, appleId, tries](bool ok, const QJsonObject &result, const QString &error) {
            if (ok)
                return signedIn(appleId, result);
            if (error == QLatin1String("wrong_code") && tries < 2) {
                note("That code is not the one; try again");
                return signedIn(appleId, QJsonObject{{"state", "needs_code"}}, tries + 1);
            }
            fail(error, result.value("message").toString());
        });
    }

    void sync() {
        call("status", {}, [this](const QJsonObject &status) {
            const QJsonObject source = status.value("source").toObject();
            if (source.value("kind").isNull())
                return fail("not_found", "no source to look in yet (pinhole sign-in, or pinhole show <folder>)");
            note(QStringLiteral("Looking for changes in %1…").arg(source.value("name").toString()));
            // Done when a round that started after this ends.
            QObject::connect(&m_service, &ServiceClient::announced, &m_service, [this](const QString &name, const QJsonObject &e) {
                if (name == QLatin1String("library_changed")) {
                    m_added += e.value("added").toInt();
                    m_removed += e.value("removed").toInt();
                } else if (name == QLatin1String("progress")) {
                    const QString phase = e.value("phase").toString();
                    if (phase == QLatin1String("done") || phase != m_phase || m_said.elapsed() > 2000) {
                        m_phase = phase;
                        m_said.restart();
                        if (phase != QLatin1String("done"))
                            note(QStringLiteral("%1 %2 of %3").arg(phase, count(e.value("done").toInteger()), count(e.value("total").toInteger())));
                    }
                } else if (name == QLatin1String("health")) {
                    m_health = e.value("state").toString();
                } else if (name == QLatin1String("rounds")) {
                    if (e.value("next").isNull()) {
                        m_roundStarted = true;
                    } else if (m_roundStarted) {
                        say(QStringLiteral("%1 new, %2 removed").arg(count(m_added), count(m_removed)));
                        if (m_health != QLatin1String("fine")) {
                            note(QStringLiteral("pinhole: the source is %1").arg(m_health));
                            return done(1);
                        }
                        done();
                    }
                }
            });
            m_said.start();
            call("sync", {}, [](const QJsonObject &) {});
        });
    }

    // --json: a line {"progress", "done", "total", "file"} as each file goes
    // (progress of all of them, 0 to 1), then {"place", "files"} with the
    // service's answer for each (docs/service.md, Adding): for what drops
    // files without a window, as Omarchy's bar does.
    void add(QStringList files) {
        if (files.removeAll(QStringLiteral("--json")) > 0)
            m_json = true;
        if (files.isEmpty())
            return usage();
        QJsonArray paths;
        for (const QString &file : files) {
            const QFileInfo info(file);
            if (!info.exists())
                return fail("not_found", QStringLiteral("no file %1").arg(file));
            paths.append(info.absoluteFilePath());
        }
        // Its progress, a line each tenth of a file's way.
        QObject::connect(&m_service, &ServiceClient::announced, &m_service, [this](const QString &name, const QJsonObject &e) {
            if (name != QLatin1String("adding") || e.value("file").isNull())
                return;
            const QString file = QFileInfo(e.value("file").toString()).fileName();
            const double total = e.value("bytes").toDouble();
            const int tenth = total > 0 ? int(e.value("sent").toDouble() * 10 / total) : 0;
            if (file != m_phase || tenth != m_tenth) {
                m_phase = file;
                m_tenth = tenth;
                const int done = e.value("done").toInt(), of = std::max(1, e.value("total").toInt());
                if (m_json)
                    say(QString::fromUtf8(QJsonDocument(QJsonObject{{"progress", (done + tenth / 10.0) / of},
                                                                    {"done", done}, {"total", of}, {"file", file}})
                                              .toJson(QJsonDocument::Compact)));
                else
                    note(QStringLiteral("%1 of %2: %3 (%4%)").arg(done + 1).arg(of).arg(file).arg(tenth * 10));
            }
        });
        call("add", {{"files", paths}}, [this](const QJsonObject &result) {
            int failed = 0;
            const QJsonArray answers = result.value("files").toArray();
            for (const QJsonValue &v : answers) {
                const QJsonObject f = v.toObject();
                const QString file = f.value("file").toString(), state = f.value("state").toString();
                if (state != QLatin1String("added") && state != QLatin1String("duplicate"))
                    ++failed;
                if (m_json)
                    continue;
                if (state == QLatin1String("added"))
                    say(QStringLiteral("%1: added").arg(file));
                else if (state == QLatin1String("duplicate"))
                    say(QStringLiteral("%1: there already").arg(file));
                else
                    say(QStringLiteral("%1: %2").arg(file, f.value("error").toObject().value("message").toString()));
                if (f.contains("note"))
                    say(QStringLiteral("  %1").arg(f.value("note").toString()));
            }
            if (m_json) {
                // What to call the place they went: iCloud, or a folder's name.
                const QJsonObject source = m_service.source();
                const QString place = source.value("kind").toString() == QLatin1String("icloud")
                                          ? QStringLiteral("iCloud") : source.value("name").toString();
                say(QString::fromUtf8(QJsonDocument(QJsonObject{{"place", place}, {"files", answers}})
                                          .toJson(QJsonDocument::Compact)));
            }
            done(failed ? 1 : 0);
        });
    }

    void storage(const QStringList &args) {
        QJsonObject change;
        for (int i = 0; i < args.size(); i += 2) {
            const QString flag = args[i], value = args.value(i + 1);
            bool ok = false;
            if (flag == "--limit" && value.toDouble(&ok) > 0 && ok)
                change.insert("limit_bytes", qint64(value.toDouble() * 1e9));
            else if (flag == "--move-to" && !value.isEmpty())
                change.insert("location", QDir::cleanPath(QFileInfo(value).absoluteFilePath()));
            else
                return usage();
        }
        call(change.isEmpty() ? "storage" : "set_storage", change, [this](const QJsonObject &s) {
            say(QStringLiteral("Folder:   %1").arg(s.value("location").toString()));
            say(QStringLiteral("Used:     %1 of %2").arg(size(s.value("used_bytes").toDouble()), size(s.value("limit_bytes").toDouble())));
            const QJsonObject kinds = s.value("kinds").toObject();
            QStringList parts;
            for (auto it = kinds.begin(); it != kinds.end(); ++it)
                parts.append(QStringLiteral("%1 %2").arg(it.key(), size(it.value().toDouble())));
            say(QStringLiteral("          %1").arg(parts.join(", ")));
            if (const QJsonObject moving = s.value("moving").toObject(); !moving.isEmpty())
                say(QStringLiteral("Moving:   %1 of %2").arg(size(moving.value("done_bytes").toDouble()), size(moving.value("total_bytes").toDouble())));
            done();
        });
    }

    // The photos as a drag out of the window hands them over (Outgoing,
    // compatible form), into a staging folder of their own unless --to
    // names one. Each as it is ready: its path, or with --json a line
    // {"id", "path", "source"} or {"id", "error"}.
    void exportPhotos(const QStringList &args) {
        QStringList ids;
        QString folder;
        // After "--" every word is an id, one beginning with "-" too.
        bool options = true;
        for (int i = 0; i < args.size(); ++i) {
            if (!options)
                ids.append(args[i]);
            else if (args[i] == QLatin1String("--"))
                options = false;
            else if (args[i] == QLatin1String("--json"))
                m_json = true;
            else if (args[i] == QLatin1String("--to") && i + 1 < args.size())
                folder = QDir::cleanPath(QFileInfo(args[++i]).absoluteFilePath());
            else if (args[i].startsWith(QLatin1Char('-')))
                return usage();
            else
                ids.append(args[i]);
        }
        if (ids.isEmpty())
            return usage();
        // The library the service shows: a plugin's source's too, whose
        // folder only it knows.
        call("status", {}, [this, ids, folder](const QJsonObject &status) {
            const QString shown = status.value("library").toString();
            exportFrom(shown.isEmpty() ? Storage::library() : shown, ids, folder);
        });
    }

    void exportFrom(const QString &dir, const QStringList &ids, const QString &folder) {
        QString error;
        if (!m_library.open(dir, &error))
            return fail("not_found", error);
        m_library.applyAdjustments(AdjustmentStore::load(m_library.dir()));
        QHash<QByteArray, int> at;
        for (int i = 0; i < m_library.count(); ++i)
            at.insert(m_library.at(i).id, i);
        m_service.setShown(m_library.source());
        m_outgoing = new Outgoing(&m_library, &m_service, &m_service);
        m_left = int(ids.size());
        for (const QString &id : ids) {
            const auto found = at.constFind(id.toUtf8());
            if (found == at.constEnd()) {
                exported(id, {}, QStringLiteral("not in the library shown"));
                continue;
            }
            m_outgoing->prepare(m_outgoing->snapshot({found.value()}), true, folder,
                                [this, id](const QStringList &files, int) {
                                    exported(id, files.value(0),
                                             files.isEmpty() ? QStringLiteral("could not be fetched") : QString());
                                }, true);
        }
    }

    void exported(const QString &id, const QString &path, const QString &error) {
        if (!error.isEmpty())
            ++m_failed;
        if (m_json)
            // With the source it is of: an id names a photo in one source only.
            say(QString::fromUtf8(QJsonDocument(error.isEmpty() ? QJsonObject{{"id", id}, {"path", path},
                                                                            {"source", m_library.source()}}
                                                                : QJsonObject{{"id", id}, {"error", error}})
                                      .toJson(QJsonDocument::Compact)));
        else if (error.isEmpty())
            say(path);
        else
            note(QStringLiteral("pinhole: %1: %2").arg(id, error));
        if (--m_left == 0)
            done(m_failed ? 1 : 0);
    }

    QStringList m_args;
    // export: the library shown, and what makes its files.
    Library m_library;
    Outgoing *m_outgoing = nullptr;
    int m_left = 0, m_failed = 0;
    bool m_json = false;
    // The service this command asks; started if none is running.
    ServiceClient m_service{QString()};
    // sync and add: what has been said so far.
    QString m_phase, m_health = QStringLiteral("fine");
    int m_added = 0, m_removed = 0, m_tenth = -1;
    bool m_roundStarted = false;
    QElapsedTimer m_said;
};

} // namespace

int runCommand(int argc, char *argv[]) {
    // No command, or the window's own options (--library, --bench): the window.
    if (argc < 2 || argv[1][0] == '-')
        return -1;
    // A command misspelt opens no window.
    if (!kCommands.contains(QString::fromLocal8Bit(argv[1]))) {
        std::fprintf(stderr, "pinhole: no command %s\n\n%s", argv[1], kUsage);
        return 2;
    }
    if (qstrcmp(argv[1], "recent") == 0)
        return runRecent(argc, argv);
    if (qstrcmp(argv[1], "help") == 0) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    QCoreApplication app(argc, argv);
    app.setApplicationName("pinhole");
    Command command(app.arguments().mid(1));
    QTimer::singleShot(0, &app, [&command] { command.start(); });
    return app.exec();
}
