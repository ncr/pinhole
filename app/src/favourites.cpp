#include "favourites.h"

#include "history.h"
#include "serviceclient.h"

#include <QJsonArray>
#include <QSet>

Favourites::Favourites(Library *library, ServiceClient *service, History *history, QObject *parent)
    : QObject(parent), m_library(library), m_service(service), m_history(history) {}

bool Favourites::all(const QList<int> &indexes) const {
    bool any = false;
    for (int i : indexes) {
        if (i < 0 || i >= m_library->count())
            continue;
        if (!m_library->at(i).favorite)
            return false;
        any = true;
    }
    return any;
}

void Favourites::toggle(const QList<int> &indexes) {
    const bool favorite = !all(indexes);
    // Only those not so already; by id, as a reload may move them before
    // the service answers.
    QList<QByteArray> ids;
    for (int i : indexes) {
        if (i < 0 || i >= m_library->count() || m_library->at(i).favorite == favorite)
            continue;
        ids.append(m_library->at(i).id);
    }
    if (ids.isEmpty())
        return;
    const QString source = m_library->source();
    set(ids, favorite, source);
    // Undone, those it changed go back; the rest were so already.
    m_history->did({}, favorite ? tr("adding to favourites") : tr("removing from favourites"),
                   [this, ids, favorite, source] { set(ids, !favorite, source); },
                   [this, ids, favorite, source] { set(ids, favorite, source); });
}

void Favourites::set(const QList<QByteArray> &ids, bool favorite, const QString &source) {
    // Another's shown since: not these photos.
    if (m_library->source() != source)
        return;
    QJsonArray assets;
    for (const QByteArray &id : ids)
        assets.append(QString::fromUtf8(id));
    apply(ids, favorite);
    QJsonObject params{{"assets", assets}, {"favorite", favorite}};
    // And the service, which may have another source's library already.
    if (!source.isEmpty())
        params.insert("source", source);
    m_service->call(QStringLiteral("set_favorite"), params, [this, ids, favorite, source](bool ok, const QJsonObject &, const QString &code) {
        if (ok)
            return;
        // Put back as they were, if they are still the ones shown.
        if (m_library->source() == source)
            apply(ids, !favorite);
        const QString source = m_service->sourceName();
        QString message;
        if (code == QLatin1String("needs_sign_in"))
            message = tr("Not signed in to %1").arg(source);
        else if (code == QLatin1String("no_service") || m_service->health() == QLatin1String("no_service"))
            message = tr("The Pinhole service is not running");
        else
            message = tr("Could not change favourites in %1").arg(source);
        emit failed(message);
    });
}

void Favourites::apply(const QList<QByteArray> &ids, bool favorite) {
    const QSet<QByteArray> wanted(ids.begin(), ids.end());
    for (int i = 0; i < m_library->count(); ++i)
        if (wanted.contains(m_library->at(i).id))
            m_library->setFavorite(i, favorite);
    ++m_revision;
    emit changed();
}

void Favourites::reloaded() {
    ++m_revision;
    emit changed();
}
