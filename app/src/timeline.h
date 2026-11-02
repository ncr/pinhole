#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <vector>

class Library;

// Presentation order only. The library stays in capture order: calendar,
// series and plugin positions must not change when someone sorts the grid.
class Timeline : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
public:
    explicit Timeline(const QString &path = {}, QObject *parent = nullptr,
                      std::function<qint64()> clock = {});
    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    void setLibrary(const Library *library);
    // After a library swap, before grid/viewer remap their positions. No
    // orderChanged signal: their normal reload does that work once.
    void libraryReloaded();
    void refresh();
    const std::vector<int> &order() const { return m_order; }
    int positionOf(int index) const;
    qint64 dateOf(int index) const;
    bool recentlyAdded(int index) const;
    static constexpr qint64 recentWindow = 30LL * 24 * 60 * 60 * 1000;

signals:
    void modeChanged();
    void orderChanged();

private:
    void rebuild();
    const Library *m_library = nullptr;
    QString m_path, m_mode;
    std::function<qint64()> m_clock;
    QTimer m_timer;
    qint64 m_now = 0, m_nextChange = 0;
    std::vector<int> m_order, m_position;
    std::vector<char> m_recent;
};
