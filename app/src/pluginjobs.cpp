#include "plugins.h"
#include "adjustment.h"
#include "encoder.h"
#include "favourites.h"
#include "library.h"
#include "outgoing.h"
#include "portal.h"
#include "selftest.h"
#include "serviceclient.h"
#include "storage.h"

#include <QDeadlineTimer>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValueIterator>
#include <QJsonArray>
#include <QPointer>
#include <QMimeData>
#include <QProcess>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

#include <signal.h>
#include <unistd.h>

namespace {

// An item's frames on their way into ffmpeg: each frame is asked for
// (seek), taken from the item as the window draws it, and written to
// ffmpeg's input, into a file beside the one wanted until it is whole.
class Recording : public QObject {
public:
    // The file made, or why not: code "cancelled" or "failed", and what
    // went wrong.
    using Done = std::function<void(const QString &path, const QString &code, const QString &error)>;

    Recording(PluginHost *host, QQuickItem *item, const QJSValue &options, const QJSValue &progress, Done done)
        : QObject(host), m_host(host), m_item(item), m_seek(options.property("seek")), m_progress(progress),
          m_done(std::move(done)) {
        m_path = options.property("path").toString();
        m_format = options.property("format").toString() == QLatin1String("mp4") ? "mp4" : "gif";
        // Even sizes, which H.264 wants.
        m_size = QSize(std::max(2, options.property("width").toInt() & ~1), std::max(2, options.property("height").toInt() & ~1));
        m_fps = std::max(1, options.property("fps").toInt());
        m_frames = std::max(1, options.property("frames").toInt());
    }

    void start() {
        if (!m_item || m_path.isEmpty())
            return fail(QStringLiteral("nothing to record"));
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        QStringList args{"-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "bgra",
                         "-s", QStringLiteral("%1x%2").arg(m_size.width()).arg(m_size.height()),
                         "-r", QString::number(m_fps), "-i", "-"};
        if (m_format == QLatin1String("gif"))
            args << "-vf" << "split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=sierra2_4a"
                 << "-loop" << "0" << "-f" << "gif";
        else
            args << "-c:v" << "libx264" << "-preset" << "medium" << "-crf" << "23" << "-pix_fmt" << "yuv420p"
                 << "-movflags" << "+faststart" << "-f" << "mp4";
        args << part();
        m_encoder = new FrameEncoder(QStringLiteral("ffmpeg"), args, m_size, this);
        connect(m_encoder, &FrameEncoder::ready, this, &Recording::next);
        connect(m_encoder, &FrameEncoder::finished, this, [this](const QString &error) {
            if (!error.isEmpty())
                return fail(error);
            QFile::remove(m_path);
            if (!QFile::rename(part(), m_path))
                return fail(QStringLiteral("could not write %1").arg(m_path));
            finish(m_path, {}, {});
        });
        m_encoder->start();
        next();
    }

    // Cancelled: it ends saying so.
    void cancel() { fail(QStringLiteral("cancelled"), QStringLiteral("cancelled")); }

    // The plugin gone: it ends saying nothing.
    void stop() {
        m_done = nullptr;
        fail({});
    }

private:
    QString part() const { return m_path + QStringLiteral(".part"); }

    void next() {
        if (m_ended || m_grab)
            return;
        if (m_at == m_frames) {
            m_encoder->end();
            return;
        }
        if (!m_item)
            return fail(QStringLiteral("the picture went away"));
        m_host->call(m_seek, {m_at});
        // The seek may have ended it (an error thrown, the plugin stopped).
        if (m_ended)
            return;
        m_grab = m_item->grabToImage(m_size);
        if (!m_grab)
            return fail(QStringLiteral("the picture could not be taken"));
        connect(m_grab.data(), &QQuickItemGrabResult::ready, this, [this] {
            const QImage frame = m_grab->image();
            m_grab.reset();
            if (m_ended)
                return;
            ++m_at;
            m_host->call(m_progress, {double(m_at) / m_frames});
            // The next once ffmpeg has caught up (ready), if it is behind.
            if (m_encoder->add(frame))
                next();
        });
    }

    void fail(const QString &error, const QString &code = QStringLiteral("failed")) {
        if (m_ended)
            return;
        if (m_encoder)
            m_encoder->stop();
        QFile::remove(part());
        finish({}, code, error.isEmpty() ? QStringLiteral("ffmpeg failed") : error);
    }

