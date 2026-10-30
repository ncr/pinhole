#include "plugins.h"

#include "history.h"
#include "library.h"
#include "photogrid.h"
#include "serviceclient.h"
#include "translator.h"
#include "viewer.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMimeData>
#include <QProcess>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

PluginLibrary::PluginLibrary(const Library *library, QObject *parent) : QObject(parent), m_library(library) {}

int PluginLibrary::count() const { return m_library->count(); }

QString PluginLibrary::dir() const { return m_library->dir(); }

QVariantMap PluginLibrary::item(int index) const {
    if (index < 0 || index >= m_library->count())
        return {};
    const LibraryItem &item = m_library->at(index);
    return {
        {"id", QString::fromUtf8(item.id)},
        {"date", double(item.date)},
        {"filename", item.filename},
        {"thumbnail", item.thumbPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(QFile::decodeName(item.thumbPath))},
        {"width", int(item.thumbW)},
        {"height", int(item.thumbH)},
        {"video", item.video},
        {"live", item.live},
        {"favorite", item.favorite},
    };
}

int PluginLibrary::indexOf(const QString &id) const {
    if (m_positions.isEmpty() && m_library->count() > 0) {
        m_positions.reserve(m_library->count());
        for (int i = 0; i < m_library->count(); ++i)
            m_positions.insert(m_library->at(i).id, i);
    }
    return m_positions.value(id.toUtf8(), -1);
}

void PluginLibrary::reloaded() {
    m_positions.clear();
    emit changed();
}

namespace {

// An item's frames on their way into ffmpeg: each frame is asked for
// (seek), taken from the item as the window draws it, and written to
// ffmpeg's input, into a file beside the one wanted until it is whole.
class Recording : public QObject {
public:
    Recording(QQuickItem *item, const QJSValue &options, const QJSValue &progress, const QJSValue &done, QObject *parent)
        : QObject(parent), m_item(item), m_seek(options.property("seek")), m_progress(progress), m_done(done) {
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
        connect(&m_ffmpeg, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            if (status != QProcess::NormalExit || code != 0)
                return fail(QString::fromUtf8(m_ffmpeg.readAllStandardError()).trimmed());
            QFile::remove(m_path);
            if (!QFile::rename(part(), m_path))
                return fail(QStringLiteral("could not write %1").arg(m_path));
            finish(m_path, {});
        });
        m_ffmpeg.start(QStringLiteral("ffmpeg"), args);
        if (!m_ffmpeg.waitForStarted())
            return fail(QStringLiteral("ffmpeg did not start"));
        next();
    }

private:
    QString part() const { return m_path + QStringLiteral(".part"); }

    void next() {
        if (m_ended)
            return;
        if (m_at == m_frames) {
            m_ffmpeg.closeWriteChannel();
            return;
        }
        if (!m_item)
            return fail(QStringLiteral("the picture went away"));
        if (m_seek.isCallable())
            m_seek.call({m_at});
        m_grab = m_item->grabToImage(m_size);
        if (!m_grab)
            return fail(QStringLiteral("the picture could not be taken"));
        connect(m_grab.data(), &QQuickItemGrabResult::ready, this, [this] {
            QImage frame = m_grab->image();
            m_grab.reset();
            if (frame.size() != m_size)
                frame = frame.scaled(m_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            frame = frame.convertToFormat(QImage::Format_RGB32);
            for (int y = 0; y < frame.height(); ++y)
                m_ffmpeg.write(reinterpret_cast<const char *>(frame.constScanLine(y)), qint64(frame.width()) * 4);
            ++m_at;
            if (m_progress.isCallable())
                m_progress.call({double(m_at) / m_frames});
            // Not ahead of ffmpeg by more than a few frames.
            if (m_ffmpeg.bytesToWrite() > qint64(m_size.width()) * m_size.height() * 4 * 8)
                m_ffmpeg.waitForBytesWritten(10000);
            next();
        });
    }

    void fail(const QString &error) {
        if (m_ended)
            return;
        m_ffmpeg.kill();
        QFile::remove(part());
        finish({}, error.isEmpty() ? QStringLiteral("ffmpeg failed") : error);
    }

    void finish(const QString &path, const QString &error) {
        m_ended = true;
        if (m_done.isCallable())
            m_done.call({path, error});
        deleteLater();
    }

    QPointer<QQuickItem> m_item;
    QJSValue m_seek, m_progress, m_done;
    QString m_path, m_format;
    QSize m_size;
    int m_fps = 20, m_frames = 1, m_at = 0;
    bool m_ended = false;
    QProcess m_ffmpeg;
    QSharedPointer<QQuickItemGrabResult> m_grab;
};

QString configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json";
}

