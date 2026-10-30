#include "history.h"

namespace {
// Far more than anyone takes back, and little to keep.
constexpr int Most = 200;
}

void History::did(const QString &owner, const QString &name, Step undo, Step redo) {
    if (m_replaying)
        return;
    m_steps.erase(m_steps.begin() + m_done, m_steps.end());
    m_steps.push_back({owner, name, std::move(undo), std::move(redo)});
    if (int(m_steps.size()) > Most)
        m_steps.erase(m_steps.begin());
    m_done = int(m_steps.size());
    emit changed();
    emit done(name);
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
    // others' still undo as they were.
    std::vector<Entry> kept;
    int done = 0;
    for (int i = 0; i < int(m_steps.size()); ++i) {
        if (m_steps[i].owner == owner)
            continue;
        if (i < m_done)
            ++done;
        kept.push_back(std::move(m_steps[i]));
    }
    if (kept.size() == m_steps.size())
        return;
    m_steps = std::move(kept);
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