    void finish(const QString &path, const QString &code, const QString &error) {
        m_ended = true;
        if (m_done)
            std::exchange(m_done, nullptr)(path, code, error);
        deleteLater();
    }

    PluginHost *m_host;
    QPointer<QQuickItem> m_item;
    QJSValue m_seek, m_progress;
    Done m_done;
    QString m_path, m_format;
    QSize m_size;
    int m_fps = 20, m_frames = 1, m_at = 0;
    bool m_ended = false;
    FrameEncoder *m_encoder = nullptr;
    QSharedPointer<QQuickItemGrabResult> m_grab;
};

// An error thrown in a plugin, where it was: "Main.qml:12: TypeError: …".
QString thrown(const QJSValue &error) {
    const QString file = QUrl(error.property("fileName").toString()).fileName();
    const int line = error.property("lineNumber").toInt();
    return file.isEmpty() ? error.toString() : QStringLiteral("%1:%2: %3").arg(file).arg(line).arg(error.toString());
}

} // namespace

void PluginJob::cancel() {
    if (m_cancel)
        std::exchange(m_cancel, nullptr)();
}

std::vector<QProcess *> PluginHost::close() {
    // Its programs asked to end (SIGTERM), as when turned off; waited for
    // by whoever takes them, as no event loop is left to.
    m_closing = true;
    stop();
    return std::exchange(m_ending, {});
}

void PluginHost::endPrograms(const std::vector<QProcess *> &programs) {
    // A second all told, however many there are; then killed, and reaped.
    QDeadlineTimer second(1000);
    for (QProcess *process : programs)
        process->waitForFinished(int(std::max<qint64>(0, second.remainingTime())));
    for (QProcess *process : programs)
        if (process->state() != QProcess::NotRunning)
            killProgram(process);
    for (QProcess *process : programs) {
        if (process->state() != QProcess::NotRunning)
            process->waitForFinished(1000);
        delete process;
    }
}

QJSValue PluginHost::chooseFolder(const QString &title) {
    auto deferred = std::make_shared<Deferred>();
    const std::weak_ptr<Deferred> weak = deferred;
    const int generation = m_generation;
    *deferred = defer([this, weak, generation] {
        if (const auto d = weak.lock())
            settle(generation, d->reject, error(QStringLiteral("cancelled"), tr("cancelled")));
    });
    auto *picker = new PortalFolderPicker(this);
    connect(picker, &PortalFolderPicker::picked, this, [this, picker, deferred, generation](const QString &folder) {
        picker->deleteLater();
        if (folder.isEmpty())
            settle(generation, deferred->reject, error(QStringLiteral("cancelled"), tr("cancelled")));
        else
            settle(generation, deferred->resolve, folder);
    });
    picker->pick(title);
    return deferred->promise;
}

QJSEngine *PluginHost::engine() const {
    if (QJSEngine *engine = m_plugins->engine())
        return engine;
    return qjsEngine(this);
}

PluginHost::Deferred PluginHost::defer(const std::function<void()> &cancel) {
    QJSEngine *js = engine();
    if (!js)
        return {};
    // One for each engine, gone with it.
    static QHash<QJSEngine *, QJSValue> factories;
    if (!factories.contains(js))
        connect(js, &QObject::destroyed, js, [js] { factories.remove(js); });
    QJSValue &factory = factories[js];
    if (factory.isUndefined())
        // Every promise made from it with then() and catch() can cancel too,
        // and cancels the whole chain: this job and any Pinhole job a step
        // of it returned, so `job.then(…).then(…)` kept and cancelled later
        // stops what it is waiting on, rather than throwing.
        factory = js->evaluate(QStringLiteral(R"((function (job) {
            let resolve, reject
            const promise = new Promise(function (a, b) { resolve = a; reject = b })
            const jobs = [job]
            const cancel = function () { for (const each of jobs.splice(0)) each.cancel() }
            const follow = function (step) {
                if (typeof step !== "function") return step
                return function (value) {
                    const next = step(value)
                    if (next && typeof next.cancel === "function") jobs.push(next)
                    return next
                }
            }
            // A promise's own then can't be replaced here (V4 keeps the
            // prototype's), so each step is a thenable around the promise.
            const carry = function (made) {
                return {
                    then: function (done, failed) { return carry(made.then(follow(done), follow(failed))) },
                    catch: function (failed) { return carry(made.then(undefined, follow(failed))) },
                    cancel: cancel
                }
            }
            return { promise: carry(promise), resolve: resolve, reject: reject }
        }))"));
    const QJSValue made = factory.call({js->newQObject(new PluginJob(cancel))});
    return {made.property("promise"), made.property("resolve"), made.property("reject")};
}

