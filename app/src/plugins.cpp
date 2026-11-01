#include "plugins.h"
#include "sdrvideo.h"
#include "selftest.h"

#include "encoder.h"
#include "favourites.h"
#include "history.h"
#include "library.h"
#include "outgoing.h"
#include "photogrid.h"
#include "photoprovider.h"
#include "portal.h"
#include "scroller.h"
#include "serviceclient.h"
#include "sources.h"
#include "storage.h"
#include "translator.h"
#include "viewer.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDeadlineTimer>
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
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <cmath>
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

QStringList PluginLibrary::keys() const { return Storage::sourceIds(); }

namespace {
// A Live Photo's effect as set on the iPhone, as plugins name it.
QString effectName(LiveEffect effect) {
    switch (effect) {
    case LiveEffect::Loop: return QStringLiteral("loop");
    case LiveEffect::Bounce: return QStringLiteral("bounce");
    case LiveEffect::LongExposure: return QStringLiteral("long_exposure");
    case LiveEffect::None: break;
    }
    return QString();
}
} // namespace

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
        // For an Image, in sRGB (FileProvider); the file itself, to hand
        // to a program.
        {"thumbnail", FileProvider::urlOf(QFile::decodeName(item.thumbPath))},
        {"thumbPath", QFile::decodeName(item.thumbPath)},
        {"width", int(item.thumbW)},
        {"height", int(item.thumbH)},
        {"video", item.video},
        {"live", item.live},
        {"favorite", item.favorite},
        {"effect", effectName(item.effect)},
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
    // (20250108); 0 for a photo with no date (its date 0, the library's
    // NULL), and any other a day, before 1970 too. The C library's local
    // time, a fifteenth of QDateTime's.
    if (name == QLatin1String("day"))
        return column([](const LibraryItem &item) {
            if (item.date == 0)
                return 0;
            // The second it is in: rounded down, before 1970 too.
            const time_t seconds = time_t(item.date / 1000 - (item.date % 1000 < 0 ? 1 : 0));
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
    if (name == QLatin1String("effect"))
        return column([](const LibraryItem &item) { return effectName(item.effect); });
    if (name == QLatin1String("avgColor"))
        return column([](const LibraryItem &item) {
            return item.thumbPath.isEmpty() ? QString() : QColor(QRgb(item.avgColor)).name();
        });
    return {};
}

int PluginLibrary::indexOf(const QString &id) const {
    // Of the library as it is now, even asked before plugins are told it
    // was read again (a handler of the grid's news, say).
    if (m_positionsOf != m_library->generation()) {
        m_positions.clear();
        m_positionsOf = m_library->generation();
    }
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
    m_told = m_library->generation();
    emit changed();
    touched();
}

void PluginLibrary::touched() {
    ++m_revision;
    emit revisionChanged();
}

bool PluginLibrary::stale() const { return m_told != m_library->generation(); }

PluginSettings::PluginSettings(PluginHost *host) : QQmlPropertyMap(this, nullptr), m_host(host) {
    const QJsonObject kept = host->m_plugins->settingsOf(host->id());
    for (auto it = kept.begin(); it != kept.end(); ++it)
        insert(it.key(), it.value().toVariant());
}

QStringList PluginSettings::keys() const { return m_host->m_plugins->settingsOf(m_host->id()).keys(); }

QVariant PluginSettings::updateValue(const QString &key, const QVariant &input) {
    // A JS object or list as plain values, as JSON keeps them.
    const QVariant value = input.metaType() == QMetaType::fromType<QJSValue>()
                               ? input.value<QJSValue>().toVariant(QJSValue::ConvertJSObjects)
                               : input;
    const QJsonValue json = QJsonValue::fromVariant(value);
    if (!m_host->m_plugins->setSettingOf(m_host->id(), key, json))
        m_host->report(tr("Its setting “%1” could not be kept.").arg(key));
    // Taken out: undefined, as one never set is.
    return json.isNull() || json.isUndefined() ? QVariant() : value;
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

QJsonObject readToured() {
    QFile file(Plugins::touredPath());
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// What Pinhole's own plugins kept in their settings before they had a
// state and a cache of their own (API 3), by plugin: a key, or each key
// beginning with one that ends in ':'.
struct Moved {
    const char *plugin, *key;
    bool cache;
};
const Moved movedKeys[] = {
    // What it found in each library: worked out again.
    {"pinhole.screenshots", "found:", true},
    // The user's series made and taken apart, each library's, and from
    // before they were.
    {"pinhole.series", "edits:", false},
    {"pinhole.series", "edits", false},
    // Its panel left open.
    {"pinhole.info", "shown", false},
    // The devices sent to.
    {"pinhole.send", "devices", false},
};

const Moved *movedKey(const QString &plugin, const QString &key) {
    for (const Moved &moved : movedKeys) {
        const QLatin1String wanted(moved.key);
        if (plugin == QLatin1String(moved.plugin) && (wanted.endsWith(':') ? key.startsWith(wanted) : key == wanted))
            return &moved;
    }
    return nullptr;
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

PluginHost::~PluginHost() {
    endPrograms(close());
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
            process->kill();
    for (QProcess *process : programs) {
        if (process->state() != QProcess::NotRunning)
            process->waitForFinished(1000);
        delete process;
    }
}

int PluginHost::api() const { return Plugins::api; }

QString PluginHost::coveredBy() const { return m_plugins->coveredBy(); }

QObject *PluginHost::state() const {
    if (!m_state)
        m_state = std::make_unique<PluginStoreMap>(m_plugins->stateStore(), m_id);
    return m_state.get();
}

QObject *PluginHost::cache() const {
    if (!m_cache)
        m_cache = std::make_unique<PluginStoreMap>(m_plugins->cacheStore(), m_id);
    return m_cache.get();
}

QObject *PluginHost::sources() const {
    if (!m_sources)
        m_sources = std::make_unique<PluginSources>(const_cast<PluginHost *>(this));
    return m_sources.get();
}

PluginSources::PluginSources(PluginHost *host) : QObject(nullptr), m_host(host) {
    connect(host->m_plugins, &Plugins::changed, this, &PluginSources::kindsChanged);
    if (Sources *window = sources()) {
        connect(window, &Sources::listChanged, this, &PluginSources::listChanged);
        connect(window, &Sources::progressChanged, this, &PluginSources::progressChanged);
        connect(window, &Sources::roundsChanged, this, &PluginSources::roundsChanged);
        connect(window, &Sources::discoveredChanged, this, &PluginSources::discoveredChanged);
    }
    connect(host->m_plugins->service(), &ServiceClient::healthChanged, this, &PluginSources::healthChanged);
}

Sources *PluginSources::sources() const { return m_host->m_plugins->sourcesObject(); }

QStringList PluginSources::kinds() const { return m_host->m_plugins->sourceKindsOf(m_host->id()); }

QVariantList PluginSources::list() const { return sources() ? sources()->list() : QVariantList(); }

QString PluginSources::current() const { return sources() ? sources()->current() : QString(); }

QString PluginSources::health() const { return m_host->m_plugins->service()->health(); }

QVariantMap PluginSources::progress() const { return sources() ? sources()->progress() : QVariantMap(); }

QVariantMap PluginSources::rounds() const { return sources() ? sources()->rounds() : QVariantMap(); }

QVariantMap PluginSources::discovered() const {
    QVariantMap own;
    if (!sources())
        return own;
    const QVariantMap found = sources()->discovered();
    for (const QString &kind : kinds())
        if (found.contains(kind))
            own.insert(kind, found.value(kind));
    return own;
}

void PluginSources::discover() {
    if (sources())
        sources()->discover();
}

void PluginSources::show(const QString &id) {
    if (sources())
        sources()->show(id);
}

QJSValue PluginSources::refused(const QString &code, const QString &message) {
    const PluginHost::Deferred deferred = m_host->defer([] {});
    m_host->settle(m_host->m_generation, deferred.reject, m_host->error(code, message));
    return deferred.promise;
}

QJSValue PluginSources::ask(const std::function<void(std::function<void(bool, const QJsonObject &, const QString &, const QString &)>)> &how,
                            const std::function<QVariant(const QJsonObject &)> &shape) {
    // The service's answer cannot be stopped, only no longer waited for.
    PluginHost *host = m_host;
    auto deferred = std::make_shared<PluginHost::Deferred>();
    const std::weak_ptr<PluginHost::Deferred> weak = deferred;
    const int generation = host->m_generation;
    *deferred = host->defer([host, weak, generation] {
        if (const auto d = weak.lock())
            host->settle(generation, d->reject, host->error(QStringLiteral("cancelled"), PluginHost::tr("cancelled")));
    });
    const QJSValue promise = deferred->promise;
    const QPointer<PluginHost> alive(host);
    how([alive, deferred, generation, shape](bool ok, const QJsonObject &result, const QString &code, const QString &message) {
        if (!alive)
            return;
        if (!ok) {
            alive->settle(generation, deferred->reject, alive->error(code, message));
            return;
        }
        if (QJSEngine *js = alive->engine())
            alive->settle(generation, deferred->resolve, js->toScriptValue(shape(result)));
    });
    return promise;
}

QJSValue PluginSources::add(const QString &kind, const QJSValue &entry) {
    if (!kinds().contains(kind))
        return refused(QStringLiteral("not_yours"), tr("%1 is not a kind of source of this plugin's").arg(kind));
    if (!sources())
        return refused(QStringLiteral("no_service"), tr("no sources here"));
    const QJsonObject params = QJsonObject::fromVariantMap(entry.toVariant().toMap());
    return ask([this, kind, params](auto answer) { sources()->add(kind, params, answer); },
               [](const QJsonObject &result) { return QVariant(result.toVariantMap()); });
}

QJSValue PluginSources::signIn(const QString &kind, const QJSValue &params) {
    if (!kinds().contains(kind))
        return refused(QStringLiteral("not_yours"), tr("%1 is not a kind of source of this plugin's").arg(kind));
    if (!sources())
        return refused(QStringLiteral("no_service"), tr("no sources here"));
    const QJsonObject asked = QJsonObject::fromVariantMap(params.toVariant().toMap());
    return ask([this, kind, asked](auto answer) { sources()->signIn(kind, asked, answer); },
               [](const QJsonObject &result) { return QVariant(QVariantMap{{"state", result.value("state").toString()}}); });
}

QJSValue PluginSources::submitCode(const QString &kind, const QJSValue &params) {
    if (!kinds().contains(kind))
        return refused(QStringLiteral("not_yours"), tr("%1 is not a kind of source of this plugin's").arg(kind));
    if (!sources())
        return refused(QStringLiteral("no_service"), tr("no sources here"));
    const QJsonObject asked = QJsonObject::fromVariantMap(params.toVariant().toMap());
    return ask([this, kind, asked](auto answer) { sources()->submitCode(kind, asked, answer); },
               [](const QJsonObject &result) { return QVariant(QVariantMap{{"state", result.value("state").toString()}}); });
}

QJSValue PluginSources::signOut(const QString &id) {
    if (!sources())
        return refused(QStringLiteral("no_service"), tr("no sources here"));
    const QVariantMap entry = sources()->entry(id);
    if (entry.isEmpty())
        return refused(QStringLiteral("not_found"), tr("no source %1").arg(id));
    const QString kind = entry.value("kind").toString();
    if (!kinds().contains(kind))
        return refused(QStringLiteral("not_yours"), tr("%1 is not a kind of source of this plugin's").arg(kind));
    return ask([this, id](auto answer) { sources()->signOut(id, answer); },
               [](const QJsonObject &result) { return QVariant(result.value("sources").toArray().toVariantList()); });
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

QString PluginHost::cacheDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/pinhole/plugins/" + m_id + '/'
         + m_plugins->cacheName();
}

QString Plugins::cacheName() const {
    // library/icloud/<apple id> → icloud-<apple id>.
    const QDir library(m_library->dir());
    QDir kind(library);
    kind.cdUp();
    return kind.dirName() + '-' + library.dirName();
}

QObject *PluginHost::library() const { return m_plugins->library(); }

QObject *PluginHost::viewer() const { return m_plugins->viewerObject(); }

QObject *PluginHost::display() const { return m_plugins->display(); }

QString PluginHost::srgbProfile() const {
    // Installed (/usr/bin/pinhole: /usr/share/pinhole), else this
    // checkout's (app/build/pinhole: app/assets).
    const QString app = QCoreApplication::applicationDirPath();
    const QString installed = QDir::cleanPath(app + "/../share/pinhole/sRGB.icc");
    return QFileInfo::exists(installed) ? installed : QDir::cleanPath(app + "/../assets/sRGB.icc");
}

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
    // Told once plugins have the library as it is now, the grid's news of
    // a reload included, so isHidden(id) and their own positions agree.
    connect(grid, &PhotoGrid::hiddenChanged, this, [this] {
        if (m_libraryApi->stale())
            m_hiddenPending = true;
        else
            emit hiddenChanged();
    });
    updateMarkBits();
}

void Plugins::setSources(Sources *sources) { m_sources = sources; }

QStringList Plugins::sourceKindsOf(const QString &id) const {
    for (const auto &p : m_plugins)
        if (p->id == id)
            return p->sources;
    return {};
}

void Plugins::setFavourites(Favourites *favourites) {
    if (m_favourites)
        disconnect(m_favourites, nullptr, m_libraryApi.get(), nullptr);
    m_favourites = favourites;
    if (m_favourites)
        connect(m_favourites, &Favourites::changed, m_libraryApi.get(), &PluginLibrary::touched);
}

void PluginHost::reveal(const QJSValue &photo) const {
    const int index = m_plugins->library()->positionOf(photo);
    PhotoGrid *grid = m_plugins->grid();
    if (!grid || index < 0)
        return;
    m_plugins->handedOver(m_id);
    grid->reveal(index);
    grid->putCursorOn(index);
    emit m_plugins->revealed(index);
}

void PluginHost::openPhoto(const QJSValue &photo) const {
    const int index = m_plugins->library()->positionOf(photo);
    if (!m_plugins->grid() || index < 0)
        return;
    m_plugins->handedOver(m_id);
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
    // One set to null is gone (every key the map has had: its own keys()
    // are those kept).
    for (const QString &key : m_settings->QQmlPropertyMap::keys())
        if (!kept.contains(key) && m_settings->value(key).isValid())
            m_settings->clear(key);
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
        if (m_closing) {
            disconnect(process, &QProcess::finished, process, &QObject::deleteLater);
            return m_ending.push_back(process);
        }
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

void PluginHost::showFile(const QString &path) const {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

QString PluginHost::picturesDir() const { return picturesLocation(); }

bool PluginHost::exists(const QString &path) const { return QFileInfo::exists(path); }

double PluginHost::fileSize(const QString &path) const { return double(QFileInfo(path).size()); }

QString PluginHost::sizeText(double bytes, int decimals) const {
    return QLocale().formattedDataSize(qint64(bytes), std::clamp(decimals, 0, 3), QLocale::DataSizeTraditionalFormat);
}

QVariant PluginHost::setting(const QString &key, const QVariant &fallback) const {
    const QJsonObject settings = m_plugins->settingsOf(m_id);
    return settings.contains(key) ? settings.value(key).toVariant() : fallback;
}

bool PluginHost::setSetting(const QString &key, const QVariant &value) {
    return m_plugins->setSettingOf(m_id, key, QJsonValue::fromVariant(value));
}

double PluginHost::did(const QString &name, const QJSValue &undo, const QJSValue &redo) {
    if (!undo.isCallable() || !redo.isCallable())
        return 0;
    return double(m_plugins->history()->did(m_id, name, [this, undo] { call(undo); }, [this, redo] { call(redo); }));
}

void PluginHost::dropStep(double step) {
    if (step > 0)
        m_plugins->history()->drop(m_id, quint64(step));
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
      m_libraryApi(std::make_unique<PluginLibrary>(library)), m_config(readConfig()),
      m_stateStore(stateRoot(), QStringLiteral("state.json"), &m_files),
      m_cacheStore(cacheRoot(), QStringLiteral("cache.json"), &m_files), m_toured(readToured()) {
    // What a plugin keeps that could not be written, said under it.
    const auto failed = [this](const QString &what) {
        return [this, what](const QString &id) {
            for (const auto &p : m_plugins)
                if (p->id == id)
                    p->host->report(what);
        };
    };
    connect(&m_stateStore, &PluginStore::failed, this, failed(tr("Its state could not be kept.")));
    connect(&m_cacheStore, &PluginStore::failed, this, failed(tr("Its cache could not be kept.")));
    moveOutOfSettings();
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
    // Pinhole's own marks, there from the first frame.
    updateMarkKinds();
}

Plugins::~Plugins() {
    // Marks outliving this (the plugins' QML goes after) tell it nothing.
    for (const auto &[id, given] : m_marks)
        for (const GivenMark &g : given)
            if (g.mark)
                disconnect(g.mark, nullptr, this, nullptr);
    // Hosts first, while what they stop can still say so to them: every
    // one's programs asked to end at once, and given one second together,
    // not a second each.
    std::vector<QProcess *> ending;
    for (auto &plugin : m_plugins)
        if (plugin->host)
            for (QProcess *process : plugin->host->close())
                ending.push_back(process);
    PluginHost::endPrograms(ending);
    for (auto &plugin : m_plugins)
        plugin->host.reset();
    // What they kept still to be written, written.
    flush();
}

QString Plugins::stateRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/pinhole/plugins";
}

QString Plugins::cacheRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/pinhole/plugins";
}

QString Plugins::touredPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/pinhole/toured.json";
}

void Plugins::flush() {
    m_stateStore.flush();
    m_cacheStore.flush();
    m_files.wait();
}

void Plugins::moveOutOfSettings() {
    // plugins.json is the user's choices, which pinholed reads again
    // whenever it changes: what plugins kept there that is not one goes to
    // their state or their cache, and the tours seen to toured.json; all
    // of it written there first, then plugins.json without it, once, away
    // from the window's thread. Their settings are read without it from
    // now on, and their state and cache with it.
    QJsonObject config = m_config;
    QJsonObject settings = config.value("settings").toObject();
    QJsonObject moved, movedSettings;
    QList<std::pair<QString, QByteArray>> files;
    QStringList said;
    const QStringList ids = settings.keys();
    for (const QString &id : ids) {
        QJsonObject mine = settings.value(id).toObject();
        QJsonObject state, cache;
        for (const QString &key : mine.keys())
            if (const Moved *to = movedKey(id, key)) {
                (to->cache ? cache : state).insert(key, mine.value(key));
                mine.remove(key);
                said << id + ' ' + key + (to->cache ? " (cache)" : " (state)");
            }
        if (state.isEmpty() && cache.isEmpty())
            continue;
        QJsonObject taken = state;
        for (auto it = cache.begin(); it != cache.end(); ++it)
            taken.insert(it.key(), it.value());
        movedSettings.insert(id, taken);
        if (mine.isEmpty())
            settings.remove(id);
        else
            settings.insert(id, mine);
        if (!state.isEmpty())
            files.append({m_stateStore.path(id), m_stateStore.merged(id, state)});
        if (!cache.isEmpty())
            files.append({m_cacheStore.path(id), m_cacheStore.merged(id, cache)});
    }
    if (!movedSettings.isEmpty()) {
        moved.insert("settings", movedSettings);
        config.insert("settings", settings);
    }
    if (config.contains("toured")) {
        const QJsonObject toured = config.value("toured").toObject();
        for (auto it = toured.begin(); it != toured.end(); ++it)
            if (it.value().toBool())
                m_toured.insert(it.key(), true);
        moved.insert("toured", config.value("toured"));
        config.remove("toured");
        files.append({touredPath(), QJsonDocument(m_toured).toJson()});
        said << "toured";
    }
    if (files.isEmpty())
        return;
    std::fprintf(stderr, "pinhole: moving out of plugins.json: %s\n", qPrintable(said.join(", ")));
    m_config = config;
    m_moved = moved;
    m_moving = std::make_shared<std::atomic<int>>(0);
    const auto moving = m_moving;
    const QString path = configPath();
    const QByteArray bytes = QJsonDocument(config).toJson();
    m_files.run(
        [files, path, bytes, moving] {
            // Where they belong first: only then out of plugins.json.
            for (const auto &[file, contents] : files)
                if (!KeptFiles::write(file, contents)) {
                    moving->store(2);
                    return false;
                }
            moving->store(1);
            // If this fails, plugins.json is written without them the next
            // time it is.
            return KeptFiles::write(path, bytes);
        },
        [this](bool) {
            if (!m_moving)
                return;
            if (m_moving->load() == 2) {
                std::fprintf(stderr, "pinhole: left in plugins.json, as they could not be kept apart\n");
                m_config = unmoved(m_config);
            }
            m_moving.reset();
            m_moved = {};
        });
}

QJsonObject Plugins::unmoved(QJsonObject config) const {
    QJsonObject settings = config.value("settings").toObject();
    const QJsonObject moved = m_moved.value("settings").toObject();
    for (auto it = moved.begin(); it != moved.end(); ++it) {
        QJsonObject mine = settings.value(it.key()).toObject();
        const QJsonObject taken = it.value().toObject();
        for (auto key = taken.begin(); key != taken.end(); ++key)
            if (!mine.contains(key.key()))
                mine.insert(key.key(), key.value());
        settings.insert(it.key(), mine);
    }
    if (!moved.isEmpty())
        config.insert("settings", settings);
    if (m_moved.contains("toured")) {
        QJsonObject toured = m_moved.value("toured").toObject();
        for (auto it = m_toured.begin(); it != m_toured.end(); ++it)
            toured.insert(it.key(), it.value());
        config.insert("toured", toured);
    }
    return config;
}

void Plugins::registerTypes() {
    for (const char *type : {"PinholePlugin", "PluginAction", "PluginSource", "Command", "ShellButton", "Chip", "UiText", "Spinner", "Toggle", "FieldInput"})
        qmlRegisterType(QUrl(QStringLiteral("qrc:/%1.qml").arg(QLatin1String(type))), "Pinhole.Plugin", 1, 0, type);
    qmlRegisterType<PluginMark>("Pinhole.Plugin", 1, 0, "PluginMark");
    // HDR video made SDR between a MediaPlayer and its VideoOutput (API 4).
    qmlRegisterType<SdrVideo>("Pinhole.Plugin", 1, 0, "SdrVideo");
    // The side panel's scrolling, for a plugin's own list (scroller.h).
    qmlRegisterType<Scroller>("Pinhole.Plugin", 1, 0, "Scroller");
}

namespace {

template <typename T>
bool change(T &field, const T &value) {
    if (field == value)
        return false;
    field = value;
    return true;
}

} // namespace

void PluginMark::setName(const QString &name) {
    if (change(m_name, name))
        emit changed();
}
void PluginMark::setText(const QString &text) {
    if (change(m_text, text))
        emit changed();
}
void PluginMark::setIcon(const QString &icon) {
    if (change(m_icon, icon))
        emit changed();
}
void PluginMark::setColour(const QString &colour) {
    if (change(m_colour, colour))
        emit changed();
}
void PluginMark::setOrder(int order) {
    if (change(m_order, order))
        emit changed();
}
void PluginMark::setField(const QString &field) {
    if (change(m_field, field))
        emit changed();
}
void PluginMark::setDefaultShown(bool shown) {
    if (change(m_defaultShown, shown))
        emit changed();
}
void PluginMark::setPhotos(const QVariantList &photos) {
    // Given again, even the same: a list of the library as it is now, no
    // longer to be followed from the one before (Plugins::marksOf).
    m_photos = photos;
    emit photosChanged();
}

void Plugins::setMarks(const QString &id, const QList<QObject *> &marks) {
    std::vector<GivenMark> before = std::move(m_marks[id]);
    std::vector<GivenMark> now;
    for (QObject *object : marks) {
        auto *mark = qobject_cast<PluginMark *>(object);
        if (!mark)
            continue;
        // One kept is still followed where its photos went.
        const auto kept = std::find_if(before.begin(), before.end(), [mark](const GivenMark &g) { return g.mark == mark; });
        now.push_back({mark, kept != before.end() ? kept->photos : markPhotos(mark->photos())});
        if (kept != before.end())
            continue;
        connect(mark, &PluginMark::changed, this, &Plugins::updateMarkKinds);
        connect(mark, &PluginMark::photosChanged, this, [this, id, mark] {
            for (GivenMark &given : m_marks[id])
                if (given.mark == mark)
                    given.photos = markPhotos(mark->photos());
            updateMarkBits();
        });
        // Gone with its plugin's QML (turned off, loaded again): a moment
        // later, as QPointer has let go of it by then.
        connect(mark, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &Plugins::updateMarkKinds); });
    }
    for (const GivenMark &given : before)
        if (given.mark && std::none_of(now.begin(), now.end(), [&](const GivenMark &g) { return g.mark == given.mark; }))
            disconnect(given.mark, nullptr, this, nullptr);
    if (now.empty())
        m_marks.erase(id);
    else
        m_marks[id] = std::move(now);
    updateMarkKinds();
    updateMarkBits();
}

std::vector<int> Plugins::markPhotos(const QVariantList &photos) const {
    std::vector<int> out;
    out.reserve(size_t(photos.size()));
    const int count = m_libraryApi->count();
    for (const QVariant &photo : photos) {
        const int index = photo.typeId() == QMetaType::QString ? m_libraryApi->indexOf(photo.toString()) : photo.toInt();
        if (index >= 0 && index < count)
            out.push_back(index);
    }
    return out;
}

QVariantMap Plugins::marksOf(const QString &id) const {
    QVariantMap out;
    const auto found = m_marks.find(id);
    if (found == m_marks.end())
        return out;
    for (const GivenMark &given : found->second)
        if (given.mark && given.mark->field().isEmpty())
            out.insert(given.mark->name(), QVariantList(given.photos.begin(), given.photos.end()));
    return out;
}

void Plugins::updateMarkKinds() {
    // Those gone with their plugin's QML.
    for (auto it = m_marks.begin(); it != m_marks.end();) {
        std::erase_if(it->second, [](const GivenMark &given) { return given.mark.isNull(); });
        it = it->second.empty() ? m_marks.erase(it) : std::next(it);
    }
    struct Kind {
        QVariantMap map;
        PluginMark *mark;
    };
    // Pinhole's own: the video and the stack marks. Live Photos' and
    // favourites' are their plugins' (plugins/live, plugins/favourites).
    auto core = [](const QString &id, const QString &icon, const QString &colour, int order, const QString &field,
                   bool shown) {
        return Kind{{{"id", id}, {"plugin", QString()}, {"name", id.section('/', 1)}, {"text", QString()},
                     {"icon", icon}, {"colour", colour}, {"order", order}, {"field", field}, {"defaultShown", shown}},
                    nullptr};
    };
    std::vector<Kind> kinds{core("pinhole/video", "video", "cyan", 20, "video", true),
                            core("pinhole/stack", "stack", "magenta", 90, "stack", true)};
    static const QStringList fields{"", "live", "favorite", "video"};
    static const QStringList colours{"red", "green", "yellow", "blue", "magenta", "cyan", "accent"};
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    static const QRegularExpression names(QStringLiteral("^[A-Za-z0-9._-]+$"));
    for (const auto &plugin : m_plugins) {
        const auto found = m_marks.find(plugin->id);
        if (found == m_marks.end())
            continue;
        QStringList problems, taken;
        for (const GivenMark &given : found->second) {
            PluginMark *mark = given.mark;
            if (!mark)
                continue;
            const QString name = mark->name();
            const QString what = QStringLiteral("its mark \"%1\"").arg(name);
            const QString id = plugin->id + '/' + name;
            if (!names.match(name).hasMatch())
                problems << QStringLiteral("a mark's name must be letters, digits, '.', '_' or '-': \"%1\"").arg(name);
            else if (taken.contains(name))
                problems << QStringLiteral("two marks are named \"%1\"").arg(name);
            else if (std::any_of(kinds.begin(), kinds.end(), [&id](const Kind &k) { return !k.mark && k.map.value("id") == id; }))
                problems << QStringLiteral("%1 is Pinhole's own").arg(what);
            else if (!fields.contains(mark->field()))
                problems << QStringLiteral("%1: no field \"%2\" (live, favorite, video, or none for photos)").arg(what, mark->field());
            else if (!markShape(mark->icon()).valid())
                problems << QStringLiteral("%1: its icon must be video, live, heart, stack or one character").arg(what);
            else if (!colours.contains(mark->colour()) && !hex.match(mark->colour()).hasMatch())
                problems << QStringLiteral("%1: no colour \"%2\" (red, green, yellow, blue, magenta, cyan, accent or #rrggbb)")
                                .arg(what, mark->colour());
            else if (taken.size() == kMaxMarksEach)
                problems << QStringLiteral("%1 is left out: %2 marks at most for a plugin").arg(what).arg(kMaxMarksEach);
            else if (int(kinds.size()) >= kMaxMarks)
                problems << QStringLiteral("%1 is left out: %2 marks at most in all").arg(what).arg(kMaxMarks);
            else {
                taken << name;
                kinds.push_back({{{"id", id}, {"plugin", plugin->id}, {"name", name}, {"text", mark->text()},
                                  {"icon", mark->icon()}, {"colour", mark->colour()}, {"order", mark->order()},
                                  {"field", mark->field()}, {"defaultShown", mark->defaultShown()}},
                                 mark});
            }
        }
        // Said once, not again each time any plugin's marks change; and
        // unsaid once its marks are right (or there is room for them),
        // unless something else went wrong since.
        const QString problem = problems.join(QStringLiteral("; "));
        QString &said = m_markProblems[plugin->id];
        if (plugin->host && !problem.isEmpty() && plugin->host->problem() != problem)
            plugin->host->report(problem);
        else if (plugin->host && problem.isEmpty() && !said.isEmpty() && plugin->host->problem() == said)
            plugin->host->report(QString(), false);
        said = problem;
    }
    // By their order, Pinhole's own first of the same.
    std::stable_sort(kinds.begin(), kinds.end(),
                     [](const Kind &a, const Kind &b) { return a.map.value("order").toInt() < b.map.value("order").toInt(); });
    QVariantList list;
    std::vector<PluginMark *> of;
    for (const Kind &kind : kinds) {
        list.append(kind.map);
        of.push_back(kind.mark && kind.mark->field().isEmpty() ? kind.mark : nullptr);
    }
    const bool bitsMoved = of != m_markOf;
    m_markOf = std::move(of);
    if (list != m_markKinds) {
        m_markKinds = list;
        emit markKindsChanged();
    }
    if (bitsMoved)
        updateMarkBits();
}

void Plugins::updateMarkBits() {
    if (!m_grid)
        return;
    std::vector<quint32> bits;
    for (size_t k = 0; k < m_markOf.size(); ++k) {
        if (!m_markOf[k])
            continue;
        for (const auto &[id, given] : m_marks)
            for (const GivenMark &g : given) {
                if (g.mark != m_markOf[k])
                    continue;
                if (bits.empty() && !g.photos.empty())
                    bits.resize(size_t(m_library->count()));
                for (int i : g.photos)
                    if (size_t(i) < bits.size())
                        bits[size_t(i)] |= 1u << k;
            }
    }
    m_grid->setMarkBits(std::move(bits));
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

void Plugins::libraryOpened() {
    m_cacheName = cacheName();
    if (m_ready)
        return;
    m_ready = true;
    // Each on loads as this is said (Main.qml), then the window gathers
    // them all at once.
    m_loadingAll = true;
    emit readyChanged();
    m_loadingAll = false;
    emit allLoaded();
}

void Plugins::libraryReloaded(const std::vector<int> &moved) {
    // What each plugin hides and stacks follows its photos until it gives
    // its own again, as the grid's do: the window gathers them again at
    // once, as the grid already has them.
    const auto follow = [&moved](std::vector<int> &photos) {
        for (int &i : photos)
            i = i >= 0 && i < int(moved.size()) ? moved[size_t(i)] : -1;
        std::erase(photos, -1);
    };
    for (auto &[id, followed] : m_followed) {
        follow(followed.hidden);
        for (auto &stack : followed.stacks)
            follow(stack);
    }
    // And the photos each mark of photos marks, the grid given them where
    // they are now at once.
    for (auto &[id, given] : m_marks)
        for (GivenMark &g : given)
            follow(g.photos);
    updateMarkBits();
    emit positionsMoved();
    m_libraryApi->reloaded();
    // Only another source's library has a cacheDir of its own; the same
    // one read again or moved elsewhere has the same.
    if (const QString name = cacheName(); name != m_cacheName) {
        m_cacheName = name;
        for (const auto &plugin : m_plugins)
            emit plugin->host->cacheDirChanged();
    }
    if (std::exchange(m_hiddenPending, false))
        emit hiddenChanged();
}

namespace {

std::vector<int> positions(const QVariantList &list) {
    std::vector<int> out;
    out.reserve(size_t(list.size()));
    for (const QVariant &index : list)
        out.push_back(index.toInt());
    return out;
}

} // namespace

QVariantList Plugins::hiddenOf(const QString &id, const QVariantList &given, bool fresh) {
    Followed &followed = m_followed[id];
    std::vector<int> list = positions(given);
    if (fresh || list != followed.hiddenGiven) {
        followed.hidden = list;
        followed.hiddenGiven = std::move(list);
    }
    return QVariantList(followed.hidden.begin(), followed.hidden.end());
}

QVariantList Plugins::stacksOf(const QString &id, const QVariantList &given, bool fresh) {
    Followed &followed = m_followed[id];
    std::vector<std::vector<int>> lists;
    lists.reserve(size_t(given.size()));
    for (const QVariant &stack : given)
        lists.push_back(positions(stack.toList()));
    if (fresh || lists != followed.stacksGiven) {
        followed.stacks = lists;
        followed.stacksGiven = std::move(lists);
    }
    QVariantList now;
    now.reserve(qsizetype(followed.stacks.size()));
    for (const auto &stack : followed.stacks)
        now.append(QVariant(QVariantList(stack.begin(), stack.end())));
    return now;
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
            {"toured", m_toured.value(p->id).toBool()},
            {"sources", p->sources},
            {"copy", p->copy},
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
    // pinholed reads the switches too (a source kind follows its plugin's):
    // told once the file says so, it lists and shows what is on at once.
    if (writeConfig(config) && m_service)
        m_service->call(QStringLiteral("plugins_changed"), {}, [](bool, const QJsonObject &, const QString &) {});
    if (!on) {
        m_history->forget(id);
        for (const auto &p : m_plugins)
            if (p->id == id)
                p->host->stop();
        // Its marks go with it at once, before its QML does.
        setMarks(id, {});
    }
    updateCatalogs();
    ++m_revision;
    emit changed();
}

void Plugins::setCovering(const QStringList &ids) {
    if (ids == m_covering)
        return;
    const QString was = coveredBy();
    m_covering = ids;
    if (coveredBy() == was)
        return;
    emit coveredByChanged();
    for (const auto &p : m_plugins)
        emit p->host->coveredByChanged();
}

void Plugins::handedOver(const QString &id) {
    if (coveredBy() != id)
        return;
    // Each closing changes what covers: told from a copy.
    const QStringList beneath = m_covering.mid(0, m_covering.size() - 1);
    for (const QString &other : beneath)
        for (const auto &p : m_plugins)
            if (p->id == other && p->host)
                emit p->host->dismissed();
}

void Plugins::setToured(const QString &id) {
    // Not a choice of the user's, so not in plugins.json.
    if (m_toured.value(id).toBool())
        return;
    m_toured.insert(id, true);
    const QString path = touredPath();
    const QByteArray bytes = QJsonDocument(m_toured).toJson();
    m_files.run([path, bytes] { return KeptFiles::write(path, bytes); });
    ++m_revision;
    emit changed();
}

void Plugins::updateCatalogs() {
    // Those off too: the side panel names one in the language, and says
    // what it does, whether it is on or not (iCloud turned off, one's own
    // not yet turned on).
    QList<std::pair<QString, QString>> catalogs;
    for (const auto &p : m_plugins)
        if (p->error.isEmpty())
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

bool Plugins::writeConfig(QJsonObject config) {
    // After what is being written away from this thread (the move out of
    // it, as Pinhole started): the keys it moved back in, if they could
    // not be kept where they now belong.
    m_files.wait();
    if (m_moving && m_moving->load() == 2)
        config = unmoved(config);
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
    // Null (or undefined) takes it out.
    const bool gone = value.isNull() || value.isUndefined();
    if (gone ? !mine.contains(key) : mine.value(key) == value && mine.contains(key))
        return true;
    if (gone)
        mine.remove(key);
    else
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

void Plugins::sourceRefused(const QString &id, const QString &kind) {
    for (const auto &p : m_plugins)
        if (p->id == id && p->host)
            p->host->report(tr("sources: %1 is not among the kinds its manifest's \"sources\" names").arg(kind));
}

void Plugins::layerFailed(const QString &id, const QString &message) {
    for (const auto &p : m_plugins)
        if (p->id == id && p->host)
            p->host->report(QStringLiteral("viewerLayer: ") + message.trimmed());
}

void Plugins::rescan() {
    scan();
}

namespace {

// QML or JavaScript as code alone, by its tokens: its comments gone, and
// the words of its strings and regular expressions (a URL's "//", a
// "hidden:" said), their quotes left, but a string of one word (a name:
// "jpeg", "effect"); a template's ${…} is code again.
// Line breaks stay, so what follows is where it was.
QString codeOnly(const QString &source) {
    QString code;
    code.reserve(source.size());
    const qsizetype n = source.size();
    qsizetype i = 0;
    // The brace depth, and where each ${ of a template began.
    int depth = 0;
    QList<int> templates;
    // A "/" begins a regular expression where a value would: after an
    // operator, a bracket or nothing, or a word such as return; else it
    // divides.
    auto valueNext = [&code] {
        qsizetype k = code.size() - 1;
        while (k >= 0 && code[k].isSpace())
            --k;
        if (k < 0)
            return true;
        auto wordy = [](QChar c) { return c.isLetterOrNumber() || c == '_' || c == '$'; };
        if (wordy(code[k])) {
            qsizetype from = k;
            while (from > 0 && wordy(code[from - 1]))
                --from;
            static const QStringList before = {"return", "typeof", "case", "do",   "else",  "in",   "of",
                                               "new",    "delete", "void", "throw", "yield", "await"};
            return before.contains(code.mid(from, k - from + 1));
        }
        // After a value's ++ or --, it divides.
        if ((code[k] == '+' || code[k] == '-') && k > 0 && code[k - 1] == code[k]) {
            qsizetype j = k - 2;
            while (j >= 0 && code[j].isSpace())
                --j;
            if (j >= 0 && (wordy(code[j]) || code[j] == ')' || code[j] == ']'))
                return false;
        }
        return QStringLiteral("(,=:[!&|?{};+-*%<>~^").contains(code[k]);
    };
    // A template's words, up to its end or its next ${.
    auto templateWords = [&] {
        while (i < n) {
            const QChar c = source[i];
            if (c == '\\') {
                i += 2;
            } else if (c == '`') {
                code += c;
                ++i;
                return;
            } else if (c == '$' && i + 1 < n && source[i + 1] == '{') {
                code += QStringLiteral("${");
                i += 2;
                templates << depth++;
                return;
            } else {
                if (c == '\n')
                    code += c;
                ++i;
            }
        }
    };
    while (i < n) {
        const QChar c = source[i];
        const QChar next = i + 1 < n ? source[i + 1] : QChar();
        if (c == '/' && next == '/') {
            while (i < n && source[i] != '\n')
                ++i;
        } else if (c == '/' && next == '*') {
            code += ' ';
            for (i += 2; i < n && !(source[i] == '*' && i + 1 < n && source[i + 1] == '/'); ++i)
                if (source[i] == '\n')
                    code += '\n';
            i += 2;
        } else if (c == '"' || c == '\'') {
            // A string's words are not code; one word alone stays, as a
            // name (fetch's "jpeg", field's "effect").
            const qsizetype from = i + 1;
            bool word = true;
            for (++i; i < n && source[i] != c && source[i] != '\n'; ++i) {
                word &= source[i].isLetterOrNumber() || source[i] == '_';
                if (source[i] == '\\')
                    ++i;
            }
            code += c;
            if (word)
                code += source.mid(from, i - from);
            code += c;
            ++i;
        } else if (c == '`') {
            code += c;
            ++i;
            templateWords();
        } else if (c == '/' && valueNext()) {
            // Its flags follow, as a word.
            code += QStringLiteral("//");
            bool inClass = false;
            for (++i; i < n && source[i] != '\n'; ++i) {
                if (source[i] == '\\')
                    ++i;
                else if (source[i] == '[')
                    inClass = true;
                else if (source[i] == ']')
                    inClass = false;
                else if (source[i] == '/' && !inClass)
                    break;
            }
            ++i;
        } else if (c == '}' && !templates.isEmpty() && templates.last() == depth - 1) {
            code += c;
            ++i;
            --depth;
            templates.removeLast();
            templateWords();
        } else {
            if (c == '{')
                ++depth;
            else if (c == '}')
                --depth;
            code += c;
            ++i;
        }
    }
    return code;
}

// Where the bracket at `open` closes, in code alone; else its end.
qsizetype closing(const QString &code, qsizetype open) {
    const QChar in = code[open], out = in == '(' ? ')' : in == '[' ? ']' : '}';
    int depth = 0;
    for (qsizetype k = open; k < code.size(); ++k) {
        if (code[k] == in)
            ++depth;
        else if (code[k] == out && --depth == 0)
            return k;
    }
    return code.size() - 1;
}

// An object's own lines from its `{` at `open`: what is inside its
// children, calls and lists left out.
QString ownLines(const QString &code, qsizetype open) {
    const qsizetype end = closing(code, open);
    QString own;
    int depth = 0;
    for (qsizetype k = open; k <= end; ++k) {
        const QChar c = code[k];
        if (c == '{' || c == '(' || c == '[')
            ++depth;
        else if (c == '}' || c == ')' || c == ']')
            --depth;
        else if (depth == 1)
            own += c;
        if (depth != 1 && !own.endsWith('\n'))
            own += '\n';
    }
    return own;
}

} // namespace

QMap<int, QStringList> Plugins::apiUses(const QString &dir) {
    // Each by the API that brought it, and the words that use it: pinhole's
    // own, or what a plugin's root sets. Anything added to what a plugin is
    // given goes here, under the API it is released in.
    static const QList<std::pair<int, QString>> order = {
        {2, "promises"},      {2, "pinhole.settings"}, {2, "pinhole.notify"},   {2, "pinhole.warn"},
        {2, "pinhole.api"},   {2, "pinhole.coveredBy"}, {2, "pinhole.dismissed"}, {2, "hidden"},
        {2, "avgColor"},      {2, "library.field"},    {2, "pinhole.isHidden"}, {2, "run's output"},
        {2, "pinhole.keepAwake"}, {3, "marks"}, {3, "pinhole.state"}, {3, "pinhole.cache"},
        {3, "viewerLayer"}, {3, "an action's order, hint, busyDelay, textColour or complain"}, {3, "fetch's options"},
        {3, "fetch's loop, bounce, long_exposure or jpeg"}, {3, "a photo's effect"},
        {3, "pinhole.setFavorite"}, {3, "library.revision"}, {3, "sources in the manifest"}, {3, "PluginSource"},
        {3, "pinhole.sources"}, {3, "pinhole.chooseFolder"}, {3, "pinhole.dropStep"}, {4, "a photo's thumbPath"},
        {4, "pinhole.display"}, {4, "viewer.hdr"}, {4, "pinhole.srgbProfile"}, {4, "SdrVideo"}};
    static const QList<std::pair<QString, QRegularExpression>> words = {
        // Its root's marks, each a PluginMark.
        {"marks", QRegularExpression(QStringLiteral("\\bPluginMark\\s*\\{"))},
        {"promises", QRegularExpression(QStringLiteral("\\.\\s*then\\s*\\("))},
        {"pinhole.settings", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*settings\\b"))},
        {"pinhole.notify", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*notify\\b"))},
        {"pinhole.warn", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*warn\\b"))},
        {"pinhole.api", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*api\\b"))},
        {"pinhole.coveredBy", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*coveredBy\\b"))},
        {"pinhole.dismissed", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*dismissed\\b"))},
        // A photo's, as library.item gives it.
        {"avgColor", QRegularExpression(QStringLiteral("\\.\\s*avgColor\\b"))},
        // pinhole.library's, or by a name of the plugin's for it.
        {"library.field", QRegularExpression(QStringLiteral("\\blibrary\\s*\\.\\s*field\\s*\\("))},
        {"pinhole.isHidden",
         QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*(isHidden|hiddenCount|hiddenChanged)\\b"))},
        {"pinhole.keepAwake", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*keepAwake\\b"))},
        {"pinhole.state", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*state\\b"))},
        {"pinhole.cache", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*cache\\b"))},
        {"viewerLayer", QRegularExpression(QStringLiteral("\\bviewerLayer\\s*:"))},
        {"pinhole.setFavorite", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*setFavorite\\b"))},
        // Its root's sources, each a PluginSource.
        {"PluginSource", QRegularExpression(QStringLiteral("\\bPluginSource\\s*\\{"))},
        {"pinhole.sources", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*sources\\b"))},
        {"pinhole.chooseFolder", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*chooseFolder\\b"))},
        {"pinhole.dropStep", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*dropStep\\b"))},
        // pinhole.library's, or by a name of the plugin's for it.
        {"library.revision", QRegularExpression(QStringLiteral("\\blibrary\\s*\\.\\s*revision\\b"))},
        {"an action's order, hint, busyDelay, textColour or complain", QRegularExpression(QStringLiteral("\\.\\s*complain\\s*\\("))},
        // The kinds fetch has had since API 3, named where they are not in
        // the call itself (Background's `kind: "loop"`).
        {"fetch's loop, bounce, long_exposure or jpeg",
         QRegularExpression(QStringLiteral("\\bkind\\s*[:=]\\s*[\"'](loop|bounce|long_exposure|jpeg)[\"']"))},
        {"a photo's effect", QRegularExpression(QStringLiteral("\\bfield\\s*\\(\\s*[\"']effect[\"']\\s*\\)"))},
        // The thumbnail's file beside its URL. (image://file, API 4's too,
        // is in a string, whose words are not looked at.)
        {"a photo's thumbPath", QRegularExpression(QStringLiteral("\\.\\s*thumbPath\\b"))},
        {"pinhole.display", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*display\\b"))},
        // pinhole.viewer's, or by a name of the plugin's for it.
        {"viewer.hdr", QRegularExpression(QStringLiteral("\\bviewer\\s*\\.\\s*hdr\\b"))},
        {"SdrVideo", QRegularExpression(QStringLiteral("\\bSdrVideo\\s*\\{"))},
        {"pinhole.srgbProfile", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*srgbProfile\\b"))},
    };
    // pinhole's signals, in a Connections to it.
    static const QRegularExpression connections(QStringLiteral("\\bConnections\\s*\\{"));
    static const QRegularExpression toPinhole(
        QStringLiteral("\\btarget\\s*:\\s*([\\w$]+\\s*\\.\\s*)*pinhole\\s*[;}\\n]"));
    static const QList<std::pair<QString, QRegularExpression>> handlers = {
        {"pinhole.coveredBy", QRegularExpression(QStringLiteral("\\bonCoveredByChanged\\b"))},
        {"pinhole.dismissed", QRegularExpression(QStringLiteral("\\bonDismissed\\b"))},
        {"pinhole.isHidden", QRegularExpression(QStringLiteral("\\bonHiddenChanged\\b"))},
    };
    // run's options, in its call.
    static const QRegularExpression run(QStringLiteral("\\bpinhole\\s*\\.\\s*run\\s*\\("));
    static const QRegularExpression output(QStringLiteral("\\boutput\\s*:"));
    // fetch's, in its call.
    static const QRegularExpression fetch(QStringLiteral("\\bpinhole\\s*\\.\\s*fetch\\s*\\("));
    static const QRegularExpression fetchOptions(QStringLiteral("\\bstream\\s*:"));
    static const QRegularExpression fetchKinds(QStringLiteral("[\"'](loop|bounce|long_exposure|jpeg)[\"']"));
    // A photo's effect, as library.item gives it: whatever is before the
    // dot but a layer (Qt Quick's own `layer.effect`).
    static const QRegularExpression effect(QStringLiteral("(?:\\b([\\w$]+)|[\\])])\\s*\\.\\s*effect\\b"));
    // A PluginAction's own lines that say its order, hint, busyDelay or
    // textColour.
    static const QRegularExpression action(QStringLiteral("\\bPluginAction\\s*\\{"));
    static const QRegularExpression orderOrHint(QStringLiteral("(^|[;\\s])(order|hint|busyDelay|textColour)\\s*:"));
    // A file's root object, its id, and its hidden set.
    static const QRegularExpression root(QStringLiteral("([\\w.]+)\\s*\\{"));
    static const QRegularExpression rootId(QStringLiteral("(^|[;\\s])id\\s*:\\s*([\\w$]+)"));
    static const QRegularExpression hiddenSet(QStringLiteral("(^|[;\\s])hidden\\s*:"));

    QSet<QString> uses;
    QDirIterator it(dir, {"*.qml", "*.js"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (file.fileName().contains(QLatin1String("/target/")) || !file.open(QIODevice::ReadOnly))
            continue;
        const QString code = codeOnly(QString::fromUtf8(file.readAll()));
        for (const auto &[name, pattern] : words)
            if (code.contains(pattern))
                uses << name;
        for (auto match = connections.globalMatch(code); match.hasNext();) {
            const qsizetype open = match.next().capturedEnd() - 1;
            const QString block = code.mid(open, closing(code, open) - open + 1);
            if (block.contains(toPinhole))
                for (const auto &[name, pattern] : handlers)
                    if (block.contains(pattern))
                        uses << name;
        }
        for (auto match = run.globalMatch(code); match.hasNext();) {
            const qsizetype open = match.next().capturedEnd() - 1;
            if (code.mid(open, closing(code, open) - open + 1).contains(output))
                uses << QStringLiteral("run's output");
        }
        for (auto match = fetch.globalMatch(code); match.hasNext();) {
            const qsizetype open = match.next().capturedEnd() - 1;
            const QString call = code.mid(open, closing(code, open) - open + 1);
            if (call.contains(fetchOptions))
                uses << QStringLiteral("fetch's options");
            if (call.contains(fetchKinds))
                uses << QStringLiteral("fetch's loop, bounce, long_exposure or jpeg");
        }
        for (auto match = effect.globalMatch(code); match.hasNext();)
            if (match.next().captured(1) != QLatin1String("layer"))
                uses << QStringLiteral("a photo's effect");
        for (auto match = action.globalMatch(code); match.hasNext();) {
            const qsizetype open = match.next().capturedEnd() - 1;
            if (ownLines(code, open).contains(orderOrHint))
                uses << QStringLiteral("an action's order, hint, busyDelay, textColour or complain");
        }
        // A PinholePlugin root's hidden: set where it is, or by its id.
        const auto top = root.match(code);
        if (!file.fileName().endsWith(QLatin1String(".qml")) || !top.hasMatch()
            || top.captured(1).section('.', -1) != QLatin1String("PinholePlugin"))
            continue;
        const QString own = ownLines(code, top.capturedEnd() - 1);
        const auto id = rootId.match(own);
        if (own.contains(hiddenSet)
            || (id.hasMatch()
                && code.contains(QRegularExpression("\\b" + QRegularExpression::escape(id.captured(2)) + "\\s*\\.\\s*hidden\\s*=(?!=)"))))
            uses << QStringLiteral("hidden");
    }
    // Sources of its own, in its manifest.
    if (QFile manifest(dir + "/manifest.json");
        manifest.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(manifest.readAll()).object().contains(QLatin1String("sources")))
        uses << QStringLiteral("sources in the manifest");
    QMap<int, QStringList> byApi;
    for (const auto &[version, name] : order)
        if (uses.contains(name))
            byApi[version] << name;
    return byApi;
}

QStringList Plugins::nameClashes(const QString &dir, const QList<std::pair<QString, QString>> &others) {
    // Its files by name, as a qsTr() names them (Translator).
    auto filesIn = [](const QString &folder, QDirIterator::IteratorFlags flags) {
        QMap<QString, QString> files;
        QDirIterator it(folder, {"*.qml", "*.js"}, QDir::Files, flags);
        while (it.hasNext()) {
            const QFileInfo info(it.next());
            if (!info.filePath().contains(QLatin1String("/target/")))
                files.insert(info.completeBaseName(), info.fileName());
        }
        return files;
    };
    const QMap<QString, QString> own = filesIn(dir, QDirIterator::Subdirectories);
    const QMap<QString, QString> pinholes = filesIn(QStringLiteral(":/"), QDirIterator::NoIteratorFlags);
    const QString self = QDir(dir).canonicalPath();
    QStringList clashes;
    for (auto file = own.cbegin(); file != own.cend(); ++file) {
        // A main file is told apart by its URL.
        if (file.key() == QLatin1String("Main"))
            continue;
        QStringList whose;
        if (pinholes.contains(file.key()))
            whose << QStringLiteral("Pinhole's own %1").arg(pinholes.value(file.key()));
        for (const auto &[id, folder] : others) {
            if (QDir(folder).canonicalPath() == self)
                continue;
            if (const QMap<QString, QString> theirs = filesIn(folder, QDirIterator::Subdirectories); theirs.contains(file.key()))
                whose << QStringLiteral("%1's %2").arg(id, theirs.value(file.key()));
        }
        if (!whose.isEmpty())
            clashes << QStringLiteral("%1 is named as %2").arg(file.value(), whose.join(QStringLiteral(", ")));
    }
    return clashes;
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
    return sourcesProblem(dir, id, *manifest);
}

QStringList Plugins::sourceKinds(const QJsonObject &manifest) {
    QStringList kinds;
    for (const QJsonValue &source : manifest.value("sources").toArray())
        if (const QString kind = source.toObject().value("kind").toString(); !kind.isEmpty())
            kinds << kind;
    return kinds;
}

QString Plugins::sourcesProblem(const QString &dir, const QString &id, const QJsonObject &manifest) {
    const QJsonValue declared = manifest.value("sources");
    if (declared.isUndefined())
        return {};
    if (!declared.isArray())
        return tr("manifest.json's \"sources\" is a list");
    // Pinhole's own plugins' folder; the kinds its plugins bring with no
    // provider (pinholed's own); and those of its plugin of this id.
    const QString builtIn = QDir(builtInDir()).canonicalPath();
    const bool own = !builtIn.isEmpty() && QDir(dir).canonicalPath().startsWith(builtIn + '/');
    QStringList ownKinds, pinholeds;
    for (const QFileInfo &each : QDir(builtInDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile file(each.filePath() + "/manifest.json");
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject theirs = QJsonDocument::fromJson(file.readAll()).object();
        for (const QJsonValue &source : theirs.value("sources").toArray())
            if (!source.toObject().contains("provider"))
                pinholeds << source.toObject().value("kind").toString();
        if (theirs.value("id").toString(each.fileName()) == id)
            ownKinds = sourceKinds(theirs);
    }
    static const QRegularExpression named(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    static const QRegularExpression extension(QStringLiteral("^[a-z0-9]{1,16}$"));
    QStringList seen;
    for (const QJsonValue &each : declared.toArray()) {
        const QJsonObject source = each.toObject();
        const QString kind = source.value("kind").toString();
        if (!each.isObject() || kind.isEmpty())
            return tr("each of manifest.json's \"sources\" names its \"kind\"");
        if (seen.contains(kind))
            return tr("\"sources\": %1 is named twice").arg(kind);
        seen << kind;
        // A kind with no provider is one of pinholed's own (iCloud's,
        // folders'): only Pinhole's own plugin of it brings it, or one's own
        // copy of that plugin.
        if (!source.contains("provider")) {
            if (!own && !ownKinds.contains(kind))
                return tr("\"sources\": %1 is pinholed's own kind, which only Pinhole's own plugin of it brings").arg(kind);
            continue;
        }
        // One of its own, run by its provider: as pinholed takes it
        // (docs/source-providers.md), so what it would refuse is said here.
        const auto wrong = [&kind](const QString &why) { return tr("\"sources\": %1: %2").arg(kind, why); };
        if (manifest.value("api").toInt(1) < 3)
            return wrong(tr("a source needs \"api\": 3"));
        if (!named.match(kind).hasMatch())
            return wrong(tr("a kind is letters, digits, '.', '_' and '-'"));
        if (kind != id && !kind.startsWith(id + '.'))
            return wrong(tr("its kind must be the plugin's id, %1, or start with %1 and a dot").arg(id));
        if (pinholeds.contains(kind))
            return wrong(tr("that kind is pinholed's own"));
        const QString provider = source.value("provider").toString();
        if (!safeEntry(provider))
            return wrong(tr("\"provider\" is a path in the plugin's folder, without \"..\""));
        if (const QFileInfo program(dir + '/' + provider); !program.isFile() || !program.isExecutable())
            return wrong(tr("%1 is not a program in the plugin's folder (a file it can run)").arg(provider));
        if (source.contains("needs_sign_in") && !source.value("needs_sign_in").isBool())
            return wrong(tr("\"needs_sign_in\" is true or false"));
        if (source.contains("accepts")) {
            const QJsonValue accepts = source.value("accepts");
            bool fine = accepts.isArray();
            for (const QJsonValue &e : accepts.toArray())
                fine = fine && extension.match(e.toString()).hasMatch();
            if (!fine)
                return wrong(tr("\"accepts\" is a list of extensions in lower case, without the dot"));
        }
        if (source.contains("rounds_every_secs")) {
            const double every = source.value("rounds_every_secs").toDouble(-1);
            if (every != std::floor(every) || every < 5 || every > 86400)
                return wrong(tr("\"rounds_every_secs\" is a whole number from 5 to 86400"));
        }
    }
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
            plugin->sources = sourceKinds(manifest);
            plugin->stamp = stampOf(plugin->dir);
            // One's own with a built-in's id takes its place.
            const auto same = std::find_if(found.begin(), found.end(), [&](const auto &p) { return p->id == plugin->id; });
            if (same != found.end()) {
                plugin->copy = personal && !(*same)->personal;
                found.erase(same);
            }
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
    const QMap<int, QStringList> uses = Plugins::apiUses(dir);
    const int needed = uses.isEmpty() ? 1 : uses.lastKey();
    for (auto it = uses.begin(); it != uses.end(); ++it)
        if (asked < it.key())
            err(QStringLiteral("%1: warning: it uses %2, of plugin API %3, and its manifest asks for API %4: give it \"api\": %5.")
                    .arg(name, it.value().join(QStringLiteral(", ")))
                    .arg(it.key())
                    .arg(asked)
                    .arg(needed));
    // A file named as another plugin's, or as one of Pinhole's own: Qt
    // tells a qsTr() only by its file's name. One of the same id is the
    // one it replaces, or its copy.
    QList<std::pair<QString, QString>> others;
    for (const QString &root : {Plugins::personalDir(), Plugins::builtInDir()})
        for (const QFileInfo &each : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            QFile file(each.filePath() + "/manifest.json");
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QString other = QJsonDocument::fromJson(file.readAll()).object().value("id").toString(each.fileName());
            if (other != id)
                others.append({other, each.filePath()});
        }
    for (const QString &clash : Plugins::nameClashes(dir, others))
        err(QStringLiteral("%1: warning: %2: a qsTr() in it may be given that one's words; give it a name of its own.")
                .arg(name, clash));
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