// A path a manifest names, kept inside the plugin's folder.
bool safeEntry(const QString &path) {
    return !path.isEmpty() && !path.startsWith('/') && !path.contains(QLatin1String(".."));
}

} // namespace

PluginHost::PluginHost(const QString &id, const QString &dir, Plugins *plugins)
    : QObject(plugins), m_id(id), m_dir(dir), m_plugins(plugins) {
    connect(plugins, &Plugins::selectionChanged, this, &PluginHost::selectionChanged);
}

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

void Plugins::setGrid(PhotoGrid *grid) {
    m_grid = grid;
    connect(grid, &PhotoGrid::selectionChanged, this, &Plugins::selectionChanged);
}

void PluginHost::reveal(int index) const {
    if (m_plugins->grid())
        m_plugins->grid()->reveal(index);
}

void PluginHost::openPhoto(int index) const {
    if (!m_plugins->grid() || index < 0 || index >= m_plugins->libraryData()->count())
        return;
    m_plugins->grid()->reveal(index);
    m_plugins->viewerObject()->openAt(index, m_plugins->grid()->rectOf(index));
}

void PluginHost::fetch(int index, const QString &kind, const QJSValue &done) const {
    const Library *library = m_plugins->libraryData();
    if (index < 0 || index >= library->count()) {
        QJSValue(done).call({QString(), QStringLiteral("not_found")});
        return;
    }
    m_plugins->service()->fetch(library->at(index).id, kind, [done](bool ok, const QJsonObject &result, const QString &code) {
        QJSValue callback(done);
        if (callback.isCallable())
            callback.call({ok ? result.value("path").toString() : QString(), ok ? QString() : code});
    });
}

void PluginHost::run(const QString &program, const QStringList &arguments, const QJSValue &done) {
    QString path = program;
    if (!program.contains('/') && QFileInfo(m_dir + '/' + program).isExecutable())
        path = m_dir + '/' + program;
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, done](int code, QProcess::ExitStatus status) {
        QJSValue callback(done);
        if (callback.isCallable())
            callback.call({status == QProcess::NormalExit ? code : -1, QString::fromUtf8(process->readAllStandardOutput()),
                           QString::fromUtf8(process->readAllStandardError())});
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [process, done, path](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        QJSValue callback(done);
        if (callback.isCallable())
            callback.call({-1, QString(), QStringLiteral("%1 did not start").arg(path)});
        process->deleteLater();
    });
    process->setWorkingDirectory(m_dir);
    process->start(path, arguments);
}

void PluginHost::record(QQuickItem *item, const QJSValue &options, const QJSValue &progress, const QJSValue &done) {
    (new Recording(item, options, progress, done, this))->start();
}

void PluginHost::copyFile(const QString &path) const {
    if (!QFileInfo::exists(path))
        return;
    auto *mime = new QMimeData;
    const QUrl url = QUrl::fromLocalFile(path);
    mime->setUrls({url});
    // File managers paste from this one.
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), "copy\n" + url.toEncoded());
    // A chat takes a GIF pasted as one.
    if (path.endsWith(QLatin1String(".gif"), Qt::CaseInsensitive)) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            mime->setData(QStringLiteral("image/gif"), file.readAll());
    }
    QGuiApplication::clipboard()->setMimeData(mime);
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

void PluginHost::setSetting(const QString &key, const QVariant &value) {
    m_plugins->setSettingOf(m_id, key, QJsonValue::fromVariant(value));
}

void PluginHost::did(const QString &name, const QJSValue &undo, const QJSValue &redo) {
    if (!undo.isCallable() || !redo.isCallable())
        return;
    const QString id = m_id;
    auto call = [id](const QJSValue &step) {
        const QJSValue result = step.call();
        if (result.isError())
            qWarning("%s: %s", qPrintable(id), qPrintable(result.toString()));
    };
    m_plugins->history()->did(m_id, name, [call, undo] { call(undo); }, [call, redo] { call(redo); });
}

Plugins::Plugins(const Library *library, ServiceClient *service, Viewer *viewer, Translator *translator,
                 History *history, QObject *parent)
    : QObject(parent), m_library(library), m_service(service), m_viewer(viewer), m_translator(translator),
      m_history(history),
      m_libraryApi(std::make_unique<PluginLibrary>(library)) {
    // Saving a file writes it in steps, and editors save several at once:
    // one reload once they are done.
    m_settle.setSingleShot(true);
    m_settle.setInterval(300);
    connect(&m_settle, &QTimer::timeout, this, &Plugins::rescan);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_settle, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_settle, qOverload<>(&QTimer::start));
    scan();
}