QJSValue PluginHost::error(const QString &code, const QString &message) const {
    QJSEngine *js = engine();
    if (!js)
        return {};
    QJSValue error = js->newErrorObject(QJSValue::GenericError, message.isEmpty() ? code : message);
    error.setProperty("code", code);
    return error;
}

void PluginHost::settle(int generation, const QJSValue &how, const QJSValue &value) {
    if (generation == m_generation && how.isCallable())
        QJSValue(how).call({value});
}

void PluginHost::call(const QJSValue &function, const QJSValueList &args) {
    if (!function.isCallable())
        return;
    const QJSValue result = QJSValue(function).call(args);
    if (result.isError())
        report(thrown(result));
}

void PluginHost::track(QObject *job, std::function<void()> stop) {
    std::erase_if(m_running, [](const auto &running) { return running.first.isNull(); });
    m_running.emplace_back(job, std::move(stop));
}

void PluginHost::stop() {
    ++m_generation;
    if (std::exchange(m_away, false))
        emit awayChanged();
    m_awayInterrupted = false;
    m_plugins->keepAwake()->hold(this, false);
    for (auto &[job, stop] : std::exchange(m_running, {}))
        if (job)
            stop();
    if (!m_problem.isEmpty()) {
        m_problem.clear();
        emit problemChanged();
        m_plugins->touched();
    }
}

