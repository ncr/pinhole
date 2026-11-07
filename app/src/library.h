#pragma once

#include "adjustment.h"

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
    // The thumbnail's size as the photo is drawn: turned and cut as its
    // adjustment says; rawW and rawH, as its file is.
    quint16 thumbW = 0;
    quint16 thumbH = 0;
    quint16 rawW = 0;
    quint16 rawH = 0;
    Adjustment adjust;
    // What its source has done to it (iCloud's edits, as the index's
    // `edit` says): its thumbnail and renditions are of it so already.
    Adjustment sourceEdit;
    float duration = 0;
    qint64 date = 0;
    qint64 added = 0; // source's added date (ms); 0 when unknown, never index/import time
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
    // Its index says what kind of picture each is (schema 5's subtype).
    bool hasSubtype() const { return m_hasSubtype; }
    // A favourite set from the window, before the index has it
    // (favourites.h).
    void setFavorite(int i, bool favorite) { m_items[size_t(i)].favorite = favorite; }
    // What the person did to its photos (adjustment.h): each one's
    // adjustment, and its size drawn so. Those not named, none.
    void applyAdjustments(const Adjustments &all);
    // Take another library's items (a fresh read of the same index). Only
    // while nothing else reads this one: see ThumbnailLoader::reload.
    void swap(Library &other) {
        m_items.swap(other.m_items);
        m_dir.swap(other.m_dir);
        m_source.swap(other.m_source);
        std::swap(m_hasSubtype, other.m_hasSubtype);
        ++m_generation;
        ++other.m_generation;
    }
    // Which read of it this is: another after each swap, so what holds its
    // positions knows they are of an older one.
    quint64 generation() const { return m_generation; }

private:
    QString m_dir;
    QString m_source;
    bool m_hasSubtype = false;
    std::vector<LibraryItem> m_items;
    quint64 m_generation = 0;
};
