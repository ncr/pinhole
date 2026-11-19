#include "plugins.h"
#include "pluginkeystore.h"
#include "propertymap.h"
#include "synced.h"
#include "panzoom.h"
#include "pluginguide.h"
#include "videoview.h"
#include "selftest.h"

#include "favourites.h"
#include "history.h"
#include "library.h"
#include "photogrid.h"
#include "photoprovider.h"
#include "scroller.h"
#include "serviceclient.h"
#include "sources.h"
#include "storage.h"
#include "translator.h"
#include "keymap.h"
#include "viewer.h"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QMap>
#include <QDirIterator>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>

PluginSettings::PluginSettings(PluginHost *host) : QQmlPropertyMap(this, nullptr), m_host(host) {
    const QJsonObject kept = host->m_plugins->settingsOf(host->id());
    QVariantHash all;
    for (auto it = kept.begin(); it != kept.end(); ++it)
        all.insert(it.key(), it.value().toVariant());
    insertAll(this, all);
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
    // Taken out: its default, or undefined, as one never set is.
    if (json.isNull() || json.isUndefined())
        return m_host->m_settingDefaults.value(key);
    return value;
}

namespace {

QString configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/plugins.json";
}

// Pinhole's own plugins once under another id, and the one they have now:
// their switch, settings, state and cache follow them, once.
const std::pair<const char *, const char *> renamedPlugins[] = {
    {"me.people", "pinhole.people"},
};