QJSValue PluginHost::fetch(const QJSValue &photo, const QString &kind, const QJSValue &done) {
    const Library *data = m_plugins->libraryData();
    // Only this library's photos: a position, or an id it has.
    QByteArray id;
    if (const int index = m_plugins->library()->positionOf(photo); index >= 0)
        id = data->at(index).id;
    const int generation = m_generation;
    // An object in done's place is options (API 3).
    const bool stream = !done.isCallable() && done.isObject() && done.property(QStringLiteral("stream")).toBool();

    if (done.isCallable()) {
        if (id.isEmpty()) {
            call(done, {QString(), QStringLiteral("not_found")});
            return {};
        }
        m_plugins->service()->fetch(id, kind, [this, generation, done](bool ok, const QJsonObject &result, const QString &code) {
            if (generation == m_generation)
                call(done, {ok ? result.value("path").toString() : QString(), ok ? QString() : code});
        });
        return {};
    }

    // The service's answer cannot be stopped, only no longer waited for.
    auto deferred = std::make_shared<Deferred>();
    const std::weak_ptr<Deferred> weak = deferred;
    *deferred = defer([this, weak, generation] {
        if (const auto d = weak.lock())
            settle(generation, d->reject, error(QStringLiteral("cancelled"), tr("cancelled")));
    });
    const QJSValue promise = deferred->promise;
    if (id.isEmpty()) {
        settle(generation, deferred->reject, error(QStringLiteral("not_found"), tr("no such photo")));
        return promise;
    }
    m_plugins->service()->fetch(id, kind, [this, deferred, generation, stream](bool ok, const QJsonObject &result, const QString &code) {
        if (!ok) {
            settle(generation, deferred->reject, error(code, result.value("message").toString()));
            return;
        }
        const QString path = result.value("path").toString();
        // To play: the service's own URL while it still downloads.
        const QString streamed = result.value("stream").toString();
        settle(generation, deferred->resolve,
               !stream ? path : !streamed.isEmpty() ? streamed : QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
    }, stream);
    return promise;
}

QJSValue PluginHost::ask(const QString &name, const QJSValue &request, const QJSValue &options) {
    // The worker's answer cannot be stopped, only no longer waited for.
    auto deferred = std::make_shared<Deferred>();
    const std::weak_ptr<Deferred> weak = deferred;
    const int generation = m_generation;
    *deferred = defer([this, weak, generation] {
        if (const auto d = weak.lock())
            settle(generation, d->reject, error(QStringLiteral("cancelled"), tr("cancelled")));
    });
    const QJSValue promise = deferred->promise;
    QJsonObject params{{"plugin", m_id}, {"name", name}, {"request", QJsonValue::fromVariant(request.toVariant())}};
    if (const QJSValue timeout = options.property(QStringLiteral("timeout")); timeout.isNumber() && timeout.toNumber() > 0)
        params.insert("timeout", qint64(timeout.toNumber()));
    m_plugins->service()->call(QStringLiteral("ask"), params, [this, deferred, generation](bool ok, const QJsonObject &result, const QString &code) {
        if (!ok) {
            settle(generation, deferred->reject, error(code, result.value("message").toString()));
            return;
        }
        if (QJSEngine *js = engine())
            settle(generation, deferred->resolve, js->toScriptValue(result.value("result").toVariant()));
    });
    return promise;
}

QJSValue PluginHost::setFavorite(const QJSValue &photos, bool favorite) {
    // The service's answer cannot be stopped, only no longer waited for:
    // the photos are set, or put back, either way.
    auto deferred = std::make_shared<Deferred>();
    const std::weak_ptr<Deferred> weak = deferred;
    const int generation = m_generation;
    *deferred = defer([this, weak, generation] {
        if (const auto d = weak.lock())
            settle(generation, d->reject, error(QStringLiteral("cancelled"), tr("cancelled")));
    });
    const QJSValue promise = deferred->promise;
    Favourites *favourites = m_plugins->favourites();
    if (!favourites) {
        settle(generation, deferred->reject, error(QStringLiteral("unsupported"), QStringLiteral("no favourites here")));
        return promise;
    }
    // Only this library's photos: positions, or ids it has.
    const Library *data = m_plugins->libraryData();
    QList<QByteArray> ids;
    const auto add = [this, data, &ids](const QJSValue &photo) {
        if (const int index = m_plugins->library()->positionOf(photo); index >= 0)
            ids.append(data->at(index).id);
    };
    if (photos.isArray()) {
        const int length = photos.property(QStringLiteral("length")).toInt();
        for (int i = 0; i < length; ++i)
            add(photos.property(quint32(i)));
    } else {
        add(photos);
    }
    if (ids.isEmpty()) {
        settle(generation, deferred->reject, error(QStringLiteral("not_found"), tr("no such photo")));
        return promise;
    }
    // Known once set() returns; its answer comes before that only when
    // there is none to change.
    auto changed = std::make_shared<QList<QByteArray>>();
    *changed = favourites->set(ids, favorite, [this, deferred, generation, changed](const QString &code, const QString &message) {
        if (!code.isEmpty()) {
            settle(generation, deferred->reject, error(code, message));
            return;
        }
        QJSEngine *js = engine();
        if (!js)
            return;
        QJSValue list = js->newArray(quint32(changed->size()));
        for (int i = 0; i < changed->size(); ++i)
            list.setProperty(quint32(i), QString::fromUtf8(changed->at(i)));
        QJSValue result = js->newObject();
        result.setProperty(QStringLiteral("changed"), list);
        settle(generation, deferred->resolve, result);
    });
    return promise;
}

namespace {
// A plugin's program runs in a process group of its own, so that what it
// starts (a shell's magick or ffmpeg) is asked to end and killed with it,
// never left running when it is cancelled, times out or the plugin goes.
void ownGroup(QProcess *process) {
    process->setChildProcessModifier([] { ::setpgid(0, 0); });
}

void signalGroup(QProcess *process, int signal) {
    if (const qint64 pid = process->processId(); pid > 0)
        ::kill(-pid_t(pid), signal);
}
} // namespace

void PluginHost::endProgram(QProcess *process) { signalGroup(process, SIGTERM); }
void PluginHost::killProgram(QProcess *process) { signalGroup(process, SIGKILL); }

QJSValue PluginHost::run(const QString &program, const QStringList &arguments, const QJSValue &options) {
    // What check refuses, however its name was made: not run.
    if (const QString refused = Plugins::refusedRun(program, arguments); !refused.isEmpty()) {
        qWarning().noquote() << m_id + ":" << refused;
        if (options.isCallable()) {
            call(options, {-1, QString(), refused});
            return {};
        }
        const Deferred deferred = defer([] {});
        settle(m_generation, deferred.reject, error(QStringLiteral("refused"), refused));
        return deferred.promise;
    }
    QString path = program;
    if (!program.contains('/') && QFileInfo(m_dir + '/' + program).isExecutable())
        path = m_dir + '/' + program;
    auto *process = new QProcess(this);
    process->setWorkingDirectory(m_dir);
    ownGroup(process);
    track(process, [this, process] {
        // Nothing more said of it, and it goes once it has ended: asked to
        // end (SIGTERM) and given a second to tidy up, as cancel() does,
        // the plugin turned off or loaded again (pinhole-send tells the
        // phone, Background's helper takes away its part file). Its own,
        // so that a host going too does not kill it at once.
        disconnect(process, nullptr, this, nullptr);
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
        if (process->state() == QProcess::NotRunning) {
            process->deleteLater();
            return;
        }
        process->setParent(nullptr);
        endProgram(process);
        if (m_closing) {
            disconnect(process, &QProcess::finished, process, &QObject::deleteLater);
            return m_ending.push_back(process);
        }
        QTimer::singleShot(1000, process, [process] {
            if (process->state() != QProcess::NotRunning)
                killProgram(process);
        });
    });

    if (options.isCallable()) {
        const QJSValue done = options;
        connect(process, &QProcess::finished, this, [this, process, done](int code, QProcess::ExitStatus status) {
            call(done, {status == QProcess::NormalExit ? code : -1, QString::fromUtf8(process->readAllStandardOutput()),
                        QString::fromUtf8(process->readAllStandardError())});
            process->deleteLater();
        });
        connect(process, &QProcess::errorOccurred, this, [this, process, done, path](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            call(done, {-1, QString(), QStringLiteral("%1 did not start").arg(path)});
            process->deleteLater();
        });
        process->start(path, arguments);
        // As with a promise: its stdin ends at once.
        process->closeWriteChannel();
        return {};
    }

    const int generation = m_generation;
    // Why it was stopped, if it was: "cancelled", "timeout".
    auto why = std::make_shared<QString>();
    const QPointer<QProcess> running = process;
    // Asked to end (SIGTERM), so it can tidy up as it goes (pinhole-send
    // tells the phone); killed if it has not a second later.
    const Deferred deferred = defer([running, why] {
        if (running && running->state() != QProcess::NotRunning) {
            *why = QStringLiteral("cancelled");
            endProgram(running);
            QTimer::singleShot(1000, running.data(), [running] {
                if (running->state() != QProcess::NotRunning)
                    killProgram(running);
            });
        }
    });
    // Its whole lines to output(line), as they come: what is left at the
    // end is the result's stdout.
    const QJSValue output = options.property("output");
    auto lines = [this, process, output, generation] {
        while (process->canReadLine()) {
            QByteArray line = process->readLine();
            line.chop(1);
            if (generation == m_generation)
                call(output, {QString::fromUtf8(line)});
        }
    };
    if (output.isCallable())
        connect(process, &QProcess::readyReadStandardOutput, this, lines);
    if (QJSValue env = options.property("env"); env.isObject()) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (QJSValueIterator it(env); it.hasNext();) {
            it.next();
            environment.insert(it.name(), it.value().toString());
        }
        process->setProcessEnvironment(environment);
    }
    if (const int timeout = options.property("timeout").toInt(); timeout > 0)
        QTimer::singleShot(timeout, process, [process, why] {
            if (process->state() != QProcess::NotRunning) {
                *why = QStringLiteral("timeout");
                killProgram(process);
            }
        });
    connect(process, &QProcess::finished, this, [this, process, deferred, why, generation, path, output, lines](int code, QProcess::ExitStatus status) {
        if (output.isCallable())
            lines();
        const QString out = QString::fromUtf8(process->readAllStandardOutput());
        const QString err = QString::fromUtf8(process->readAllStandardError());
        process->deleteLater();
        if (!why->isEmpty())
            return settle(generation, deferred.reject,
                          error(*why, *why == QLatin1String("timeout") ? tr("%1 took too long").arg(QFileInfo(path).fileName())
                                                                       : tr("cancelled")));
        if (status != QProcess::NormalExit || code != 0) {
            QJSValue e = error(QStringLiteral("exit"),
                               !err.trimmed().isEmpty() ? err.trimmed()
                               : status == QProcess::NormalExit ? tr("%1 exited with %2").arg(QFileInfo(path).fileName()).arg(code)
                                                                : tr("%1 crashed").arg(QFileInfo(path).fileName()));
            e.setProperty("exitCode", status == QProcess::NormalExit ? code : -1);
            e.setProperty("stdout", out);
            e.setProperty("stderr", err);
            return settle(generation, deferred.reject, e);
        }
        QJSEngine *js = engine();
        if (!js)
            return;
        QJSValue result = js->newObject();
        result.setProperty("code", code);
        result.setProperty("stdout", out);
        result.setProperty("stderr", err);
        settle(generation, deferred.resolve, result);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, deferred, generation, path](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        settle(generation, deferred.reject, error(QStringLiteral("start"), tr("%1 did not start").arg(path)));
        process->deleteLater();
    });
    process->start(path, arguments);
    // Its stdin ends after what it is given, or at once: a program that
    // reads it to the end (jq, cat) never waits for more that cannot come.
    if (const QJSValue input = options.property("input"); !input.isUndefined() && !input.isNull())
        process->write(input.toString().toUtf8());
    process->closeWriteChannel();
    return deferred.promise;
}

