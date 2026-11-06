#pragma once

#include <QFuture>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <memory>

// Queries have their own SQLite connections and bounded worker pool. Never
// pass a handle from Qt's SQLite plugin to the system SQLite library.
namespace PluginQuery {

struct Limits {
    int milliseconds = 2000;
    qsizetype rows = 100000;
    qsizetype bytes = 32 * 1024 * 1024;
    int cellBytes = 1024 * 1024;
    int columns = 64;
    int sqlBytes = 64 * 1024;
    // Pinhole's own asking (screenshots()): a source's own tables too.
    // A plugin reads only the public ones: assets, renditions and meta.
    bool privateTables = false;
};

class Control {
public:
    explicit Control(int milliseconds = Limits{}.milliseconds);
    void cancel() { m_cancelled.store(true); }
    bool cancelled() const { return m_cancelled.load(); }
    bool expired() const;
    bool done() const { return m_done.load(); }
    void finish() { m_done.store(true); }

private:
    std::atomic_bool m_cancelled{false}, m_done{false};
    std::chrono::steady_clock::time_point m_deadline;
};

struct Answer {
    QVariantList rows;
    QString code;
    // SQLite's diagnostic for "sql". Other codes have translated messages
    // at the PluginHost boundary. Failures never carry partial rows.
    QString message;
};

// Synchronous reader for worker use and deterministic tests. Its deadline
// begins with Control creation, so waiting in the pool counts too.
Answer read(const QString &path, const QString &sql, const QVariantList &values,
            const std::shared_ptr<Control> &control, const Limits &limits = {});

// At most two workers and eight admitted jobs, independent of Qt's global
// pool used for photo decoding/copying. Rejected admission is query_busy.
QFuture<Answer> start(const QString &path, const QString &sql, const QVariantList &values,
                      const std::shared_ptr<Control> &control, const Limits &limits = {});
int outstanding();

} // namespace PluginQuery