QJsonObject readConfig() {
    QFile file(configPath());
    QJsonObject config = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    file.close();
    bool changed = false;
    for (const auto &[was, is] : renamedPlugins) {
        const QString from = QString::fromLatin1(was), to = QString::fromLatin1(is);
        for (const QString section : {"enabled", "settings"}) {
            QJsonObject part = config.value(section).toObject();
            if (part.contains(from) && !part.contains(to)) {
                part.insert(to, part.value(from));
                part.remove(from);
                config.insert(section, part);
                changed = true;
            }
        }
        for (const QString &root : {Plugins::stateRoot(), Plugins::cacheRoot()}) {
            const QString old = root + '/' + from, now = root + '/' + to;
            if (!QFileInfo::exists(old))
                continue;
            if (!QFileInfo::exists(now)) {
                QDir().rename(old, now);
                continue;
            }
            // Begun under the new id already (pinholed's worker before this
            // window): what is only under the old one goes over.
            for (const QString &name : QDir(old).entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
                if (!QFileInfo::exists(now + '/' + name))
                    QDir().rename(old + '/' + name, now + '/' + name);
            QDir().rmdir(old);
        }
    }
    if (changed) {
        QSaveFile saved(configPath());
        if (saved.open(QIODevice::WriteOnly) && saved.write(QJsonDocument(config).toJson()) >= 0)
            saved.commit();
    }
    return config;
}

QJsonObject readJson(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

QJsonObject readToured() {
    return readJson(Plugins::touredPath());
}

} // namespace

PluginHost::PluginHost(const QString &id, const QString &dir, Plugins *plugins)
    : QObject(plugins), m_id(id), m_dir(dir), m_plugins(plugins) {
    m_settings = std::make_unique<PluginSettings>(this);
    connect(plugins, &Plugins::selectionChanged, this, &PluginHost::selectionChanged);
    connect(plugins, &Plugins::hiddenChanged, this, &PluginHost::hiddenChanged);
    // A plugin turned on or off, or loaded again: what fetch can ask may too.
    connect(plugins, &Plugins::changed, this, &PluginHost::fetchableChanged);
    connect(this, &PluginHost::notified, plugins, [this](const QString &text) { emit m_plugins->notice(m_id, text); });
}

PluginHost::~PluginHost() {
    endPrograms(close());
}

int PluginHost::api() const { return Plugins::api; }

void Plugins::startupLoaded(const QString &id, double ms) const {
    if (!qEnvironmentVariableIsSet("PINHOLE_STARTUP_TRACE")) return;
    const auto json = QJsonDocument(QJsonObject{{"id", id}, {"ms", ms}}).toJson(QJsonDocument::Compact);
    std::fprintf(stderr, "pinhole plugin startup: %s\n", json.constData());
}

QString PluginHost::coveredBy() const { return m_plugins->coveredBy(); }

QObject *PluginHost::synced() const {
    if (!m_synced)
        m_synced = std::make_unique<PluginSyncedMap>(m_plugins->syncedStore(), m_id);
    return m_synced.get();
}

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

QObject *PluginHost::store(const QString &name) {
    if (!PluginKeyStore::usableName(name)) {
        qWarning("pinhole: %s: pinhole.store(\"%s\"): a store's name is lowercase letters, digits, '_' and '-'",
                 qPrintable(m_id), qPrintable(name));
        return nullptr;
    }
    PluginKeyStore *&store = m_stores[name];
    if (!store) {
        // Its host's child: the plugin's QML never takes it over.
        store = new PluginKeyStore(this, name);
        QQmlEngine::setObjectOwnership(store, QQmlEngine::CppOwnership);
    }
    return store;
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

QVariantList PluginHost::workers() const {
    QVariantList mine;
    for (const QJsonValue &each : m_plugins->serviceWorkers()) {
        const QJsonObject worker = each.toObject();
        if (worker.value("plugin").toString() != m_id)
            continue;
        const QJsonValue exit = worker.value("last_exit");
        mine.append(QVariantMap{
            {"name", worker.value("name").toString()},
            {"running", worker.value("running").toBool()},
            {"since", worker.value("since").toDouble()},
            {"key", worker.value("key").toString()},
            {"lastExit", exit.isObject() ? exit.toObject().toVariantMap() : QVariant()},
            // What it said last while it runs: a panel opened
            // after shows how far it is at once.
            {"said", worker.value("said").isObject() ? worker.value("said").toObject().toVariantMap() : QVariant()},
        });
    }
    return mine;
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

QObject *PluginHost::viewer() const { return m_plugins->viewerApi(); }

PluginViewer::PluginViewer(Viewer *viewer, PluginLibrary *library, QObject *parent)
    : QObject(parent), m_viewer(viewer), m_library(library) {
    connect(viewer, &Viewer::openChanged, this, &PluginViewer::openChanged);
    connect(viewer, &Viewer::indexChanged, this, &PluginViewer::indexChanged);
    connect(viewer, &Viewer::hdrChanged, this, &PluginViewer::hdrChanged);
    // Another photo, or what is known of this one changed (a favourite, an
    // edit, the library read again).
    connect(viewer, &Viewer::openChanged, this, &PluginViewer::photoChanged);
    connect(viewer, &Viewer::indexChanged, this, &PluginViewer::photoChanged);
    if (library)
        connect(library, &PluginLibrary::revisionChanged, this, &PluginViewer::photoChanged);
}

QVariantMap PluginViewer::photo() const {
    if (!m_library || !m_viewer->isOpen() || m_viewer->index() < 0)
        return {};
    return m_library->item(QJSValue(m_viewer->index()));
}

QString PluginViewer::id() const { return photo().value(QStringLiteral("id")).toString(); }

bool PluginViewer::open() const { return m_viewer->isOpen(); }
int PluginViewer::index() const { return m_viewer->index(); }
QVariantMap PluginViewer::hdr() const { return m_viewer->hdr(); }
void PluginViewer::close() { m_viewer->close(); }

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

QStringList PluginHost::selectionIds() const {
    QStringList ids;
    for (int index : selection())
        if (const QString id = m_plugins->library()->item(index).value(QStringLiteral("id")).toString(); !id.isEmpty())
            ids.append(id);
    return ids;
}

int PluginHost::selectionCount() const { return m_plugins->grid() ? m_plugins->grid()->selectionCount() : 0; }

bool PluginHost::isHidden(const QJSValue &photo) const {
    return m_plugins->grid() && m_plugins->grid()->isHidden(m_plugins->library()->positionOf(photo));
}

int PluginHost::hiddenCount() const { return m_plugins->grid() ? m_plugins->grid()->hiddenCount() : 0; }

void Plugins::setKeymap(Keymap *keymap) {
    m_keymap = keymap;
    connect(keymap, &Keymap::inputChanged, this, [this] {
        for (const auto &p : m_plugins)
            if (p->host)
                emit p->host->keyboardInUseChanged();
    });
    const auto keys = [this] {
        for (const auto &p : m_plugins)
            if (p->host)
                emit p->host->hasKeysChanged();
    };
    connect(keymap, &Keymap::layerChanged, this, keys);
    connect(keymap, &Keymap::pluginChanged, this, keys);
}

QString PluginHost::input() const {
    return m_plugins->keymap() ? m_plugins->keymap()->input() : QString();
}

bool PluginHost::hasKeys() const {
    const Keymap *keymap = m_plugins->keymap();
    return keymap && keymap->layer() == QLatin1String("plugin") && keymap->plugin() == m_id;
}

QList<QObject *> PluginHost::fileActions() const { return m_plugins->fileActions(); }
QStringList PluginHost::fetchable() const { return m_plugins->fetchable(); }

QStringList Plugins::fetchable() const {
    QStringList kinds = {"medium", "full", "original", "jpeg", "video", "live", "loop"};
    for (const auto &p : m_plugins)
        if (p->error.isEmpty() && enabled(*p))
            for (const QString &kind : p->makes)
                if (!kinds.contains(kind))
                    kinds << kind;
    return kinds;
}

void Plugins::setFileActions(const QList<QObject *> &actions) {
    QList<QPointer<QObject>> now(actions.begin(), actions.end());
    if (now == m_fileActions)
        return;
    m_fileActions = now;
    for (const auto &p : m_plugins)
        if (p->host)
            emit p->host->fileActionsChanged();
}

QList<QObject *> Plugins::fileActions() const {
    QList<QObject *> actions;
    for (const QPointer<QObject> &action : m_fileActions)
        if (action)
            actions << action.data();
    return actions;
}

bool PluginHost::keyboardInUse() const {
    return m_plugins->keymap() && m_plugins->keymap()->input() == QLatin1String("keyboard");
}

void Plugins::setGrid(PhotoGrid *grid) {
    m_grid = grid;
    m_keepAwake.setWindow(grid->window());
    connect(grid, &PhotoGrid::selectionChanged, this, &Plugins::selectionChanged);
    // Told once plugins have the library as it is now, the grid's news of
    // a reload included, so isHidden(id) and their own positions agree.
    connect(grid, &PhotoGrid::hiddenChanged, this, [this] {
        if (m_libraryApi->stale()) {
            m_hiddenPending = true;
        } else {
            emit hiddenChanged();
            m_libraryApi->settle(false, true);
        }
    });
    m_libraryApi->setHidden([this](int index) { return m_grid && m_grid->isHidden(index); });
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

void PluginHost::openPhotos(const QJSValue &photos, const QJSValue &photo, const QJSValue &options) {
    const PluginLibrary *library = m_plugins->library();
    const int index = library->positionOf(photo);
    if (!m_plugins->grid() || index < 0)
        return;
    std::vector<int> among;
    const int n = photos.property(QStringLiteral("length")).toInt();
    among.reserve(size_t(std::max(0, n)));
    for (int i = 0; i < n; ++i)
        among.push_back(library->positionOf(photos.property(quint32(i))));
    m_plugins->handedOver(m_id);
    if (options.property(QStringLiteral("comeBack")).toBool() && !m_away) {
        m_away = true;
        m_awayInterrupted = false;
        emit awayChanged();
    }
    m_plugins->viewerObject()->openAmong(among, index);
}

void PluginHost::cameBack(const QString &photo) {
    if (!m_away)
        return;
    m_away = false;
    const bool interrupted = std::exchange(m_awayInterrupted, false);
    emit awayChanged();
    // Another plugin went over the photos meanwhile: the way back leads to
    // it now, and this one closes for good.
    if (interrupted)
        emit dismissed();
    else
        emit returned(photo);
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
    // are those kept), back to its default if it has one.
    for (const QString &key : m_settings->QQmlPropertyMap::keys())
        if (!kept.contains(key) && m_settings->value(key) != m_settingDefaults.value(key)) {
            if (m_settingDefaults.contains(key))
                m_settings->insert(key, m_settingDefaults.value(key));
            else
                m_settings->clear(key);
        }
}

void PluginHost::setSettingDefaults(const QVariantMap &defaults) {
    QVariantMap plain;
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        plain.insert(it.key(), it.value().metaType() == QMetaType::fromType<QJSValue>()
                                   ? it.value().value<QJSValue>().toVariant(QJSValue::ConvertJSObjects)
                                   : it.value());
    m_settingDefaults = plain;
    const QJsonObject kept = m_plugins->settingsOf(m_id);
    for (auto it = plain.begin(); it != plain.end(); ++it)
        if (!kept.contains(it.key()) && m_settings->value(it.key()) != it.value())
            m_settings->insert(it.key(), it.value());
    settingsChanged();
}

void PluginHost::showFile(const QString &path) const {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

QString PluginHost::picturesDir() const { return picturesLocation(); }

QString PluginHost::picturePath(const QString &name, const QString &extension) const {
    const QString dir = picturesLocation() + QStringLiteral("/Pinhole");
    QDir().mkpath(dir);
    // A name, not a path: no folders, nor a hidden file.
    QString base = name.trimmed();
    base.replace(QLatin1Char('/'), QLatin1Char('-'));
    while (base.startsWith(QLatin1Char('.')))
        base.remove(0, 1);
    if (base.isEmpty())
        base = QStringLiteral("Pinhole");
    QString ext = extension.trimmed();
    while (ext.startsWith(QLatin1Char('.')))
        ext.remove(0, 1);
    const QString dot = ext.isEmpty() ? QString() : QLatin1Char('.') + ext;
    QString path = dir + QLatin1Char('/') + base + dot;
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = dir + QLatin1Char('/') + base + QLatin1Char(' ') + QString::number(n) + dot;
    return path;
}

bool PluginHost::exists(const QString &path) const { return QFileInfo::exists(path); }

bool PluginHost::has(const QString &program) const {
    if (program.isEmpty() || program.contains(QLatin1String("..")))
        return false;
    // Its own, by a path in its folder; else on PATH, as run() finds it.
    if (program.contains('/'))
        return !program.startsWith('/') && QFileInfo(m_dir + '/' + program).isExecutable();
    return !QStandardPaths::findExecutable(program).isEmpty();
}

double PluginHost::fileSize(const QString &path) const { return double(QFileInfo(path).size()); }

QString PluginHost::sizeText(double bytes, int decimals) const {
    return QLocale().formattedDataSize(qint64(bytes), std::clamp(decimals, 0, 3), QLocale::DataSizeTraditionalFormat);
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
      m_libraryApi(std::make_unique<PluginLibrary>(library)),
      m_viewerApi(viewer ? std::make_unique<PluginViewer>(viewer, m_libraryApi.get()) : nullptr), m_config(readConfig()),
      m_stateStore(stateRoot(), QStringLiteral("state.json"), &m_files),
      m_cacheStore(cacheRoot(), QStringLiteral("cache.json"), &m_files),
      m_syncedStore(std::make_unique<SyncedStore>()), m_toured(readToured()),
      m_putOff(readJson(putOffPath())) {
    // pinholed's workers: as each window connects (it hears the source
    // first), and as they change.
    if (m_service)
        connect(m_service, &ServiceClient::announced, this, [this](const QString &name, const QJsonObject &details) {
            const auto take = [this](const QJsonArray &workers) {
                if (workers == m_workers)
                    return;
                m_workers = workers;
                for (const auto &p : m_plugins)
                    if (p->host)
                        emit p->host->workersChanged();
            };
            if (name == QLatin1String("workers"))
                take(details.value("workers").toArray());
            else if (name == QLatin1String("worker_said")) {
                const QString plugin = details.value("plugin").toString();
                // Its `said` in pinhole.workers too, for what reads it later
                // (no workersChanged: workerSaid is the news).
                for (qsizetype i = 0; i < m_workers.size(); ++i) {
                    QJsonObject worker = m_workers.at(i).toObject();
                    if (worker.value("plugin").toString() == plugin
                        && worker.value("name").toString() == details.value("name").toString()) {
                        worker.insert(QStringLiteral("said"), details.value("said"));
                        m_workers.replace(i, worker);
                    }
                }
                for (const auto &p : m_plugins)
                    if (p->host && p->id == plugin)
                        emit p->host->workerSaid(details.value("name").toString(), details.value("said").toObject().toVariantMap());
            }
            else if (name == QLatin1String("source"))
                m_service->call(QStringLiteral("workers"), {}, [take](bool ok, const QJsonObject &result, const QString &) {
                    if (ok)
                        take(result.value("workers").toArray());
                });
        });
    // The photo a plugin opened to come back from closed: back to it, on
    // the one shown last.
    if (m_viewer)
        connect(m_viewer, &Viewer::openChanged, this, [this] {
            if (m_viewer->isOpen())
                return;
            const QString photo = m_libraryApi->item(m_viewer->index()).value(QStringLiteral("id")).toString();
            for (const auto &p : m_plugins)
                if (p->host)
                    p->host->cameBack(photo);
        });
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
    connect(m_syncedStore.get(), &SyncedStore::failed, this, failed(tr("What it keeps of the library could not be kept.")));
    // Saving a file writes it in steps, and editors save several at once:
    // one reload once they are done.
    m_settle.setSingleShot(true);
    m_settle.setInterval(300);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        if (m_held)
            m_changedWhileHeld = true;
        else
            rescan();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_settle, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_settle, qOverload<>(&QTimer::start));
    // Another window's switches and settings, as it writes them.
    m_configSettle.setSingleShot(true);
    m_configSettle.setInterval(150);
    connect(&m_configSettle, &QTimer::timeout, this, &Plugins::configChangedElsewhere);
    connect(&m_configWatcher, &QFileSystemWatcher::directoryChanged, &m_configSettle, qOverload<>(&QTimer::start));
    if (const QString folder = QFileInfo(configPath()).absolutePath(); QDir().mkpath(folder))
        m_configWatcher.addPath(folder);
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

QString Plugins::putOffPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/pinhole/new-plugins.json";
}

void Plugins::flush() {
    m_stateStore.flush();
    m_cacheStore.flush();
    m_syncedStore->flush();
    m_files.wait();
}

void Plugins::registerTypes() {
    for (const char *type : {"PinholePlugin", "PluginAction", "PluginSource", "Command", "ShellButton", "Chip", "UiText", "Spinner",
                             "Toggle", "FieldInput", "ScrollList", "ScrollGrid", "ScrollArea", "PinchZoom", "Panel", "Card",
                             "Choices", "PhotoTiles", "PanelHeader", "SettingNote", "SettingSwitch", "PluginView", "Job"})
        qmlRegisterType(QUrl(QStringLiteral("qrc:/%1.qml").arg(QLatin1String(type))), "Pinhole.Plugin", 1, 0, type);
    qmlRegisterType<PluginMark>("Pinhole.Plugin", 1, 0, "PluginMark");
    // A player's video drawn right, HDR tone mapped on the GPU: what
    // a plugin shows video with, never a VideoOutput.
    qmlRegisterType<VideoView>("Pinhole.Plugin", 1, 0, "VideoView");
    // The grid's scrolling over a Flickable of one's own (scroller.h, API
    // 4): ScrollList, ScrollGrid and ScrollArea are each one with it.
    qmlRegisterType<Scroller>("Pinhole.Plugin", 1, 0, "Scroller");
    // A surface panned and zoomed without levels, on the grid's physics
    // (panzoom.h): a map, a picture larger than its room.
    qmlRegisterType<PanZoom>("Pinhole.Plugin", 1, 0, "PanZoom");
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
    if (marks.isEmpty() && !m_marks.contains(id))
        return;
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
    addModules(engine);
    connect(engine, &QQmlEngine::warnings, this, &Plugins::warned);
}

QJSEngine *Plugins::engine() const { return m_engine; }

void Plugins::addModules(QQmlEngine *engine) {
    for (const QFileInfo &plugin : QDir(builtInDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (QFileInfo(plugin.filePath() + "/qml").isDir())
            engine->addImportPath(plugin.filePath() + "/qml");
}

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

void Plugins::holdReloads(bool held) {
    if (held == m_held)
        return;
    m_held = held;
    if (!held && std::exchange(m_changedWhileHeld, false))
        rescan();
}

void Plugins::libraryOpened() {
    m_cacheName = cacheName();
    m_syncedStore->setLibrary(m_library->dir());
    if (m_ready)
        return;
    m_ready = true;
    // The library read is the one plugins start with, not a switch to it.
    m_libraryApi->opened();
    // Each on loads as this is said (Main.qml), then the window gathers
    // them all at once.
    m_loadingAll = true;
    emit readyChanged();
    m_loadingAll = false;
    emit allLoaded();
}

void Plugins::libraryReloaded(const std::vector<int> &moved) {
    // pinhole.synced is the library's before pinhole.library says it is
    // another: what a plugin sets as it hears of it lands in that one.
    m_syncedStore->setLibrary(m_library->dir());
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
    m_libraryApi->settle(true, m_hiddenPending);
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

QVariantList Plugins::hiddenOf(const QString &id, const QVariantList &given, bool fresh) {
    Followed &followed = m_followed[id];
    // Ids or positions, as a mark's photos are.
    std::vector<int> list = markPhotos(given);
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
        lists.push_back(markPhotos(stack.toList()));
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
            // What it needs that is not here (its manifest's "requires").
            {"missing", [&] {
                 QStringList missing;
                 for (const QString &program : p->required)
                     if (QStandardPaths::findExecutable(program).isEmpty())
                         missing << program;
                 return missing;
             }()},
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
    QJsonObject config = latestConfig();
    QJsonObject states = config.value("enabled").toObject();
    states.insert(switchKey(id), on);
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
    // One waiting away for its photo to close, under another now: it does
    // not come back over that one.
    for (const auto &p : m_plugins)
        if (p->host && !coveredBy().isEmpty() && coveredBy() != p->id)
            p->host->coveredMeanwhile();
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

void Plugins::putOffNew(const QStringList &ids, int days) {
    m_putOff = QJsonObject{
        {"until", QDateTime::currentMSecsSinceEpoch() + qint64(days) * 24 * 3600 * 1000},
        {"ids", QJsonArray::fromStringList(ids)},
    };
    const QString path = putOffPath();
    const QByteArray bytes = QJsonDocument(m_putOff).toJson();
    m_files.run([path, bytes] { return KeptFiles::write(path, bytes); });
    ++m_revision;
    emit changed();
}

bool Plugins::newPutOff(const QStringList &ids) const {
    if (QDateTime::currentMSecsSinceEpoch() >= qint64(m_putOff.value("until").toDouble()))
        return false;
    const QJsonArray off = m_putOff.value("ids").toArray();
    for (const QString &id : ids)
        if (!off.contains(id))
            return false;
    return true;
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

QString Plugins::switchKey(const QString &id) const {
    for (const auto &p : m_plugins)
        if (p->id == id && p->copy)
            return "copy:" + id;
    return id;
}

bool Plugins::enabled(const Plugin &plugin) const {
    // A copy of one of Pinhole's own has a switch of its own (copy:<id>):
    // the built-in's, turned on once, never turns on a copy half made.
    const QJsonValue state = m_config.value("enabled").toObject().value(plugin.copy ? "copy:" + plugin.id : plugin.id);
    return state.isBool() ? state.toBool() : !plugin.personal && !plugin.startsOff;
}

bool Plugins::writeConfig(QJsonObject config) {
    // After what is being written away from this thread.
    m_files.wait();
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

QJsonObject Plugins::latestConfig() {
    m_files.wait();
    if (!QFileInfo::exists(configPath()))
        return m_config;
    return readConfig();
}

void Plugins::configChangedElsewhere() {
    const QJsonObject fresh = latestConfig();
    if (fresh == m_config)
        return;
    const QJsonObject was = m_config.value("enabled").toObject(), now = fresh.value("enabled").toObject();
    m_config = fresh;
    // Turned off there: gone here as setEnabled has it go. Turned on: this
    // window loads it, as it reads the switch again (`changed`).
    for (const auto &p : m_plugins) {
        if (!p->host || (was.value(switchKey(p->id)) == now.value(switchKey(p->id))))
            continue;
        if (!enabled(*p)) {
            m_history->forget(p->id);
            p->host->stop();
            setMarks(p->id, {});
        }
    }
    for (const auto &p : m_plugins)
        if (p->host)
            p->host->settingsChanged();
    updateCatalogs();
    ++m_revision;
    emit changed();
}

QJsonObject Plugins::settingsOf(const QString &id) const {
    return m_config.value("settings").toObject().value(id).toObject();
}

bool Plugins::setSettingOf(const QString &id, const QString &key, const QJsonValue &value) {
    QJsonObject config = latestConfig();
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
    QString description = error.description();
    // A handler for a signal the type has not (onClicked for a button's
    // tapped): where its signals are said.
    if (description.startsWith(QLatin1String("Cannot assign to non-existent property \"on")))
        description += QStringLiteral(": a handler is \"on\" and one of the type's signals (the kit's: "
                                      "pinhole plugin docs plugin-kit)");
    return where.isEmpty() ? description : where + ": " + description;
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

void Plugins::keyClashed(const QString &id, const QString &message) {
    for (const auto &p : m_plugins)
        if (p->id == id && p->host)
            p->host->report(message);
}

void Plugins::layerFailed(const QString &id, const QString &message) {
    for (const auto &p : m_plugins)
        if (p->id == id && p->host)
            p->host->report(QStringLiteral("viewerLayer: ") + message.trimmed());
}

namespace {
bool importsInternal(const QString &dir) {
    static const QRegularExpression internal(QStringLiteral("^[ \\t]*import\\s+Pinhole\\.Internal\\b"),
                                             QRegularExpression::MultilineOption);
    QDirIterator files(dir, {"*.qml"}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (file.open(QIODevice::ReadOnly) && internal.match(QString::fromUtf8(file.readAll())).hasMatch())
            return true;
    }
    return false;
}
} // namespace

void Plugins::rescan() {
    scan();
}

void Plugins::scan() {
    // What each plugin was, to tell which ones changed on disk.
    QHash<QString, int> loads;
    QHash<QString, QString> stamps;
    for (const auto &p : m_plugins)
        loads.insert(p->id, p->loads);
    for (const auto &p : m_plugins)
        stamps.insert(p->id, p->stamp);

    std::vector<std::unique_ptr<Plugin>> found;
    // Pinhole's own ids, whatever one's own take of them and in which order.
    QSet<QString> builtIns;
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
            plugin->startsOff = !personal && manifest.value("startsOff").toBool();
            // Pinhole.Internal is Pinhole's own plugins' (and copies keeping
            // the id of one there is): it may change with any version, so
            // another's does not load with it, as check says, a made-up
            // pinhole.* id of one's own included.
            const bool own = plugin->id.startsWith(QLatin1String("pinhole.")) && (!personal || builtIns.contains(plugin->id));
            if (plugin->error.isEmpty() && !own && importsInternal(plugin->dir))
                plugin->error = tr("imports Pinhole.Internal, which is Pinhole's own plugins': use what Pinhole.Plugin gives");
            if (plugin->error.isEmpty())
                plugin->main = main;
            plugin->sources = sourceKinds(manifest);
            plugin->settingDefaults = manifest.value("settings").toObject().toVariantMap();
            plugin->makes.clear();
            if (makesProblem(plugin->dir, manifest).isEmpty())
                for (const QJsonValue &each : manifest.value("makes").toArray())
                    plugin->makes << each.toObject().value("kind").toString();
            plugin->required.clear();
            for (const QJsonValue &each : manifest.value("requires").toArray())
                if (const QString name = each.toString(); !name.isEmpty() && !name.contains('/'))
                    plugin->required << name;
            // One's own with a built-in's id takes its place. Two of one's
            // own with one id: which is meant is not known, so neither loads.
            const auto same = std::find_if(found.begin(), found.end(), [&](const auto &p) { return p->id == plugin->id; });
            if (same != found.end()) {
                if (personal && (*same)->personal) {
                    plugin->error = tr("%1 has the same id as %2: give one of them another").arg(name, QFileInfo((*same)->dir).fileName());
                    plugin->main.clear();
                }
                found.erase(same);
            }
            plugin->copy = personal && builtIns.contains(plugin->id);
            if (!personal)
                builtIns.insert(plugin->id);
            found.push_back(std::move(plugin));
        }
    };
    look(builtInDir(), false);
    look(personalDir(), true);
    std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a->personal != b->personal ? !a->personal : a->name.localeAwareCompare(b->name) < 0;
    });

    QStringList paths{personalDir(), builtInDir()};
    // Hosts stay with their plugins across scans, so what the QML loaded
    // holds of one stays good; one loaded again stops what it started.
    for (auto &plugin : found) {
        QStringList parts, directories{plugin->dir};
        paths << plugin->dir;
        while (!directories.isEmpty()) {
            const QDir dir(directories.takeLast());
            for (const QFileInfo &entry : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Unsorted)) {
                const QString path = entry.filePath();
                if (entry.isDir()) {
                    // What a helper makes as it runs or builds is not the
                    // plugin: a build tree, Python's caches, an environment,
                    // anything hidden. Changed, it would load the plugin
                    // again, and stop the helper that changed it.
                    static const QStringList made = {"target", "node_modules", "__pycache__", "venv", "build", "dist"};
                    if (made.contains(entry.fileName()) || entry.fileName().startsWith('.'))
                        continue;
                    if (!entry.isSymLink())
                        directories << path;
                } else {
                    // What Pinhole loads of it: its QML, its JavaScript, its
                    // manifest and words.
                    static const QStringList loaded = {"qml", "js", "mjs", "json"};
                    if (!loaded.contains(entry.suffix()) && entry.fileName() != QLatin1String("qmldir"))
                        continue;
                    parts << path + QString::number(entry.lastModified().toMSecsSinceEpoch()) + QString::number(entry.size());
                }
                paths << path;
            }
        }
        parts.sort();
        plugin->stamp = parts.join('|');

        const auto old = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const auto &p) { return p->id == plugin->id; });
        if (old != m_plugins.end() && (*old)->dir == plugin->dir)
            plugin->host = std::move((*old)->host);
        if (!plugin->host)
            plugin->host = std::make_unique<PluginHost>(plugin->id, plugin->dir, this);
        // Before its QML is made, so a binding on one never set has it.
        plugin->host->setSettingDefaults(plugin->settingDefaults);
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
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    QDir().mkpath(personalDir());
    m_watcher.addPaths(paths);
}

namespace {

const char *kPluginUsage = R"(Usage: pinhole plugin <command>

  new <id> [folder]      begin a plugin of your own in
                         ~/.config/pinhole/plugins/<id> (or the folder
                         named, made for it): a manifest.json, a Main.qml
                         and its Polish to start from
  check [folder | id]    look a plugin over as Pinhole would load it: its
                         manifest, its QML, its translations (the folder
                         here, if none is named)
  docs [name | --json]   read the bundled handbook; --json includes the
                         installed API version and every document

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

int newPlugin(const QString &id, const QString &folder = {}) {
    if (!Plugins::usableId(id)) {
        err(QStringLiteral("pinhole: a plugin's id is letters, digits, dots, dashes: me.favourites"));
        return 2;
    }
    if (id.startsWith(QLatin1String("pinhole."))) {
        err(QStringLiteral("pinhole: pinhole. ids are Pinhole's own plugins': give it one of yours (me.%1)").arg(id.mid(8)));
        return 2;
    }
    const QString dir = folder.isEmpty() ? Plugins::personalDir() + '/' + id : QFileInfo(folder).absoluteFilePath();
    if (QFileInfo::exists(dir)) {
        err(QStringLiteral("pinhole: %1 is there already").arg(tilde(dir)));
        return 1;
    }
    // "me.favourite-count" is named "Favourite count" to begin with.
    QString name = id.section('.', -1).replace('-', ' ').replace('_', ' ');
    name[0] = name[0].toUpper();
    const QJsonObject manifest{
        {"api", Plugins::api},
        {"id", id},
        {"name", name},
        {"version", "0.1.0"},
        {"description", "What it does, in a sentence."},
    };
    // Its key: an F key no plugin here names, so two new plugins do not
    // share one (two shortcuts on one key: neither works).
    QString key = QStringLiteral("F2");
    {
        QSet<QString> used;
        static const QRegularExpression named(QStringLiteral("[\"'](F(?:[2-9]|1[0-2]))[\"']"));
        for (const QString &root : {Plugins::builtInDir(), Plugins::personalDir()}) {
            QDirIterator files(root, {"*.qml"}, QDir::Files, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                QFile file(files.next());
                if (file.open(QIODevice::ReadOnly))
                    for (auto m = named.globalMatch(QString::fromUtf8(file.readAll())); m.hasNext();)
                        used.insert(m.next().captured(1));
            }
        }
        for (int n = 2; n <= 12; ++n)
            if (!used.contains(QStringLiteral("F%1").arg(n))) {
                key = QStringLiteral("F%1").arg(n);
                break;
            }
    }
    // A view of its own over the photos, as most plugins have: on its key,
    // the newest photos as the kit's tiles; ⏎ opens one, and Esc there
    // comes back to it. The kit's ways (PluginView, PhotoTiles), photos by
    // id, every word in qsTr() and in Polish: what `pinhole plugin check`
    // asks of a plugin it has.
    const QByteArray main = QStringLiteral(R"(import QtQuick
import Pinhole.Plugin

// %1: what it adds to Pinhole. docs/plugins.md has all a plugin is given,
// docs/plugin-kit.md every part of the kit; saving this file loads it
// again at once.
PinholePlugin {
    id: plugin
    actions: [
        PluginAction {
            place: "grid"
            text: qsTr("Newest")
            key: "F2"
            shown: pinhole.library.count > 0
            onTriggered: newest.open()
        }
    ]
    help: [["F2", qsTr("The newest photos")]]

    // Over the photos while open: made the first time, Esc and F2 close it.
    PluginView {
        id: newest
        toggleKey: "F2"
        content: Component {
            Item {
                id: page
                // Photos by id, newest first: ids stay the same photo when
                // the library is read again; positions do not.
                readonly property var photos: (pinhole.library.revision, pinhole.library.field("id").slice(-500).reverse())
                Panel {}
                PanelHeader { id: header; title: qsTr("Newest") }
                PhotoTiles {
                    id: tiles
                    anchors.top: header.bottom
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 16
                    library: pinhole.library
                    photos: page.photos
                    active: newest.shown && !pinhole.away
                    // Esc on the photo comes back here, on it.
                    onOpened: (photo, index) => pinhole.openPhotos(page.photos, photo, { comeBack: true })
                }
                Connections {
                    target: pinhole
                    function onReturned(photo) { tiles.moveTo(photo) }
                }
            }
        }
    }
}
)").arg(name, id).replace(QStringLiteral("\"F2\""), QStringLiteral("\"%1\"").arg(key)).toUtf8();
    // Its words in Polish, as Pinhole's own have theirs.
    const QByteArray polish = QJsonDocument(QJsonObject{{"Newest", QString::fromUtf8("Najnowsze")},
                                                        {"The newest photos", QString::fromUtf8("Najnowsze zdjęcia")},
                                                        {"What it does, in a sentence.", QString::fromUtf8("Co robi, w jednym zdaniu.")}})
                                  .toJson();
    QDir().mkpath(dir + "/i18n");
    QSaveFile manifestFile(dir + "/manifest.json"), mainFile(dir + "/Main.qml"), polishFile(dir + "/i18n/pl.json");
    if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(QJsonDocument(manifest).toJson()) < 0 || !manifestFile.commit()
        || !mainFile.open(QIODevice::WriteOnly) || mainFile.write(main) < 0 || !mainFile.commit()
        || !polishFile.open(QIODevice::WriteOnly) || polishFile.write(polish) < 0 || !polishFile.commit()) {
        err(QStringLiteral("pinhole: could not write %1").arg(tilde(dir)));
        return 1;
    }
    if (folder.isEmpty()) {
        out(QStringLiteral("Made %1. Turn it on in the side panel (Tab), then change Main.qml: it loads again as you save it.")
                .arg(tilde(dir)));
        out(QStringLiteral("pinhole plugin check %1 looks it over.").arg(id));
    } else {
        out(QStringLiteral("Made %1. Pinhole loads it from ~/.config/pinhole/plugins/%2 once it is there.").arg(tilde(dir), id));
        out(QStringLiteral("pinhole plugin check %1 looks it over.").arg(tilde(dir)));
    }
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
    Plugins::addModules(&engine);
    QQmlComponent component(&engine, QUrl::fromLocalFile(dir + '/' + main), QQmlComponent::PreferSynchronous);
    for (const QQmlError &error : component.errors())
        problems << Plugins::describe(error, dir);
    // And every other: one a Loader loads by its name is not among what
    // the main file uses, until the window loads it.
    const bool builtinId = manifest.value("id").toString().startsWith(QLatin1String("pinhole."));
    QSet<QString> said;
    for (const QQmlError &error : component.errors())
        said << error.url().toLocalFile();
    QDirIterator files(dir, {"*.qml"}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = QDir::cleanPath(files.next());
        const QString file = QDir(dir).relativeFilePath(path);
        if (file == main || said.contains(path) || file.startsWith(QLatin1String("check/"))
            || file.startsWith(QLatin1String("tests/")) || file.contains(QLatin1String("target/"))
            || (builtinId && file.startsWith(QLatin1String("module/"))))
            continue;
        QQmlComponent each(&engine, QUrl::fromLocalFile(path), QQmlComponent::PreferSynchronous);
        for (const QQmlError &error : each.errors())
            if (!said.contains(error.url().toLocalFile() + ':' + QString::number(error.line()))) {
                said << error.url().toLocalFile() + ':' + QString::number(error.line());
                problems << Plugins::describe(error, dir);
            }
    }
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
    // QML that does not compile is said, and what the rest of the check
    // finds with it, so one run says all it can.
    // The footguns: refused, or warned of.
    Plugins::Findings findings = Plugins::footguns(dir);
    // Its manifest's words, errors: a key never read is something it meant
    // and does not have.
    problems << Plugins::manifestWarnings(dir, manifest);
    problems << findings.errors;
    if (!problems.isEmpty()) {
        for (const QString &problem : problems)
            err(QStringLiteral("%1: %2").arg(name, problem));
        for (const QString &warning : findings.warnings)
            err(QStringLiteral("%1: warning: %2").arg(name, warning));
        return 1;
    }
    out(QStringLiteral("%1 (plugin API %2): looks fine.").arg(name).arg(Plugins::api));
    for (const QString &warning : findings.warnings)
        err(QStringLiteral("%1: warning: %2").arg(name, warning));
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
    if (command == QLatin1String("docs") && argc <= 4) {
        QCoreApplication app(argc, argv);
        const QString name = argc == 4 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("plugin-quickstart");
        if (name == QLatin1String("--json")) {
            QJsonArray documents;
            for (const QString &id : PluginGuide::documents())
                documents.append(QJsonObject{{"id", id}, {"markdown", PluginGuide::document(id)}});
            const auto json = QJsonDocument(QJsonObject{{"api", Plugins::api}, {"documents", documents}}).toJson();
            std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
            return 0;
        }
        if (!PluginGuide::documents().contains(name)) {
            std::fprintf(stderr, "Unknown document. Choose: %s, or --json\n", qPrintable(PluginGuide::documents().join(", ")));
            return 2;
        }
        const auto text = PluginGuide::document(name).toUtf8();
        std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
        return text.isEmpty() ? 1 : 0;
    }
    if (command == QLatin1String("new") && (argc == 4 || argc == 5)) {
        QCoreApplication app(argc, argv);
        return newPlugin(QString::fromLocal8Bit(argv[3]), argc == 5 ? QString::fromLocal8Bit(argv[4]) : QString());
    }
    if (command == QLatin1String("check") && argc <= 4) {
        // A window system is not needed to compile QML.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QPA_PLATFORMTHEME", "generic");
        QGuiApplication app(argc, argv);
        app.setApplicationName("pinhole");
        Plugins::registerTypes();
        return checkPlugin(argc == 4 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("."));
    }
    std::fputs(kPluginUsage, command.isEmpty() || command == QLatin1String("help") ? stdout : stderr);
    return command.isEmpty() || command == QLatin1String("help") ? 0 : 2;
}