QJSValue PluginHost::record(QQuickItem *item, const QJSValue &options, const QJSValue &progress, const QJSValue &done) {
    const QJSValue onProgress = progress.isCallable() ? progress : options.property("progress");
    if (done.isCallable()) {
        auto *recording = new Recording(this, item, options, onProgress, [this, done](const QString &path, const QString &, const QString &e) {
            call(done, {path, e});
        });
        track(recording, [recording] { recording->stop(); });
        recording->start();
        return {};
    }
    const int generation = m_generation;
    auto deferred = std::make_shared<Deferred>();
    auto *recording = new Recording(this, item, options, onProgress, [this, deferred, generation](const QString &path, const QString &code, const QString &e) {
        if (code.isEmpty())
            settle(generation, deferred->resolve, path);
        else
            settle(generation, deferred->reject, error(code, e));
    });
    const QPointer<Recording> running = recording;
    *deferred = defer([running] {
        if (running)
            running->cancel();
    });
    const QJSValue promise = deferred->promise;
    track(recording, [recording] { recording->stop(); });
    recording->start();
    return promise;
}

QJSValue PluginHost::copyFile(const QString &path) {
    const int generation = m_generation;
    // Nothing to stop: the clipboard has it a moment later either way.
    const Deferred deferred = defer([] {});
    if (!QFileInfo::exists(path)) {
        settle(generation, deferred.reject, error(QStringLiteral("not_found"), tr("no such file: %1").arg(path)));
        return deferred.promise;
    }
    // As Pinhole's own copy: an image pastes into a chat as one. Another
    // copy asked for while this one was made wins: this one `cancelled`.
    Outgoing::toClipboard(
        {path}, this, [this, deferred, generation, path] { settle(generation, deferred.resolve, path); },
        [this, deferred, generation] {
            settle(generation, deferred.reject, error(QStringLiteral("cancelled"), tr("another copy came after it")));
        });
    return deferred.promise;
}

