#include "timeline.h"
#include "library.h"

#include <QDateTime>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <limits>
#include <numeric>

Timeline::Timeline(const QString &path, QObject *parent, std::function<qint64()> clock)
    : QObject(parent), m_path(path.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/window.conf" : path),
      m_clock(clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); }) {
    QSettings settings(m_path, QSettings::IniFormat);
    m_mode = settings.value("grid/order", "smart").toString();
    if (m_mode != "smart" && m_mode != "taken" && m_mode != "added")
        m_mode = "smart";
    // Expire recent photos even if the library gets no new changes, and
    // after waking from sleep. No sorting until an actual boundary passes.
    m_timer.setInterval(60000);
    connect(&m_timer, &QTimer::timeout, this, &Timeline::refresh);
    m_timer.start();
}

void Timeline::setMode(const QString &mode) {
    if ((mode != "smart" && mode != "taken" && mode != "added") || mode == m_mode)
        return;
    m_mode = mode;
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setValue("grid/order", mode);
    settings.sync();
    rebuild();
    emit modeChanged();
    emit orderChanged();
}

void Timeline::setLibrary(const Library *library) { m_library = library; rebuild(); }
void Timeline::libraryReloaded() { rebuild(); }

void Timeline::refresh() {
    const qint64 now = m_clock();
    if (m_library && m_mode == "smart" && (now < m_now || now >= m_nextChange)) {
        rebuild();
        emit orderChanged();
    }
}

void Timeline::rebuild() {
    const int n = m_library ? m_library->count() : 0;
    m_now = m_clock();
    m_nextChange = std::numeric_limits<qint64>::max();
    m_recent.assign(size_t(n), 0);
    m_order.resize(size_t(n));
    std::iota(m_order.begin(), m_order.end(), 0);
    for (int i = 0; i < n; ++i) {
        const qint64 added = m_library->at(i).added;
        const bool recent = added > 0 && added <= m_now && added >= m_now - recentWindow;
        m_recent[size_t(i)] = recent;
        if (recent)
            m_nextChange = std::min(m_nextChange, added + recentWindow + 1);
        else if (added > m_now)
            m_nextChange = std::min(m_nextChange, added);
    }
    const auto byDate = [this](auto date) {
        return [this, date](int a, int b) {
            const qint64 ad = date(m_library->at(a)), bd = date(m_library->at(b));
            return ad != bd ? ad < bd : a < b;
        };
    };
    if (m_mode == "smart") {
        const auto recent = std::stable_partition(m_order.begin(), m_order.end(), [this](int i) {
            return !m_recent[size_t(i)];
        });
        // The index already reads in capture order. Check it after date
        // conversion as older databases can contain missing or mixed types.
        const auto captureOrder = byDate([](const LibraryItem &item) { return item.date; });
        if (!std::is_sorted(m_order.begin(), recent, captureOrder))
            std::sort(m_order.begin(), recent, captureOrder);
        std::sort(recent, m_order.end(), byDate([](const LibraryItem &item) { return item.added; }));
    } else if (m_mode == "added") {
        std::sort(m_order.begin(), m_order.end(), byDate([](const LibraryItem &item) {
            return item.added > 0 ? item.added : item.date;
        }));
    }
    m_position.resize(size_t(n));
    for (int place = 0; place < n; ++place)
        m_position[size_t(m_order[size_t(place)])] = place;
}

int Timeline::positionOf(int index) const {
    return index >= 0 && index < int(m_position.size()) ? m_position[size_t(index)] : -1;
}

bool Timeline::recentlyAdded(int index) const {
    return m_mode == "smart" && index >= 0 && index < int(m_recent.size()) && m_recent[size_t(index)];
}

qint64 Timeline::dateOf(int index) const {
    if (!m_library || index < 0 || index >= m_library->count())
        return 0;
    const auto &item = m_library->at(index);
    return (m_mode == "added" || recentlyAdded(index)) && item.added > 0 ? item.added : item.date;
}
