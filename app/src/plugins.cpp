#include "plugins.h"

#include "encoder.h"
#include "history.h"
#include "library.h"
#include "outgoing.h"
#include "photogrid.h"
#include "serviceclient.h"
#include "translator.h"
#include "viewer.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QSaveFile>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QJSValueIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMimeData>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <cstdio>
#include <ctime>

PluginLibrary::PluginLibrary(const Library *library, QObject *parent) : QObject(parent), m_library(library) {}

int PluginLibrary::count() const { return m_library->count(); }

QString PluginLibrary::dir() const { return m_library->dir(); }

QString PluginLibrary::key() const {
    // The library's own word for its source, read with its photos: it
    // changes with them, never before (as the service's news of another
    // source would, while this library is still the last one).
    return m_library->source();
}

QVariantMap PluginLibrary::item(const QJSValue &photo) const {
    const int index = positionOf(photo);
    if (index < 0)
        return {};
    const LibraryItem &item = m_library->at(index);
    return {
        {"index", index},
        {"id", QString::fromUtf8(item.id)},
        {"date", double(item.date)},
        {"filename", item.filename},
        {"thumbnail", item.thumbPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(QFile::decodeName(item.thumbPath))},
        {"width", int(item.thumbW)},
        {"height", int(item.thumbH)},
        {"video", item.video},
        {"live", item.live},
        {"favorite", item.favorite},
        // The thumbnail's average colour, "#rrggbb"; "" before it has one.
        {"avgColor", item.thumbPath.isEmpty() ? QString() : QColor(QRgb(item.avgColor)).name()},
    };
}

QVariantList PluginLibrary::field(const QString &name) const {
    const auto column = [this](auto value) {
        QVariantList list;
        list.reserve(m_library->count());
        for (int i = 0; i < m_library->count(); ++i)
            list.append(value(m_library->at(i)));
        return list;
    };
    if (name == QLatin1String("date"))
        return column([](const LibraryItem &item) { return double(item.date); });
    // Not one of item's: the day it was taken, in local time, as yyyymmdd
    // (20250108); 0 for a photo with no date. The C library's local time,
    // a fifteenth of QDateTime's.
    if (name == QLatin1String("day"))
        return column([](const LibraryItem &item) {
            if (item.date <= 0)
                return 0;
            const time_t seconds = time_t(item.date / 1000);
            struct tm local;
            if (!localtime_r(&seconds, &local))
                return 0;
            return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
        });
    if (name == QLatin1String("id"))
        return column([](const LibraryItem &item) { return QString::fromUtf8(item.id); });
    if (name == QLatin1String("filename"))
        return column([](const LibraryItem &item) { return item.filename; });
    if (name == QLatin1String("width"))
        return column([](const LibraryItem &item) { return int(item.thumbW); });
    if (name == QLatin1String("height"))
        return column([](const LibraryItem &item) { return int(item.thumbH); });
    if (name == QLatin1String("video"))
        return column([](const LibraryItem &item) { return item.video; });
    if (name == QLatin1String("live"))
        return column([](const LibraryItem &item) { return item.live; });
    if (name == QLatin1String("favorite"))
        return column([](const LibraryItem &item) { return item.favorite; });
    if (name == QLatin1String("avgColor"))
        return column([](const LibraryItem &item) {
            return item.thumbPath.isEmpty() ? QString() : QColor(QRgb(item.avgColor)).name();
        });
    return {};
}

int PluginLibrary::indexOf(const QString &id) const {
    if (m_positions.isEmpty() && m_library->count() > 0) {
        m_positions.reserve(m_library->count());
        for (int i = 0; i < m_library->count(); ++i)
            m_positions.insert(m_library->at(i).id, i);
    }
    return m_positions.value(id.toUtf8(), -1);
}

int PluginLibrary::positionOf(const QJSValue &photo) const {
    if (photo.isString())
        return indexOf(photo.toString());
    if (!photo.isNumber())
        return -1;
    const int index = photo.toInt();
    return index >= 0 && index < m_library->count() ? index : -1;
}

void PluginLibrary::reloaded() {
    m_positions.clear();
    emit changed();
}

PluginSettings::PluginSettings(PluginHost *host) : QQmlPropertyMap(this, nullptr), m_host(host) {
    const QJsonObject kept = host->m_plugins->settingsOf(host->id());
    for (auto it = kept.begin(); it != kept.end(); ++it)
        insert(it.key(), it.value().toVariant());
}

