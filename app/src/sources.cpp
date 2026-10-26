#include "sources.h"

#include "portal.h"
#include "serviceclient.h"

#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>

Sources::Sources(ServiceClient *service, QObject *parent) : QObject(parent), m_service(service) {
    connect(m_service, &ServiceClient::announced, this, [this](const QString &name, const QJsonObject &details) {
        if (name == QLatin1String("sources")) {
            take(details);
        } else if (name == QLatin1String("progress")) {
            QVariantMap progress = details.toVariantMap();
            progress.remove(QStringLiteral("event"));
            if (progress.value("phase").toString() == QLatin1String("done"))
                progress.clear();
            if (progress != m_progress) {
                m_progress = progress;
                emit progressChanged();
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

void Sources::addFolder(const QString &path) {
    const QString folder = QDir::cleanPath(path);
    m_switching = QStringLiteral("folder:") + folder;
    setError({});
    m_service->call(QStringLiteral("set_source"), {{"kind", "folder"}, {"path", folder}},
                    [this](bool ok, const QJsonObject &, const QString &code) {
                        if (!ok) {
                            m_switching.clear();
                            setError(code);
                        }
                    });
}

void Sources::chooseFolder() {
    auto *picker = new PortalFolderPicker(this);
    connect(picker, &PortalFolderPicker::picked, this, [this, picker](const QString &folder) {
        picker->deleteLater();
        if (!folder.isEmpty())
            addFolder(folder);
    });
    picker->pick(tr("Show photos from"));
}

void Sources::discover() {
    m_service->call(QStringLiteral("discover"), {}, [this](bool ok, const QJsonObject &result, const QString &) {
        if (!ok)
            return;
        QStringList icloudpd;
        for (const QJsonValue &stem : result.value("icloudpd").toArray())
            icloudpd.append(stem.toString());
        m_icloudpd = icloudpd;
        m_folders = result.value("folders").toArray().toVariantList();
        emit discoveredChanged();
    });
}

bool Sources::icloudpdHas(const QString &appleId) const {
    static const QRegularExpression notWord(QStringLiteral("[^A-Za-z0-9_]"));
    QString stem = appleId.trimmed();
    stem.remove(notWord);
    return !stem.isEmpty() && appleId.contains('@') && m_icloudpd.contains(stem);
}

void Sources::signIn(const QString &appleId, const QString &password) {
    // A code asked for again keeps the code boxes up.
    const QString back = m_signInState == QLatin1String("needs_code") ? m_signInState : QString();
    setSignIn(QStringLiteral("busy"));
    QJsonObject params{{"apple_id", appleId.trimmed()}};
    if (!password.isEmpty())
        params.insert("password", password);
    m_switching = QStringLiteral("icloud:") + appleId.trimmed();
    m_service->call(QStringLiteral("sign_in"), params, [this, back](bool ok, const QJsonObject &result, const QString &code) {
        if (m_signInState.isEmpty())
            return; // cancelled meanwhile
        if (!ok) {
            m_switching.clear();
            setSignIn(back, code);
        } else if (result.value("state").toString() == QLatin1String("needs_code")) {
            setSignIn(QStringLiteral("needs_code"));
        } else {
            setSignIn({});
            emit signedIn();
        }
    });
}

void Sources::submitCode(const QString &appleId, const QString &code) {
    setSignIn(QStringLiteral("busy"));
    m_service->call(QStringLiteral("submit_code"), {{"apple_id", appleId.trimmed()}, {"code", code}},
                    [this](bool ok, const QJsonObject &, const QString &code) {
                        if (m_signInState.isEmpty())
                            return;
                        if (!ok) {
                            setSignIn(QStringLiteral("needs_code"), code);
                        } else {
                            setSignIn({});
                            emit signedIn();
                        }
                    });
}

void Sources::cancelSignIn() {
    m_switching.clear();
    setSignIn({});
}

void Sources::setSignIn(const QString &state, const QString &errorCode) {
    m_signInState = state;
    // What each of the provider's errors means for the person signing in.
    if (errorCode.isEmpty())
        m_signInError.clear();
    else if (errorCode == QLatin1String("wrong_password"))
        m_signInError = tr("Apple did not accept that Apple ID or password.");
    else if (errorCode == QLatin1String("wrong_code"))
        m_signInError = tr("That code did not work. The newest one Apple sent is the one to type.");
    else if (errorCode == QLatin1String("rate_limited"))
        m_signInError = tr("Apple is not taking sign-ins for this Apple ID right now. Try once more in half an hour.");
    else if (errorCode == QLatin1String("needs_sign_in"))
        m_signInError = tr("icloudpd's sign-in for this Apple ID has run out. Sign in with the password instead.");
    else if (errorCode == QLatin1String("unavailable"))
        m_signInError = tr("iCloud cannot be reached. Is this computer online?");
    else
        m_signInError = tr("Signing in did not work (%1).").arg(errorCode);
    emit signInChanged();
}

void Sources::setError(const QString &code) {
    const QString error = code.isEmpty() ? QString()
                          : code == QLatin1String("bad_request") ? tr("Photos cannot be shown from there.")
                                                                 : tr("The Pinhole service is not answering.");
    if (error != m_error) {
        m_error = error;
        emit errorChanged();
    }
}
