#include "library.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

bool Library::open(const QString &dir, QString *error) {
    m_dir = QDir(dir).absolutePath();
    const QString path = m_dir + "/library.sqlite";
    if (!QFile::exists(path)) {
        *error = QStringLiteral("No library at %1").arg(path);
        return false;
    }
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "library");
        db.setDatabaseName(path);
        // The core owns writes; the window only ever reads.
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (!db.open()) {
            *error = db.lastError().text();
            return false;
        }
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec("SELECT thumb_path, avg_color, thumb_w, thumb_h, duration, asset_date, is_video, is_live, favorite "
                    "FROM assets ORDER BY asset_date, id")) {
            *error = q.lastError().text();
            return false;
        }
        const QByteArray base = QFile::encodeName(m_dir) + '/';
        m_items.clear();
        m_items.reserve(110000);
        while (q.next()) {
            LibraryItem item;
            if (!q.value(0).isNull())
                item.thumbPath = base + q.value(0).toString().toUtf8();
            if (!q.value(1).isNull())
                item.avgColor = q.value(1).toUInt();
            item.thumbW = quint16(q.value(2).toUInt());
            item.thumbH = quint16(q.value(3).toUInt());
            item.duration = q.value(4).toFloat();
            item.date = q.value(5).toLongLong();
            item.video = q.value(6).toBool();
            item.live = q.value(7).toBool();
            item.favorite = q.value(8).toBool();
            m_items.push_back(std::move(item));
        }
    }
    QSqlDatabase::removeDatabase("library");
    return true;
}