QVariant PluginSettings::updateValue(const QString &key, const QVariant &input) {
    // A JS object or list as plain values, as JSON keeps them.
    const QVariant value = input.metaType() == QMetaType::fromType<QJSValue>()
                               ? input.value<QJSValue>().toVariant(QJSValue::ConvertJSObjects)
                               : input;
    if (!m_host->m_plugins->setSettingOf(m_host->id(), key, QJsonValue::fromVariant(value)))
        m_host->report(tr("Its setting “%1” could not be kept.").arg(key));
    return value;
}

void PluginJob::cancel() {
    if (m_cancel)
        std::exchange(m_cancel, nullptr)();
}

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

QString configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json";
}

QJsonObject readConfig() {
    QFile file(configPath());
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// A path a manifest names, kept inside the plugin's folder.
bool safeEntry(const QString &path) {
    return !path.isEmpty() && !path.startsWith('/') && !path.contains(QLatin1String(".."));
}

// An error thrown in a plugin, where it was: "Main.qml:12: TypeError: …".
QString thrown(const QJSValue &error) {
    const QString file = QUrl(error.property("fileName").toString()).fileName();
    const int line = error.property("lineNumber").toInt();
    return file.isEmpty() ? error.toString() : QStringLiteral("%1:%2: %3").arg(file).arg(line).arg(error.toString());
}

} // namespace

PluginHost::PluginHost(const QString &id, const QString &dir, Plugins *plugins)
    : QObject(plugins), m_id(id), m_dir(dir), m_plugins(plugins) {
    m_settings = std::make_unique<PluginSettings>(this);
    connect(plugins, &Plugins::selectionChanged, this, &PluginHost::selectionChanged);
    connect(plugins, &Plugins::hiddenChanged, this, &PluginHost::hiddenChanged);
    connect(this, &PluginHost::notified, plugins, [this](const QString &text) { emit m_plugins->notice(m_id, text); });
}

PluginHost::~PluginHost() { stop(); }

int PluginHost::api() const { return Plugins::api; }

QString PluginHost::coveredBy() const { return m_plugins->coveredBy(); }

QString PluginHost::cacheDir() const {
    // library/icloud/<apple id> → icloud-<apple id>.
    const QDir library(m_plugins->libraryData()->dir());
    QDir kind(library);
    kind.cdUp();
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/pinhole/plugins/" + m_id + '/'
         + kind.dirName() + '-' + library.dirName();
}

QObject *PluginHost::library() const { return m_plugins->library(); }

QObject *PluginHost::viewer() const { return m_plugins->viewerObject(); }

int PluginHost::currentIndex() const { return m_plugins->grid() ? m_plugins->grid()->currentIndex() : -1; }

QList<int> PluginHost::selection() const { return m_plugins->grid() ? m_plugins->grid()->selection() : QList<int>(); }

int PluginHost::selectionCount() const { return m_plugins->grid() ? m_plugins->grid()->selectionCount() : 0; }

bool PluginHost::isHidden(const QJSValue &photo) const {
    return m_plugins->grid() && m_plugins->grid()->isHidden(m_plugins->library()->positionOf(photo));
}

int PluginHost::hiddenCount() const { return m_plugins->grid() ? m_plugins->grid()->hiddenCount() : 0; }

void Plugins::setGrid(PhotoGrid *grid) {
    m_grid = grid;
    m_keepAwake.setWindow(grid->window());
    connect(grid, &PhotoGrid::selectionChanged, this, &Plugins::selectionChanged);
    connect(grid, &PhotoGrid::hiddenChanged, this, &Plugins::hiddenChanged);
}

void PluginHost::reveal(const QJSValue &photo) const {
    const int index = m_plugins->library()->positionOf(photo);
    PhotoGrid *grid = m_plugins->grid();
    if (!grid || index < 0)
        return;
    grid->reveal(index);
    grid->putCursorOn(index);
    emit m_plugins->revealed(index);
}

