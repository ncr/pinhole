#include "plugins.h"
#include "pluginquery.h"
#include "library.h"
#include "photoprovider.h"
#include "storage.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJSEngine>

#include <algorithm>
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

QJSValue PluginHost::query(const QString &sql, const QVariantList &values) {
    const int generation = m_generation;
    const auto control = std::make_shared<PluginQuery::Control>();
    auto deferred = std::make_shared<Deferred>();
    const std::weak_ptr<Deferred> weak = deferred;
    const QPointer<PluginHost> alive(this);
    *deferred = defer([alive, control, weak, generation] {
        control->cancel();
        if (alive)
            if (const auto d = weak.lock())
                alive->settle(generation, d->reject, alive->error(QStringLiteral("cancelled"), tr("cancelled")));
    });
    const auto fail = [&](const QString &code, const QString &message) {
        settle(generation, deferred->reject, error(code, message));
        return deferred->promise;
    };
    const QString dir = m_plugins->library() ? m_plugins->library()->dir() : QString();
    const QString path = dir + QStringLiteral("/library.sqlite");
    if (dir.isEmpty() || !QFileInfo::exists(path))
        return fail(QStringLiteral("no_library"), tr("no library is shown"));
    std::erase_if(m_queries, [](const auto &weak) {
        const auto task = weak.lock();
        return !task || task->done() || task->cancelled();
    });
    if (m_queries.size() >= 2)
        return fail(QStringLiteral("query_busy"), tr("Too many queries are running. Wait for one to finish."));
    m_queries.emplace_back(control);
    auto *watcher = new QFutureWatcher<PluginQuery::Answer>(this);
    // A stopped/reloaded plugin must stop computation too, not merely drop
    // its eventual callback. A catalog reload invalidates old answers.
    track(watcher, [control] { control->cancel(); });
    connect(m_plugins->library(), &PluginLibrary::changed, watcher, [control] { control->cancel(); });
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, deferred, generation, control] {
        watcher->deleteLater();
        auto answer = watcher->result();
        if (control->cancelled()) answer = {{}, QStringLiteral("cancelled"), {}};
        if (!answer.code.isEmpty()) {
            QString message = answer.message;
            if (answer.code == QLatin1String("query_timeout"))
                message = tr("The query took too long. Narrow it down and try again.");
            else if (answer.code == QLatin1String("query_limit"))
                message = tr("The query exceeds Pinhole's limits. Use fewer rows or smaller values.");
            else if (answer.code == QLatin1String("query_busy"))
                message = tr("Too many queries are running. Wait for one to finish.");
            else if (answer.code == QLatin1String("cancelled"))
                message = tr("cancelled");
            settle(generation, deferred->reject, error(answer.code, message));
        } else if (QJSEngine *js = engine()) {
            settle(generation, deferred->resolve, js->toScriptValue(answer.rows));
        }
    });
    watcher->setFuture(PluginQuery::start(path, sql, values, control));
    return deferred->promise;
}