QJSValue PluginHost::copyText(const QString &text) {
    const Deferred deferred = defer([] {});
    auto *data = new QMimeData;
    data->setText(text);
    setClipboard(data);
    settle(m_generation, deferred.resolve, text);
    return deferred.promise;
}

// A file of its own read, away from the window: one in its folder or its
// cache (what its worker wrote there), whole, as text or as JSON (API 13).
QJSValue PluginHost::readText(const QString &path) { return readFile(path, false); }
QJSValue PluginHost::readJson(const QString &path) { return readFile(path, true); }

QJSValue PluginHost::readFile(const QString &path, bool json) {
    const int generation = m_generation;
    const Deferred deferred = defer([] {});
    const QString file = QFileInfo(path).canonicalFilePath();
    const QString cache = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/pinhole/plugins/"
                               + m_id).canonicalPath();
    const QString own = QDir(m_dir).canonicalPath();
    const auto inside = [&file](const QString &root) { return !root.isEmpty() && file.startsWith(root + '/'); };
    if (file.isEmpty() || !QFileInfo(file).isFile()) {
        settle(generation, deferred.reject, error(QStringLiteral("not_found"), tr("no such file: %1").arg(path)));
        return deferred.promise;
    }
    if (!inside(own) && !inside(cache)) {
        settle(generation, deferred.reject,
               error(QStringLiteral("not_yours"), tr("only a file in the plugin's folder or its cache: %1").arg(path)));
        return deferred.promise;
    }
    // Whole, so bounded: a plugin's own state, not a library.
    constexpr qint64 most = 32 * 1024 * 1024;
    struct Read { QByteArray bytes; QString code; };
    auto *watcher = new QFutureWatcher<Read>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, deferred, generation, json, path] {
        watcher->deleteLater();
        const Read read = watcher->result();
        if (read.code == QLatin1String("too_large")) {
            settle(generation, deferred.reject, error(read.code, tr("larger than 32 MB: %1").arg(path)));
            return;
        }
        if (!read.code.isEmpty()) {
            settle(generation, deferred.reject, error(read.code, tr("could not read %1").arg(path)));
            return;
        }
        QJSEngine *js = engine();
        if (!js)
            return;
        if (!json) {
            settle(generation, deferred.resolve, QString::fromUtf8(read.bytes));
            return;
        }
        QJsonParseError parsed;
        const QJsonDocument document = QJsonDocument::fromJson(read.bytes, &parsed);
        if (parsed.error != QJsonParseError::NoError) {
            settle(generation, deferred.reject,
                   error(QStringLiteral("json"), tr("%1 is not JSON: %2").arg(path, parsed.errorString())));
            return;
        }
        settle(generation, deferred.resolve, js->toScriptValue(document.toVariant()));
    });
    watcher->setFuture(QtConcurrent::run([file] {
        QFile f(file);
        if (f.size() > most)
            return Read{{}, QStringLiteral("too_large")};
        if (!f.open(QIODevice::ReadOnly))
            return Read{{}, QStringLiteral("unreadable")};
        return Read{f.readAll(), {}};
    }));
    return deferred.promise;
}

