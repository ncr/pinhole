#pragma once

#include <QByteArray>
#include <QString>
#include <vector>

// One asset as the grid sees it. Kept small: the whole library sits in
// memory, oldest first, and the grid addresses it by position.
struct LibraryItem {
    QByteArray id;        // the asset record name, as pinholed knows it
    QByteArray thumbPath; // absolute, ready for fopen
    quint32 avgColor = 0x303030;
    quint16 thumbW = 0;
    quint16 thumbH = 0;
    float duration = 0;
    qint64 date = 0;
    bool video = false;
    bool live = false;
    bool favorite = false;
};

// The library index the core writes (library.sqlite), read once at start.
class Library {
public:
    bool open(const QString &dir, QString *error);
    int count() const { return int(m_items.size()); }
    const LibraryItem &at(int i) const { return m_items[size_t(i)]; }
    QString dir() const { return m_dir; }

private:
    QString m_dir;
    std::vector<LibraryItem> m_items;
};
