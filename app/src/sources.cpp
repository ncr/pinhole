#include "sources.h"

#include "serviceclient.h"

#include <QJsonArray>

Sources::Sources(ServiceClient *service, QObject *parent) : QObject(parent), m_service(service) {
    connect(m_service, &ServiceClient::announced, this, [this](const QString &name, const QJsonObject &details) {
        if (name == QLatin1String("sources")) {
            take(details);
        } else if (name == QLatin1String("kinds")) {
            const QVariantList kinds = details.value("kinds").toArray().toVariantList();
            if (kinds != m_kinds) {
                m_kinds = kinds;
                emit kindsChanged();
            }
        } else if (name == QLatin1String("progress")) {
            QVariantMap progress = details.toVariantMap();
            progress.remove(QStringLiteral("event"));
            if (progress.value("phase").toString() == QLatin1String("done"))
                progress.clear();
            if (progress != m_progress) {
                m_progress = progress;
                emit progressChanged();
            }
        } else if (name == QLatin1String("health") && details.value("state").toString() == QLatin1String("no_service")) {
            // No service, no rounds: what it said last is no longer so.
            if (!m_rounds.isEmpty()) {
                m_rounds.clear();
                emit roundsChanged();
            }
        } else if (name == QLatin1String("rounds")) {
            QVariantMap rounds;
            for (const QString &key : {QStringLiteral("source"), QStringLiteral("last"), QStringLiteral("next"), QStringLiteral("push")})
                if (!details.value(key).isNull())
                    rounds.insert(key, details.value(key).toVariant());
            if (rounds != m_rounds) {
                m_rounds = rounds;
                emit roundsChanged();
            }
        }
    });
}

int Sources::currentIndex() const {
    for (int i = 0; i < m_list.size(); ++i)
        if (m_list[i].toMap().value("id").toString() == m_current)
            return i;
    return -1;
}

QVariantMap Sources::entry(const QString &id) const {
    for (const QVariant &source : m_list)
        if (source.toMap().value("id").toString() == id)
            return source.toMap();
    return {};
}

void Sources::take(const QJsonObject &sources) {
    m_list = sources.value("sources").toArray().toVariantList();
    m_current = sources.value("current").toString();
    m_known = true;
    emit listChanged();
    if (!m_switching.isEmpty() && m_current == m_switching) {
        m_switching.clear();
        const int at = currentIndex();
        if (at >= 0)
            emit switched(m_list[at].toMap().value("name").toString());
    }
}

void Sources::announce(const QString &id) {
    m_switching = id;
    if (!id.isEmpty() && m_current == id) {
        m_switching.clear();
        const int at = currentIndex();
        if (at >= 0)
            emit switched(m_list[at].toMap().value("name").toString());
    }
}

void Sources::show(const QString &id) {
    if (id.isEmpty() || id == m_current)
        return;
    // Said at once, as the key is pressed: its photos follow within a
    // second or so.
    for (const QVariant &source : std::as_const(m_list))
        if (source.toMap().value("id").toString() == id)
            emit switched(source.toMap().value("name").toString());
    setError({});
    m_service->call(QStringLiteral("set_source"), {{"id", id}}, [this](bool ok, const QJsonObject &, const QString &code) {
        if (!ok)
            setError(code);
    });
}

bool Sources::showAt(int index) {
    if (index < 0 || index >= m_list.size())
        return false;
    const QString id = m_list[index].toMap().value("id").toString();
    if (id == m_current) {
        // Said again: which one this is.
        emit switched(m_list[index].toMap().value("name").toString());
        return true;
    }
    show(id);
    return true;
}

void Sources::remove(const QString &id) {
    setError({});
    m_service->call(QStringLiteral("remove_source"), {{"id", id}},
                    [this](bool ok, const QJsonObject &result, const QString &code) {
                        if (ok)
                            take(result);
                        else
                            setError(code);
                    });
}

void Sources::discover() {
    m_service->call(QStringLiteral("discover"), {}, [this](bool ok, const QJsonObject &result, const QString &) {
        if (!ok)
            return;
        m_discovered = result.value("kinds").toObject().toVariantMap();
        emit discoveredChanged();
    });
}

void Sources::add(const QString &kind, const QJsonObject &params, Answer done) {
    QJsonObject asked = params;
    asked.insert("kind", kind);
    asked.remove("id");
    m_switching.clear();
    setError({});
    m_service->call(QStringLiteral("set_source"), asked, [this, done](bool ok, const QJsonObject &result, const QString &code) {
        if (ok)
            announce(result.value("id").toString());
        else
            setError(code);
        done(ok, result, code, ok ? QString() : result.value("message").toString());
    });
}

void Sources::signIn(const QString &kind, const QJsonObject &params, Answer done) {
    QJsonObject asked = params;
    asked.insert("kind", kind);
    m_service->call(QStringLiteral("sign_in"), asked, [this, done](bool ok, const QJsonObject &result, const QString &code) {
        // Signed in, it is shown: the service said so before it answered.
        if (ok && result.value("state").toString() == QLatin1String("signed_in"))
            announce(m_current);
        done(ok, result, code, ok ? QString() : result.value("message").toString());
    });
}

void Sources::submitCode(const QString &kind, const QJsonObject &params, Answer done) {
    QJsonObject asked = params;
    asked.insert("kind", kind);
    m_service->call(QStringLiteral("submit_code"), asked, [this, done](bool ok, const QJsonObject &result, const QString &code) {
        if (ok && result.value("state").toString() == QLatin1String("signed_in"))
            announce(m_current);
        done(ok, result, code, ok ? QString() : result.value("message").toString());
    });
}

void Sources::signOut(const QString &id, Answer done) {
    setError({});
    emit signedOut(id);
    m_service->call(QStringLiteral("sign_out"), {{"id", id}}, [this, done](bool ok, const QJsonObject &result, const QString &code) {
        if (ok)
            take(result);
        else
            setError(code);
        done(ok, result, code, ok ? QString() : result.value("message").toString());
    });
}

void Sources::setError(const QString &code) {
    const QString error = code.isEmpty() ? QString()
                          : code == QLatin1String("bad_request") ? tr("Photos cannot be shown from there.")
                          : code == QLatin1String("read_only")
                              ? tr("This library is only being looked at here: its sources are changed in a window of your own library.")
                          : code == QLatin1String("turned_off") ? tr("Its plugin is turned off.")
                              : tr("The Pinhole service is not answering.");
    if (error != m_error) {
        m_error = error;
        emit errorChanged();
    }
}