void PluginHost::openPhoto(const QJSValue &photo) const {
    const int index = m_plugins->library()->positionOf(photo);
    if (!m_plugins->grid() || index < 0)
        return;
    m_plugins->grid()->reveal(index);
    m_plugins->viewerObject()->openAt(index, m_plugins->grid()->rectOf(index));
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
        factory = js->evaluate(QStringLiteral(R"((function (job) {
            let resolve, reject
            const promise = new Promise(function (a, b) { resolve = a; reject = b })
            promise.cancel = function () { job.cancel() }
            return { promise: promise, resolve: resolve, reject: reject }
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

void PluginHost::report(const QString &problem, bool print) {
    if (print)
        qWarning("%s: %s", qPrintable(m_id), qPrintable(problem));
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit problemChanged();
    m_plugins->touched();
}

void PluginHost::settingsChanged() {
    const QJsonObject kept = m_plugins->settingsOf(m_id);
    for (auto it = kept.begin(); it != kept.end(); ++it) {
        const QVariant value = it.value().toVariant();
        if (m_settings->value(it.key()) != value)
            m_settings->insert(it.key(), value);
    }
}

QJSValue PluginHost::fetch(const QJSValue &photo, const QString &kind, const QJSValue &done) {
    const Library *data = m_plugins->libraryData();
    // Only this library's photos: a position, or an id it has.
    QByteArray id;
    if (const int index = m_plugins->library()->positionOf(photo); index >= 0)
        id = data->at(index).id;
    const int generation = m_generation;

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
    m_plugins->service()->fetch(id, kind, [this, deferred, generation](bool ok, const QJsonObject &result, const QString &code) {
        if (ok)
            settle(generation, deferred->resolve, result.value("path").toString());
        else
            settle(generation, deferred->reject, error(code, result.value("message").toString()));
    });
    return promise;
}

QJSValue PluginHost::run(const QString &program, const QStringList &arguments, const QJSValue &options) {
    QString path = program;
    if (!program.contains('/') && QFileInfo(m_dir + '/' + program).isExecutable())
        path = m_dir + '/' + program;
    auto *process = new QProcess(this);
    process->setWorkingDirectory(m_dir);
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
        process->terminate();
        QTimer::singleShot(1000, process, [process] {
            if (process->state() != QProcess::NotRunning)
                process->kill();
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
            running->terminate();
            QTimer::singleShot(1000, running.data(), [running] {
                if (running->state() != QProcess::NotRunning)
                    running->kill();
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
                process->kill();
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
    if (const QJSValue input = options.property("input"); !input.isUndefined() && !input.isNull()) {
        process->write(input.toString().toUtf8());
        process->closeWriteChannel();
    }
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

void PluginHost::copyFile(const QString &path) const {
    if (!QFileInfo::exists(path))
        return;
    // As Pinhole's own copy: an image pastes into a chat as one.
    Outgoing::toClipboard({path}, const_cast<PluginHost *>(this));
}

void PluginHost::showFile(const QString &path) const {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

QString PluginHost::picturesDir() const { return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation); }

bool PluginHost::exists(const QString &path) const { return QFileInfo::exists(path); }

double PluginHost::fileSize(const QString &path) const { return double(QFileInfo(path).size()); }

QString PluginHost::sizeText(double bytes) const {
    return QLocale().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

QVariant PluginHost::setting(const QString &key, const QVariant &fallback) const {
    const QJsonObject settings = m_plugins->settingsOf(m_id);
    return settings.contains(key) ? settings.value(key).toVariant() : fallback;
}

bool PluginHost::setSetting(const QString &key, const QVariant &value) {
    return m_plugins->setSettingOf(m_id, key, QJsonValue::fromVariant(value));
}

void PluginHost::did(const QString &name, const QJSValue &undo, const QJSValue &redo) {
    if (!undo.isCallable() || !redo.isCallable())
        return;
    m_plugins->history()->did(m_id, name, [this, undo] { call(undo); }, [this, redo] { call(redo); });
}

void PluginHost::warn(const QJSValue &error) {
    report(error.isError() ? thrown(error) : error.toString());
}

void PluginHost::notify(const QString &text) {
    if (!text.isEmpty())
        emit notified(text);
}

bool PluginHost::keepAwake(bool on) {
    m_plugins->keepAwake()->hold(this, on);
    return on && m_plugins->keepAwake()->inhibiting();
}

Plugins::Plugins(const Library *library, ServiceClient *service, Viewer *viewer, Translator *translator,
                 History *history, QObject *parent)
    : QObject(parent), m_library(library), m_service(service), m_viewer(viewer), m_translator(translator),
      m_history(history),
      m_libraryApi(std::make_unique<PluginLibrary>(library)), m_config(readConfig()) {
    // Saving a file writes it in steps, and editors save several at once:
    // one reload once they are done.
    m_settle.setSingleShot(true);
    m_settle.setInterval(300);
    connect(&m_settle, &QTimer::timeout, this, &Plugins::rescan);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_settle, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_settle, qOverload<>(&QTimer::start));
    // Their names and descriptions are in the language too.
    if (m_translator)
        connect(m_translator, &Translator::changed, this, [this] {
            ++m_revision;
            emit changed();
        });
    scan();
}

Plugins::~Plugins() {
    // Hosts first, while what they stop can still say so to them.
    for (auto &plugin : m_plugins)
        plugin->host.reset();
}

void Plugins::registerTypes() {
    for (const char *type : {"PinholePlugin", "PluginAction", "Command", "ShellButton", "Chip", "UiText", "Spinner", "Toggle"})
        qmlRegisterType(QUrl(QStringLiteral("qrc:/%1.qml").arg(QLatin1String(type))), "Pinhole.Plugin", 1, 0, type);
}

void Plugins::setEngine(QQmlEngine *engine) {
    m_engine = engine;
    connect(engine, &QQmlEngine::warnings, this, &Plugins::warned);
}

QJSEngine *Plugins::engine() const { return m_engine; }

QString Plugins::personalDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins";
}

QString Plugins::builtInDir() {
    // PINHOLE_BUILTIN_PLUGINS: another folder of them, for trying changes.
    if (const QString dir = qEnvironmentVariable("PINHOLE_BUILTIN_PLUGINS"); !dir.isEmpty())
        return dir;
    // Installed (/usr/bin/pinhole: /usr/lib/pinhole/plugins), else this
    // checkout's (app/build/pinhole: plugins/).
    const QString app = QCoreApplication::applicationDirPath();
    const QString installed = QDir::cleanPath(app + "/../lib/pinhole/plugins");
    return QFileInfo::exists(installed) ? installed : QDir::cleanPath(app + "/../../plugins");
}

void Plugins::libraryReloaded() {
    m_libraryApi->reloaded();
    for (const auto &plugin : m_plugins)
        emit plugin->host->cacheDirChanged();
}

QVariantMap Plugins::at(const QString &id) const {
    for (const auto &p : m_plugins) {
        if (p->id != id)
            continue;
        // Its name and description from its own catalog, the English the
        // key, as its other words are.
        const auto said = [&](const QString &english) {
            return m_translator && !english.isEmpty() ? m_translator->pluginText(p->id, english) : english;
        };
        return {
            {"id", p->id},
            {"name", said(p->name)},
            {"description", said(p->description)},
            {"version", p->version},
            {"author", p->author},
            {"personal", p->personal},
            {"enabled", p->error.isEmpty() && enabled(*p)},
            {"error", p->error},
            {"problem", p->host->problem()},
            {"toured", m_config.value("toured").toObject().value(p->id).toBool()},
            {"dir", p->dir},
            // Another URL for each load, so the engine reads the files again;
            // and naming the plugin, which its strings' context keeps
            // (Translator::ownersOf).
            {"main", p->main.isEmpty() ? QString()
                                       : QUrl::fromLocalFile(p->dir + '/' + p->main).toString() + "?plugin="
                                             + QString::fromUtf8(QUrl::toPercentEncoding(p->id)) + "&load=" + QString::number(p->loads)},
            {"host", QVariant::fromValue<QObject *>(p->host.get())},
        };
    }
    return {};
}

void Plugins::setEnabled(const QString &id, bool on) {
    QJsonObject config = m_config;
    QJsonObject states = config.value("enabled").toObject();
    states.insert(id, on);
    config.insert("enabled", states);
    writeConfig(config);
    if (!on) {
        m_history->forget(id);
        for (const auto &p : m_plugins)
            if (p->id == id)
                p->host->stop();
    }
    updateCatalogs();
    ++m_revision;
    emit changed();
}

void Plugins::setCoveredBy(const QString &id) {
    if (id == m_coveredBy)
        return;
    m_coveredBy = id;
    emit coveredByChanged();
    for (const auto &p : m_plugins)
        emit p->host->coveredByChanged();
}

void Plugins::setToured(const QString &id) {
    QJsonObject config = m_config;
    QJsonObject toured = config.value("toured").toObject();
    if (toured.value(id).toBool())
        return;
    toured.insert(id, true);
    config.insert("toured", toured);
    writeConfig(config);
    ++m_revision;
    emit changed();
}

void Plugins::updateCatalogs() {
    QList<std::pair<QString, QString>> catalogs;
    for (const auto &p : m_plugins)
        if (p->error.isEmpty() && enabled(*p))
            catalogs.append({p->id, p->dir});
    if (m_translator)
        m_translator->setPluginCatalogs(catalogs);
}

QString Plugins::idOf(const QString &url) const {
    const QString path = QUrl(url).toLocalFile();
    for (const auto &p : m_plugins)
        if (!path.isEmpty() && path.startsWith(p->dir + '/'))
            return p->id;
    return {};
}

void Plugins::showPersonalDir() const {
    QDir().mkpath(personalDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(personalDir()));
}

bool Plugins::enabled(const Plugin &plugin) const {
    const QJsonValue state = m_config.value("enabled").toObject().value(plugin.id);
    return state.isBool() ? state.toBool() : !plugin.personal;
}

bool Plugins::writeConfig(const QJsonObject &config) {
    // In place of the one there in one step (written beside it, then
    // renamed over it), so a failure leaves that one as it was, and what
    // is kept here with it.
    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    QSaveFile file(configPath());
    if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(config).toJson()) >= 0 && file.commit()) {
        m_config = config;
        return true;
    }
    qWarning("pinhole: could not keep %s: %s", qPrintable(configPath()), qPrintable(file.errorString()));
    return false;
}

QJsonObject Plugins::settingsOf(const QString &id) const {
    return m_config.value("settings").toObject().value(id).toObject();
}

bool Plugins::setSettingOf(const QString &id, const QString &key, const QJsonValue &value) {
    QJsonObject config = m_config;
    QJsonObject all = config.value("settings").toObject();
    QJsonObject mine = all.value(id).toObject();
    if (mine.value(key) == value && mine.contains(key))
        return true;
    mine.insert(key, value);
    all.insert(id, mine);
    config.insert("settings", all);
    if (!writeConfig(config))
        return false;
    for (const auto &p : m_plugins)
        if (p->id == id)
            p->host->settingsChanged();
    return true;
}

void Plugins::touched() {
    // Once whatever said so (a binding, a load) is through.
    if (std::exchange(m_touching, true))
        return;
    QMetaObject::invokeMethod(this, [this] {
        m_touching = false;
        ++m_revision;
        emit changed();
    }, Qt::QueuedConnection);
}

QString Plugins::describe(const QQmlError &error, const QString &dir) {
    QString file = error.url().toLocalFile();
    if (file.startsWith(dir + '/'))
        file = file.mid(dir.size() + 1);
    else if (!file.isEmpty())
        file = QFileInfo(file).fileName();
    QString where = file;
    if (!where.isEmpty() && error.line() > 0)
        where += ':' + QString::number(error.line());
    return where.isEmpty() ? error.description() : where + ": " + error.description();
}

void Plugins::warned(const QList<QQmlError> &warnings) {
    for (const QQmlError &warning : warnings) {
        const QString file = warning.url().toLocalFile();
        if (file.isEmpty())
            continue;
        for (const auto &p : m_plugins)
            if (file.startsWith(p->dir + '/')) {
                p->host->report(describe(warning, p->dir), false);
                break;
            }
    }
}

void Plugins::loadFailed(const QString &id) {
    if (!m_engine)
        return;
    const QVariantMap info = at(id);
    const QString main = info.value("main").toString();
    if (main.isEmpty())
        return;
    // The engine keeps what it read: asking again costs nothing, and says
    // what went wrong as it did then.
    QQmlComponent component(m_engine, QUrl(main));
    for (const auto &p : m_plugins)
        if (p->id == id && !component.errors().isEmpty())
            p->host->report(describe(component.errors().first(), p->dir), false);
}

void Plugins::rescan() {
    scan();
}

QStringList Plugins::apiTwoUses(const QString &dir) {
    // Each, and the words that use it. Anything added to what a plugin is
    // given before API 2 is released is API 2's, and goes here.
    static const QList<std::pair<QString, QRegularExpression>> features = {
        {"promises", QRegularExpression(QStringLiteral("\\.then\\s*\\("))},
        {"pinhole.settings", QRegularExpression(QStringLiteral("\\bpinhole\\.settings\\b"))},
        {"pinhole.notify", QRegularExpression(QStringLiteral("\\bpinhole\\.notify\\b"))},
        {"pinhole.warn", QRegularExpression(QStringLiteral("\\bpinhole\\.warn\\b"))},
        {"pinhole.api", QRegularExpression(QStringLiteral("\\bpinhole\\.api\\b"))},
        {"pinhole.coveredBy", QRegularExpression(QStringLiteral("\\bcoveredBy\\b"))},
        {"covered", QRegularExpression(QStringLiteral("\\bcovered\\b"))},
        {"hidden", QRegularExpression(QStringLiteral("(^|[\\s;{])hidden\\s*:"), QRegularExpression::MultilineOption)},
        {"avgColor", QRegularExpression(QStringLiteral("\\bavgColor\\b"))},
        {"library.field", QRegularExpression(QStringLiteral("\\blibrary\\.field\\s*\\("))},
        {"pinhole.isHidden", QRegularExpression(QStringLiteral("\\b(isHidden|hiddenCount|hiddenChanged|onHiddenChanged)\\b"))},
        {"run's output", QRegularExpression(QStringLiteral("\\boutput\\s*:"))},
        {"pinhole.keepAwake", QRegularExpression(QStringLiteral("\\bkeepAwake\\s*\\("))},
    };
    // Its words, not its comments.
    static const QRegularExpression comments(QStringLiteral("/\\*.*?\\*/|//[^\\n]*"),
                                             QRegularExpression::DotMatchesEverythingOption);
    QString code;
    QDirIterator it(dir, {"*.qml", "*.js"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (!file.fileName().contains(QLatin1String("/target/")) && file.open(QIODevice::ReadOnly))
            code += QString::fromUtf8(file.readAll()).remove(comments) + '\n';
    }
    QStringList uses;
    for (const auto &[name, words] : features)
        if (code.contains(words))
            uses << name;
    return uses;
}

QString Plugins::manifestProblem(const QString &dir, QJsonObject *manifest, QString *main) {
    QFile file(dir + "/manifest.json");
    if (!file.open(QIODevice::ReadOnly))
        return tr("There is no manifest.json in %1").arg(dir);
    QJsonParseError parseError;
    *manifest = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    const QString id = manifest->value("id").toString(QFileInfo(dir).fileName());
    *main = manifest->value("entryPoints").toObject().value("main").toString();
    if (parseError.error != QJsonParseError::NoError)
        return tr("manifest.json is not JSON: %1").arg(parseError.errorString());
    if (manifest->value("schemaVersion").toInt() != 1)
        return tr("manifest.json needs \"schemaVersion\": 1");
    if (id.isEmpty() || id.contains('/') || id.contains(QLatin1String("..")))
        return tr("manifest.json has no usable \"id\"");
    if (!safeEntry(*main) || !QFileInfo::exists(dir + '/' + *main))
        return tr("manifest.json's \"entryPoints\" has no \"main\" file in the plugin's folder");
    if (const int wanted = manifest->value("api").toInt(1); wanted > api)
        return tr("It needs a newer Pinhole: plugin API %1, and this one has %2.").arg(wanted).arg(api);
    return {};
}

void Plugins::scan() {
    // What each plugin was, to tell which ones changed on disk.
    QHash<QString, int> loads;
    QHash<QString, QString> stamps;
    for (const auto &p : m_plugins)
        loads.insert(p->id, p->loads);
    auto stampOf = [](const QString &dir) {
        QStringList parts;
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo info(it.next());
            // A helper's build folder is not the plugin.
            if (info.filePath().contains(QLatin1String("/target/")))
                continue;
            parts << info.filePath() + QString::number(info.lastModified().toMSecsSinceEpoch()) + QString::number(info.size());
        }
        parts.sort();
        return parts.join('|');
    };
    for (const auto &p : m_plugins)
        stamps.insert(p->id, p->stamp);

    std::vector<std::unique_ptr<Plugin>> found;
    auto look = [&](const QString &root, bool personal) {
        const QDir dir(root);
        for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            auto plugin = std::make_unique<Plugin>();
            plugin->dir = dir.absoluteFilePath(name);
            plugin->personal = personal;
            if (!QFileInfo::exists(plugin->dir + "/manifest.json"))
                continue;
            QJsonObject manifest;
            QString main;
            plugin->error = manifestProblem(plugin->dir, &manifest, &main);
            plugin->id = manifest.value("id").toString(name);
            plugin->name = manifest.value("name").toString(plugin->id);
            plugin->description = manifest.value("description").toString();
            plugin->version = manifest.value("version").toString();
            plugin->author = manifest.value("author").toString();
            if (plugin->error.isEmpty())
                plugin->main = main;
            plugin->stamp = stampOf(plugin->dir);
            // One's own with a built-in's id takes its place.
            const auto same = std::find_if(found.begin(), found.end(), [&](const auto &p) { return p->id == plugin->id; });
            if (same != found.end())
                found.erase(same);
            found.push_back(std::move(plugin));
        }
    };
    look(builtInDir(), false);
    look(personalDir(), true);
    std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a->personal != b->personal ? !a->personal : a->name.localeAwareCompare(b->name) < 0;
    });

    // Hosts stay with their plugins across scans, so what the QML loaded
    // holds of one stays good; one loaded again stops what it started.
    for (auto &plugin : found) {
        const auto old = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const auto &p) { return p->id == plugin->id; });
        if (old != m_plugins.end() && (*old)->dir == plugin->dir)
            plugin->host = std::move((*old)->host);
        if (!plugin->host)
            plugin->host = std::make_unique<PluginHost>(plugin->id, plugin->dir, this);
        plugin->loads = loads.value(plugin->id) + (stamps.contains(plugin->id) && stamps.value(plugin->id) != plugin->stamp ? 1 : 0);
        if (plugin->loads != loads.value(plugin->id))
            plugin->host->stop();
    }
    if (std::any_of(found.begin(), found.end(), [&](const auto &p) { return p->loads != loads.value(p->id); }))
        if (m_engine)
            m_engine->clearComponentCache();
    // What a plugin loaded again or gone did is its old self's to undo.
    for (const auto &old : m_plugins) {
        const auto now = std::find_if(found.begin(), found.end(), [&](const auto &p) { return p->id == old->id; });
        if (now == found.end() || (*now)->loads != old->loads || (*now)->dir != old->dir)
            m_history->forget(old->id);
    }
    m_plugins = std::move(found);

    QStringList ids;
    for (const auto &p : m_plugins)
        ids << p->id;
    if (ids != m_ids) {
        m_ids = ids;
        emit idsChanged();
    }
    updateCatalogs();
    ++m_revision;
    emit changed();
    watch();
}

