#include "favourites.h"

#include "serviceclient.h"

#include <QJsonArray>
#include <QSet>

Favourites::Favourites(Library *library, ServiceClient *service, QObject *parent)
    : QObject(parent), m_library(library), m_service(service) {}

QList<QByteArray> Favourites::set(const QList<QByteArray> &ids, bool favorite, const Done &done) {
    // Only those not so already, in the library's order.
    const QSet<QByteArray> wanted(ids.begin(), ids.end());
    QList<QByteArray> changing;
    for (int i = 0; i < m_library->count(); ++i) {
        const LibraryItem &item = m_library->at(i);
        if (item.favorite != favorite && wanted.contains(item.id))
            changing.append(item.id);
    }
    if (changing.isEmpty()) {
        if (done)
            done(QString(), QString());
        return changing;
    }
    apply(changing, favorite);
    // And the service, which may have another source's library already
    // (one that says whose it is not, "", is no other's either): then
    // other_source, and they go back.
    const QString source = m_library->source();
    QJsonArray assets;
    for (const QByteArray &id : std::as_const(changing))
        assets.append(QString::fromUtf8(id));
    const QJsonObject params{{"assets", assets}, {"favorite", favorite}, {"source", source}};
    m_service->call(QStringLiteral("set_favorite"), params,
                    [this, changing, favorite, source, done](bool ok, const QJsonObject &result, const QString &code) {
        // Put back as they were, if they are still the ones shown.
        if (!ok && m_library->source() == source)
            apply(changing, !favorite);
        if (done)
            done(ok ? QString() : code.isEmpty() ? QStringLiteral("unavailable") : code,
                 ok ? QString() : result.value("message").toString());
    });
    return changing;
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