void PluginHost::warn(const QJSValue &error) {
    // What the plugin cancelled itself, or what the library read again
    // stopped, is no fault: .catch(pinhole.warn) at the end of every chain
    // is right as it is, with no test for either code before it.
    const QString code = error.isObject() ? error.property(QStringLiteral("code")).toString() : QString();
    if (code == QLatin1String("cancelled") || code == QLatin1String("reloaded"))
        return;
    report(error.isError() ? thrown(error) : error.toString());
}

QList<QByteArray> PluginHost::idsOf(const QJSValue &photos) const {
    const Library *data = m_plugins->libraryData();
    QList<QByteArray> ids;
    const auto add = [this, data, &ids](const QJSValue &photo) {
        if (const int index = m_plugins->library()->positionOf(photo); index >= 0)
            ids.append(data->at(index).id);
    };
    if (photos.isArray()) {
        const int length = photos.property(QStringLiteral("length")).toInt();
        for (int i = 0; i < length; ++i)
            add(photos.property(quint32(i)));
    } else {
        add(photos);
    }
    return ids;
}

namespace {
QVariantMap mapOf(const Adjustment &a) {
    return {{"turn", a.turn},
            {"crop", QVariantMap{{"x", a.crop.x()}, {"y", a.crop.y()}, {"width", a.crop.width()}, {"height", a.crop.height()}}}};
}
} // namespace

Adjustment PluginHost::currentAdjustment(const QByteArray &id) const {
    // Kept here (a folder's, or one being saved), else its source's.
    if (AdjustmentStore *store = m_plugins->adjustments(); store && !store->of(id).isIdentity())
        return store->of(id);
    const Library *data = m_plugins->libraryData();
    if (const int at = m_plugins->library()->positionOf(QJSValue(QString::fromUtf8(id))); at >= 0)
        return data->at(at).sourceEdit;
    return {};
}

QVariantMap PluginHost::adjustment(const QJSValue &photo) const {
    const QList<QByteArray> ids = idsOf(photo);
    if (ids.isEmpty())
        return mapOf(Adjustment());
    QVariantMap map = mapOf(currentAdjustment(ids.first()));
    // Where it is kept: "source" (its renditions are of it already), "here"
    // or "" for none.
    AdjustmentStore *store = m_plugins->adjustments();
    const bool here = store && !store->of(ids.first()).isIdentity();
    map.insert("kept", here ? QStringLiteral("here") : currentAdjustment(ids.first()).isIdentity() ? QString() : QStringLiteral("source"));
    return map;
}

