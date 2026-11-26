#include "sourceedits.h"

#include "library.h"
#include "serviceclient.h"

#include <QJsonArray>
#include <QJsonObject>

namespace {
QJsonValue jsonOf(const Adjustment &a) {
    if (a.isIdentity())
        return QJsonValue();
    return QJsonObject{{"turn", a.turn}, {"crop", QJsonArray{a.crop.x(), a.crop.y(), a.crop.width(), a.crop.height()}}};
}
} // namespace

SourceEdits::SourceEdits(const Library *library, ServiceClient *service, AdjustmentStore *store, QObject *parent)
    : QObject(parent), m_library(library), m_service(service), m_store(store) {
    m_timer.setSingleShot(true);
    m_timer.setInterval(3000);
    connect(&m_timer, &QTimer::timeout, this, &SourceEdits::flush);
}

SourceEdits::~SourceEdits() { *m_alive = false; }

int SourceEdits::positionOf(const QByteArray &id) const {
    for (int i = 0; i < m_library->count(); ++i)
        if (m_library->at(i).id == id)
            return i;
    return -1;
}

Adjustment SourceEdits::sourceEditOf(const QByteArray &id) const {
    const int at = positionOf(id);
    return at >= 0 ? m_library->at(at).sourceEdit : Adjustment();
}

std::optional<Adjustment> SourceEdits::target(const QByteArray &id) const {
    const auto it = m_edits.constFind(id);
    return it == m_edits.cend() ? std::nullopt : std::optional(it->target);
}

bool SourceEdits::sending() const {
    for (const Edit &e : m_edits)
        if (e.sending)
            return true;
    return false;
}

bool SourceEdits::busy() const {
    if (m_timer.isActive())
        return true;
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it)
        if (it->sending || !it->target.near(it->written.value_or(sourceEditOf(it.key()))))
            return true;
    return false;
}

void SourceEdits::hold(const Adjustments &targets, const QString &source, const QString &by) {
    bool now = false;
    Adjustments shown;
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        Edit &e = m_edits[it.key()];
        e.target = it.value().normalized();
        e.source = source;
        e.by = by;
        shown.insert(it.key(), e.target);
        // Cut otherwise than its files: not to be drawn over them, so sent
        // at once rather than shown wrong for a while.
        now = now || Adjustment::quartersBetween(sourceEditOf(it.key()), e.target) < 0;
    }
    if (m_store)
        m_store->setPending(shown);
    if (now)
        QTimer::singleShot(0, this, &SourceEdits::flush);
    else
        m_timer.start();
}

void SourceEdits::flush() {
    m_timer.stop();
    for (const QByteArray &id : m_edits.keys()) {
        Edit &e = m_edits[id];
        // Sent again once this one is answered, if it changed since.
        if (e.sending)
            continue;
        if (e.source != m_library->source() || positionOf(id) < 0) {
            drop(id);
            continue;
        }
        const Adjustment base = e.written.value_or(sourceEditOf(id));
        if (e.target.near(base)) {
            // Back to what the source has: nothing to send.
            if (!e.written)
                drop(id);
            continue;
        }
        send(id);
    }
    checkIdle();
}

void SourceEdits::send(const QByteArray &id) {
    Edit &e = m_edits[id];
    e.sending = e.target;
    const Adjustment target = e.target;
    const Adjustment base = e.written.value_or(sourceEditOf(id));
    const QString source = e.source;
    const std::weak_ptr<bool> alive = m_alive;
    auto answered = [this, alive, id](bool ok, const QJsonObject &result, const QString &code) {
        if (alive.expired())
            return;
        auto it = m_edits.find(id);
        if (it == m_edits.end())
            return;
        Edit &e = *it;
        const Adjustment sent = *e.sending;
        e.sending.reset();
        if (ok) {
            e.written = sent;
            emit wrote(id);
            // Turned again while it went: that one next, once left alone.
            if (!e.target.near(sent) && !m_timer.isActive())
                m_timer.start();
            return checkIdle();
        }
        // A source that keeps no edits (a provider's): kept here instead.
        if (code == QLatin1String("unsupported")) {
            if (m_store)
                m_store->set({{id, e.target}});
            drop(id);
            return;
        }
        emit failed(e.by, result.value("message").toString(code));
        // As the source has it: the last one written, else its own.
        if (e.written) {
            e.target = *e.written;
            if (m_store)
                m_store->setPending({{id, e.target}});
            return checkIdle();
        }
        drop(id);
    };
    m_service->fetch(id, QStringLiteral("original"), [=, this](bool ok, const QJsonObject &result, const QString &code) {
        if (alive.expired())
            return;
        if (!ok)
            return answered(false, result, code);
        QJsonObject params{{"asset", QString::fromUtf8(id)}, {"original", result.value("path").toString()},
                           {"source", source}, {"base", jsonOf(base)}};
        if (!target.isIdentity()) {
            params.insert("turn", target.turn);
            params.insert("crop", QJsonArray{target.crop.x(), target.crop.y(), target.crop.width(), target.crop.height()});
        }
        m_service->call(QStringLiteral("set_edit"), params, answered);
    }, false, source);
}

void SourceEdits::libraryReloaded() {
    for (const QByteArray &id : m_edits.keys()) {
        Edit &e = m_edits[id];
        if (e.sending)
            continue;
        if (e.source != m_library->source() || positionOf(id) < 0) {
            drop(id);
            continue;
        }
        const Adjustment now = sourceEditOf(id);
        // The library has what was written: its files are of it.
        if (e.written && now.near(*e.written))
            e.written.reset();
        if (!e.written && e.target.near(now))
            drop(id);
    }
    checkIdle();
}

void SourceEdits::drop(const QByteArray &id) {
    m_edits.remove(id);
    if (m_store)
        m_store->forgetPending({id});
}

void SourceEdits::checkIdle() {
    if (!busy())
        emit idle();
}
