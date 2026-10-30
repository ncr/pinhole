#pragma once

#include <QByteArray>
#include <QString>
#include <vector>

// A Live Photo's effect as set on the iPhone (the index's live_effect).
enum class LiveEffect : quint8 { None, Loop, Bounce, LongExposure };

// One asset as the grid sees it. Kept small: the whole library sits in
// memory, oldest first, and the grid addresses it by position.
struct LibraryItem {
    QByteArray id;        // the asset record name, as pinholed knows it
    QByteArray thumbPath; // absolute, ready for fopen
    QString filename;     // as it was on the camera: IMG_1234.HEIC
    quint32 avgColor = 0x303030;
    quint16 thumbW = 0;
    quint16 thumbH = 0;
    float duration = 0;
    qint64 date = 0;
    bool video = false;
    bool live = false;
    bool favorite = false;
    LiveEffect effect = LiveEffect::None;
};

// The library index the core writes (library.sqlite), read once at start.
class Library {
public:
    bool open(const QString &dir, QString *error);
    int count() const { return int(m_items.size()); }
    const LibraryItem &at(int i) const { return m_items[size_t(i)]; }
    QString dir() const { return m_dir; }
    // The source whose library it is, as the core wrote into it
    // (sync::claim): "icloud:me@example.com", "folder:/home/me/Pictures";
    // empty for one that says nothing (pinhole-fake's).
    QString source() const { return m_source; }
    // A favourite set from the window, before the index has it
    // (favourites.h).
    void setFavorite(int i, bool favorite) { m_items[size_t(i)].favorite = favorite; }
    // Take another library's items (a fresh read of the same index). Only
    // while nothing else reads this one: see ThumbnailLoader::reload.
    void swap(Library &other) {
        m_items.swap(other.m_items);
        m_dir.swap(other.m_dir);
        m_source.swap(other.m_source);
    }

private:
    QString m_dir;
    QString m_source;
    std::vector<LibraryItem> m_items;
};