Plugins::~Plugins() = default;

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
        return {
            {"id", p->id},
            {"name", p->name},
            {"description", p->description},
            {"version", p->version},
            {"author", p->author},
            {"personal", p->personal},
            {"enabled", p->error.isEmpty() && enabled(*p)},
            {"error", p->error},
            {"toured", readConfig().value("toured").toObject().value(p->id).toBool()},
            {"dir", p->dir},
            // Another URL for each load, so the engine reads the files again.
            {"main", p->main.isEmpty() ? QString()
                                       : QUrl::fromLocalFile(p->dir + '/' + p->main).toString() + "?load=" + QString::number(p->loads)},
            {"host", QVariant::fromValue<QObject *>(p->host.get())},
        };
    }
    return {};
}

void Plugins::setEnabled(const QString &id, bool on) {
    QJsonObject config = readConfig();
    QJsonObject states = config.value("enabled").toObject();
    states.insert(id, on);
    config.insert("enabled", states);
    writeConfig(config);
    if (!on)
        m_history->forget(id);
    updateCatalogs();
    ++m_revision;
    emit changed();
}

void Plugins::setToured(const QString &id) {
    QJsonObject config = readConfig();
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
    QStringList catalogs;
    for (const auto &p : m_plugins)
        if (p->error.isEmpty() && enabled(*p))
            catalogs << p->dir + "/i18n";
    m_translator->setExtraCatalogs(catalogs);
}

void Plugins::showPersonalDir() const {
    QDir().mkpath(personalDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(personalDir()));
}

bool Plugins::enabled(const Plugin &plugin) const {
    const QJsonValue state = readConfig().value("enabled").toObject().value(plugin.id);
    return state.isBool() ? state.toBool() : !plugin.personal;
}

QJsonObject Plugins::readConfig() const {
    QFile file(configPath());
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

void Plugins::writeConfig(const QJsonObject &config) const {
    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    QFile file(configPath() + ".part");
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(config).toJson());
    file.close();
    QFile::remove(configPath());
    file.rename(configPath());
}

QJsonObject Plugins::settingsOf(const QString &id) const {
    return readConfig().value("settings").toObject().value(id).toObject();
}

void Plugins::setSettingOf(const QString &id, const QString &key, const QJsonValue &value) {
    QJsonObject config = readConfig();
    QJsonObject all = config.value("settings").toObject();
    QJsonObject mine = all.value(id).toObject();
    mine.insert(key, value);
    all.insert(id, mine);
    config.insert("settings", all);
    writeConfig(config);
}

void Plugins::rescan() {
    scan();
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
            QFile file(plugin->dir + "/manifest.json");
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QJsonParseError parseError;
            const QJsonObject manifest = QJsonDocument::fromJson(file.readAll(), &parseError).object();
            plugin->id = manifest.value("id").toString(name);
            plugin->name = manifest.value("name").toString(plugin->id);
            plugin->description = manifest.value("description").toString();
            plugin->version = manifest.value("version").toString();
            plugin->author = manifest.value("author").toString();
            const QString main = manifest.value("entryPoints").toObject().value("main").toString();
            if (parseError.error != QJsonParseError::NoError)
                plugin->error = tr("manifest.json is not JSON: %1").arg(parseError.errorString());
            else if (manifest.value("schemaVersion").toInt() != 1)
                plugin->error = tr("manifest.json needs \"schemaVersion\": 1");
            else if (plugin->id.isEmpty() || plugin->id.contains('/') || plugin->id.contains(QLatin1String("..")))
                plugin->error = tr("manifest.json has no usable \"id\"");
            else if (!safeEntry(main) || !QFileInfo::exists(plugin->dir + '/' + main))
                plugin->error = tr("manifest.json's \"entryPoints\" has no \"main\" file in the plugin's folder");
            else
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

    // Hosts stay with their plugins across scans, so what a plugin holds
    // on to (a program running) is not cut off.
    for (auto &plugin : found) {
        const auto old = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const auto &p) { return p->id == plugin->id; });
        if (old != m_plugins.end() && (*old)->dir == plugin->dir)
            plugin->host = std::move((*old)->host);
        if (!plugin->host)
            plugin->host = std::make_unique<PluginHost>(plugin->id, plugin->dir, this);
        plugin->loads = loads.value(plugin->id) + (stamps.contains(plugin->id) && stamps.value(plugin->id) != plugin->stamp ? 1 : 0);
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