QJSValue PluginHost::adjust(const QJSValue &photos, const QVariantMap &adjustment) {
    Adjustments targets;
    for (const QByteArray &id : idsOf(photos)) {
        Adjustment a = currentAdjustment(id);
        if (adjustment.contains("turn"))
            a.turn = adjustment.value("turn").toInt();
        if (adjustment.contains("crop")) {
            const QVariantMap crop = adjustment.value("crop").toMap();
            a.crop = crop.isEmpty() ? QRectF(0, 0, 1, 1)
                                    : QRectF(crop.value("x").toDouble(), crop.value("y").toDouble(),
                                             crop.value("width", 1).toDouble(), crop.value("height", 1).toDouble());
        }
        targets.insert(id, a.normalized());
    }
    return applyAdjustments(targets);
}

QJSValue PluginHost::turn(const QJSValue &photos, int quarters) {
    Adjustments targets;
    for (const QByteArray &id : idsOf(photos))
        targets.insert(id, currentAdjustment(id).turned(quarters));
    return applyAdjustments(targets);
}

// Where the source keeps edits (iCloud), there: each photo's original
// fetched, then set_edit, one after another; the service reads the
// library again once each is in. Where it keeps none (a folder), here
// (AdjustmentStore). A promise of what was done: { photos: [{ id, turn,
// crop }], kept: "source" or "here" }.
QJSValue PluginHost::applyAdjustments(const Adjustments &targets) {
    const int generation = m_generation;
    const Deferred deferred = defer([] {});
    AdjustmentStore *store = m_plugins->adjustments();
    ServiceClient *service = m_plugins->service();
    const Library *data = m_plugins->libraryData();
    if (targets.isEmpty() || !store) {
        settle(generation, deferred.reject, error(QStringLiteral("not_found"), tr("no such photo")));
        return deferred.promise;
    }
    auto done = [this, generation, deferred, targets](const QString &kept) {
        QVariantList photos;
        for (auto it = targets.begin(); it != targets.end(); ++it) {
            QVariantMap one = mapOf(it.value());
            one.insert("id", QString::fromUtf8(it.key()));
            photos.append(one);
        }
        settle(generation, deferred.resolve, engine()->toScriptValue(QVariantMap{{"photos", photos}, {"kept", kept}}));
    };
    const QString source = data->source();
    // A library no source fills (pinhole-fake's), a folder's, or no
    // service: here.
    if (!service || source.isEmpty() || source == QLatin1String("fake") || source.startsWith(QLatin1String("folder:"))) {
        if (!store->set(targets)) {
            settle(generation, deferred.reject, error(QStringLiteral("unavailable"), tr("could not keep the edit")));
            return deferred.promise;
        }
        done(QStringLiteral("here"));
        return deferred.promise;
    }
    auto left = std::make_shared<QList<QByteArray>>(targets.keys());
    auto next = std::make_shared<std::function<void()>>();
    const QPointer<PluginHost> alive(this);
    *next = [=]() {
        if (!alive)
            return;
        if (left->isEmpty()) {
            // In the source now: nothing kept here for them any more.
            Adjustments none;
            for (auto it = targets.begin(); it != targets.end(); ++it)
                none.insert(it.key(), Adjustment());
            store->set(none);
            return done(QStringLiteral("source"));
        }
        const QByteArray id = left->takeFirst();
        const Adjustment target = targets.value(id);
        service->fetch(id, QStringLiteral("original"), [=](bool ok, const QJsonObject &result, const QString &code) {
            if (!alive)
                return;
            if (!ok) {
                settle(generation, deferred.reject, error(code.isEmpty() ? QStringLiteral("unavailable") : code,
                                                          result.value("message").toString(code)));
                return;
            }
            QJsonObject params{{"asset", QString::fromUtf8(id)}, {"original", result.value("path").toString()},
                               {"source", source}};
            if (!target.isIdentity()) {
                params.insert("turn", target.turn);
                params.insert("crop", QJsonArray{target.crop.x(), target.crop.y(), target.crop.width(), target.crop.height()});
            }
            service->call(QStringLiteral("set_edit"), params, [=](bool ok, const QJsonObject &answer, const QString &code) {
                if (!alive)
                    return;
                // A source that keeps no edits (a provider's): all of
                // them here.
                if (!ok && code == QLatin1String("unsupported")) {
                    store->set(targets);
                    return done(QStringLiteral("here"));
                }
                if (!ok) {
                    settle(generation, deferred.reject, error(code.isEmpty() ? QStringLiteral("unavailable") : code,
                                                              answer.value("message").toString(code)));
                    return;
                }
                (*next)();
            });
        }, false, source);
    };
    (*next)();
    return deferred.promise;
}
