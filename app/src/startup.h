#pragma once

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cstdio>
#include <mutex>

// Opt-in, process-relative milestones. No library paths or photo metadata;
// one JSON object per line, so app/bin/measure-startup can compare runs.
class StartupTrace {
public:
    StartupTrace() : m_enabled(qEnvironmentVariableIsSet("PINHOLE_STARTUP_TRACE")) { m_clock.start(); }
    void mark(const QString &event, QJsonObject details = {}) {
        if (!m_enabled) return;
        const double ms = m_clock.nsecsElapsed() / 1e6;
        std::lock_guard lock(m_mutex);
        if (m_seen.contains(event)) return;
        m_seen.insert(event);
        details.insert("event", event);
        details.insert("ms", ms);
        const auto json = QJsonDocument(details).toJson(QJsonDocument::Compact);
        std::fprintf(stderr, "pinhole startup: %s\n", json.constData());
    }
private:
    bool m_enabled;
    QElapsedTimer m_clock;
    std::mutex m_mutex;
    QSet<QString> m_seen;
};
