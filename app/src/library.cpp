#include "library.h"

#include <QDir>

#include <algorithm>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

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
        // Older/synthetic libraries may lack either optional column.
        const QSqlRecord schema = db.record("assets");
        const QString effectColumn = schema.indexOf("live_effect") >= 0 ? "live_effect" : "NULL";
        const QString addedColumn = schema.indexOf("added_date") >= 0 ? "added_date" : "NULL";
        const QString editColumn = schema.indexOf("edit") >= 0 ? "edit" : "NULL";
        m_hasSubtype = schema.indexOf("subtype") >= 0;
        const QString columns = QStringLiteral("thumb_path, avg_color, thumb_w, thumb_h, duration, asset_date, is_video, is_live, "
                                               "favorite, id, filename, %1, %2, %3 FROM assets ORDER BY asset_date, id");
        if (!q.exec(QStringLiteral("SELECT ") + columns.arg(effectColumn, addedColumn, editColumn))) {
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
            item.thumbW = item.rawW = quint16(q.value(2).toUInt());
            item.thumbH = item.rawH = quint16(q.value(3).toUInt());
            item.duration = q.value(4).toFloat();
            item.date = q.value(5).toLongLong();
            item.video = q.value(6).toBool();
            item.live = q.value(7).toBool();
            item.favorite = q.value(8).toBool();
            item.id = q.value(9).toString().toUtf8();
            item.filename = q.value(10).toString();
            item.added = q.value(12).toLongLong();
            if (!q.value(13).isNull())
                item.sourceEdit = Adjustment::fromQuery(q.value(13).toString());
            const QString effect = q.value(11).toString();
            item.effect = effect == QLatin1String("loop")            ? LiveEffect::Loop
                        : effect == QLatin1String("bounce")          ? LiveEffect::Bounce
                        : effect == QLatin1String("long_exposure")   ? LiveEffect::LongExposure
                                                                     : LiveEffect::None;
            m_items.push_back(std::move(item));
        }
        QSqlQuery meta(db);
        m_source = meta.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'source'")) && meta.next()
                       ? meta.value(0).toString()
                       : QString();
    }
    QSqlDatabase::removeDatabase("library");
    return true;
}

std::vector<int> Library::applyAdjustments(const Adjustments &all, const Adjustments &pending) {
    std::vector<int> changed;
    for (size_t i = 0; i < m_items.size(); ++i) {
        LibraryItem &item = m_items[i];
        Adjustment adjust = all.value(item.id);
        if (!pending.isEmpty())
            if (const auto it = pending.constFind(item.id); it != pending.cend()) {
                const int quarters = Adjustment::quartersBetween(item.sourceEdit, it.value());
                adjust = Adjustment();
                adjust.turn = std::max(0, quarters);
            }
        // Read with none, its size is its file's already.
        if (adjust == item.adjust)
            continue;
        changed.push_back(int(i));
        item.adjust = adjust;
        if (item.adjust.isIdentity() || !item.rawW || !item.rawH) {
            item.thumbW = item.rawW;
            item.thumbH = item.rawH;
            continue;
        }
        const QSize size = item.adjust.applied(QSize(item.rawW, item.rawH));
        item.thumbW = quint16(std::min(size.width(), 65535));
        item.thumbH = quint16(std::min(size.height(), 65535));
    }
    return changed;
}