void Plugins::watch() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    QDir().mkpath(personalDir());
    QStringList paths{personalDir(), builtInDir()};
    for (const auto &p : m_plugins) {
        QDirIterator it(p->dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        paths << p->dir;
        while (it.hasNext()) {
            const QString path = it.next();
            if (!path.contains(QLatin1String("/target/")))
                paths << path;
        }
    }
    m_watcher.addPaths(paths);
}

namespace {

const char *kPluginUsage = R"(Usage: pinhole plugin <command>

  new <id>               begin a plugin of your own in
                         ~/.config/pinhole/plugins/<id>: a manifest.json and
                         a Main.qml to start from
  check [folder | id]    look a plugin over as Pinhole would load it: its
                         manifest, its QML, its translations (the folder
                         here, if none is named)

docs/plugins.md has the whole of what a plugin is given.
)";

void out(const QString &line) {
    std::fputs(qPrintable(line + '\n'), stdout);
    std::fflush(stdout);
}

void err(const QString &line) {
    std::fputs(qPrintable(line + '\n'), stderr);
    std::fflush(stderr);
}

QString tilde(const QString &path) {
    const QString home = QDir::homePath();
    return path.startsWith(home + '/') ? '~' + path.mid(home.size()) : path;
}

int newPlugin(const QString &id) {
    static const QRegularExpression usable(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    if (!usable.match(id).hasMatch() || id.contains(QLatin1String(".."))) {
        err(QStringLiteral("pinhole: a plugin's id is letters, digits, dots, dashes: me.favourites"));
        return 2;
    }
    const QString dir = Plugins::personalDir() + '/' + id;
    if (QFileInfo::exists(dir)) {
        err(QStringLiteral("pinhole: %1 is there already").arg(tilde(dir)));
        return 1;
    }
    // "me.favourite-count" is named "Favourite count" to begin with.
    QString name = id.section('.', -1).replace('-', ' ').replace('_', ' ');
    name[0] = name[0].toUpper();
    const QJsonObject manifest{
        {"schemaVersion", 1},
        {"api", Plugins::api},
        {"id", id},
        {"name", name},
        {"version", "0.1.0"},
        {"description", "What it does, in a sentence."},
        {"entryPoints", QJsonObject{{"main", "Main.qml"}}},
    };
    const QByteArray main = QStringLiteral(R"(import QtQuick
import Pinhole.Plugin

// %1: what it adds to Pinhole. docs/plugins.md has all a plugin is given;
// saving this file loads it again at once.
PinholePlugin {
    id: plugin

    actions: [
        PluginAction {
            place: "grid"
            text: qsTr("%1")
            key: "F2"
            onTriggered: {
                const photo = pinhole.library.item(pinhole.currentIndex())
                pinhole.notify(qsTr("%2, one of %3 photos").arg(photo.filename).arg(pinhole.library.count))
            }
        }
    ]
    help: [["F2", qsTr("Say which photo is under the pointer")]]
}
)").arg(name, QStringLiteral("%1"), QStringLiteral("%2")).toUtf8();
    QDir().mkpath(dir);
    QSaveFile manifestFile(dir + "/manifest.json"), mainFile(dir + "/Main.qml");
    if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(QJsonDocument(manifest).toJson()) < 0 || !manifestFile.commit()
        || !mainFile.open(QIODevice::WriteOnly) || mainFile.write(main) < 0 || !mainFile.commit()) {
        err(QStringLiteral("pinhole: could not write %1").arg(tilde(dir)));
        return 1;
    }
    out(QStringLiteral("Made %1. Turn it on in the side panel (Tab), then change Main.qml: it loads again as you save it.")
            .arg(tilde(dir)));
    out(QStringLiteral("pinhole plugin check %1 looks it over.").arg(id));
    return 0;
}

