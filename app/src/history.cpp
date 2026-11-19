#include "history.h"

#include <algorithm>

namespace {
// Far more than anyone takes back, and little to keep.
constexpr int Most = 200;
}

quint64 History::did(const QString &owner, const QString &name, Step undo, Step redo) {
    if (m_replaying)
        return 0;
    m_steps.erase(m_steps.begin() + m_done, m_steps.end());
    m_steps.push_back({owner, name, std::move(undo), std::move(redo), ++m_numbered});
    if (int(m_steps.size()) > Most)
        m_steps.erase(m_steps.begin());
    m_done = int(m_steps.size());
    emit changed();
    emit done(name);
    return m_numbered;
}

void History::drop(const QString &owner, quint64 step) {
    for (int i = 0; i < int(m_steps.size()); ++i) {
        if (m_steps[i].number != step || m_steps[i].owner != owner || step == 0)
            continue;
        const QString name = m_steps[i].name;
        m_steps.erase(m_steps.begin() + i);
        if (i < m_done)
            --m_done;
        emit changed();
        emit dropped(name);
        return;
    }
}

void History::undo() {
    if (!canUndo()) {
        emit nothing(true);
        return;
    }
    const Entry &entry = m_steps[--m_done];
    m_replaying = true;
    entry.undo();
    m_replaying = false;
    emit changed();
    emit undone(entry.name);
}

void History::redo() {
    if (!canRedo()) {
        emit nothing(false);
        return;
    }
    const Entry &entry = m_steps[m_done++];
    m_replaying = true;
    entry.redo();
    m_replaying = false;
    emit changed();
    emit redone(entry.name);
}

void History::forget(const QString &owner) {
    // Only its own: each step puts back what one owner keeps, so the
    // others' still undo as they were. Taken out where they are: the
    // others' steps stay whole when the owner has none.
    int done = m_done;
    for (int i = 0; i < m_done; ++i)
        if (m_steps[i].owner == owner)
            --done;
    const auto gone = std::remove_if(m_steps.begin(), m_steps.end(), [&](const Entry &e) { return e.owner == owner; });
    if (gone == m_steps.end())
        return;
    m_steps.erase(gone, m_steps.end());
    m_done = done;
    emit changed();
}

void History::clear() {
    if (m_steps.empty())
        return;
    m_steps.clear();
    m_done = 0;
    emit changed();
}