int checkPlugin(const QString &target) {
    // A folder, else one of one's own or Pinhole's by the id its manifest
    // gives (Series' folder is series, its id pinhole.series).
    QString dir = QFileInfo(target).absoluteFilePath();
    for (const QString &root : {Plugins::personalDir(), Plugins::builtInDir()})
        for (const QFileInfo &each : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (QFileInfo::exists(dir + "/manifest.json"))
                break;
            QFile file(each.filePath() + "/manifest.json");
            if (file.open(QIODevice::ReadOnly)
                && QJsonDocument::fromJson(file.readAll()).object().value("id").toString(each.fileName()) == target)
                dir = each.filePath();
        }
    dir = QDir::cleanPath(dir);

    if (!QFileInfo::exists(dir + "/manifest.json")) {
        err(QStringLiteral("pinhole: no plugin %1: no folder with a manifest.json, nor one of that id").arg(target));
        return 1;
    }
    QJsonObject manifest;
    QString main;
    QStringList problems;
    if (const QString problem = Plugins::manifestProblem(dir, &manifest, &main); !problem.isEmpty()) {
        err(QStringLiteral("%1: %2").arg(tilde(dir), problem));
        return 1;
    }
    // Its QML as the window would compile it: every file it uses, and the
    // JavaScript they import.
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(dir + '/' + main), QQmlComponent::PreferSynchronous);
    for (const QQmlError &error : component.errors())
        problems << Plugins::describe(error, dir);
    // Its translations, each a JSON object.
    for (const QFileInfo &file : QDir(dir + "/i18n").entryInfoList({"*.json"}, QDir::Files)) {
        QFile json(file.filePath());
        QJsonParseError parseError;
        if (!json.open(QIODevice::ReadOnly) || !QJsonDocument::fromJson(json.readAll(), &parseError).isObject())
            problems << QStringLiteral("i18n/%1: %2").arg(file.fileName(),
                                                           parseError.error != QJsonParseError::NoError ? parseError.errorString()
                                                                                                        : QStringLiteral("not a JSON object"));
    }
    const QString id = manifest.value("id").toString(QFileInfo(dir).fileName());
    const QString name = QStringLiteral("%1 %2").arg(id, manifest.value("version").toString()).trimmed();
    if (!problems.isEmpty()) {
        for (const QString &problem : problems)
            err(QStringLiteral("%1: %2").arg(name, problem));
        return 1;
    }
    const int asked = manifest.value("api").toInt(1);
    out(QStringLiteral("%1 (plugin API %2): looks fine.").arg(name).arg(asked));
    // What it uses of a newer API than it asks for works here, and not in a
    // Pinhole of the one it asks for, which would load it all the same.
    if (const QStringList uses = Plugins::apiTwoUses(dir); asked < 2 && !uses.isEmpty())
        err(QStringLiteral("%1: warning: it uses %2, of plugin API 2, and its manifest asks for API %3: give it \"api\": 2.")
                .arg(name, uses.join(QStringLiteral(", ")))
                .arg(asked));
    if (QDir(dir).absolutePath().startsWith(Plugins::personalDir() + '/')
        && QFileInfo::exists(Plugins::builtInDir() + '/' + id + "/manifest.json"))
        out(QStringLiteral("It takes the place of Pinhole's own %1 while it is there.").arg(id));
    return 0;
}

} // namespace

int runPluginCommand(int argc, char *argv[]) {
    if (argc < 2 || qstrcmp(argv[1], "plugin") != 0)
        return -1;
    const QString command = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    if (command == QLatin1String("new") && argc == 4) {
        QCoreApplication app(argc, argv);
        return newPlugin(QString::fromLocal8Bit(argv[3]));
    }
    if (command == QLatin1String("check") && argc <= 4) {
        // A window system is not needed to compile QML.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication app(argc, argv);
        app.setApplicationName("pinhole");
        Plugins::registerTypes();
        return checkPlugin(argc == 4 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("."));
    }
    std::fputs(kPluginUsage, command.isEmpty() || command == QLatin1String("help") ? stdout : stderr);
    return command.isEmpty() || command == QLatin1String("help") ? 0 : 2;
}
