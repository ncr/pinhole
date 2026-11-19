#include "serviceclient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include "storage.h"

ServiceClient::ServiceClient(const QString &library, QObject *parent)
    : QObject(parent), m_library(sameLibrary(library, Storage::library()) ? QString() : library),
      m_socketPath(socketFor(m_library)) {
    connect(&m_socket, &QLocalSocket::readyRead, this, &ServiceClient::readReplies);
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_attempts = 0;
        m_retry.setInterval(250);
        // Fine until the service says otherwise, which it does at once.
        // What was asked meanwhile goes once it has said its protocol.
        m_greeted = false;
        setHealth(QStringLiteral("fine"));
        m_hello.start();
    });
    // A service that took the line and never says hello is stuck: what
    // waits is told so, and the line tried again, rather than waiting on.
    m_hello.setSingleShot(true);
    m_hello.setInterval(10000);
    connect(&m_hello, &QTimer::timeout, this, [this] {
        if (m_greeted || !connected())
            return;
        failWaiting(QStringLiteral("unavailable"));
        m_socket.abort();
        m_retry.start();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        // Nothing listening: start the service once, then keep trying for a
        // little while as it comes up. Still none once that while is over
        // (it ended as it started): started again, and again each time
        // trying slowly has found none for a while (twelve tries, a minute).
        if (!m_started) {
            startService();
            m_started = true;
        }
        if (++m_attempts == 41)
            giveUp();
        if (m_attempts > 41 && (m_attempts - 41) % 12 == 0)
            m_started = false;
        // Then on trying, more slowly, for one started later.
        m_retry.start();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        // The service went away: whatever was asked will not be answered.
        m_greeted = false;
        failWaiting(QStringLiteral("unavailable"));
        m_started = false;
        m_retry.start();
    });
    m_retry.setSingleShot(true);
    m_retry.setInterval(250);
    connect(&m_retry, &QTimer::timeout, this, &ServiceClient::connectNow);
    connectNow();
}

ServiceClient::~ServiceClient() {
    // The socket closes as it is destroyed; whoever was waiting is gone by
    // then and must not be called.
    disconnect(&m_socket, nullptr, this, nullptr);
}

namespace {

QString canonicalDir(const QString &path) {
    const QString real = QFileInfo(path).canonicalFilePath();
    return real.isEmpty() ? QDir::cleanPath(QDir(path).absolutePath()) : real;
}

} // namespace

bool ServiceClient::sameLibrary(const QString &a, const QString &b) {
    if (a.isEmpty() || b.isEmpty())
        return a.isEmpty() && b.isEmpty();
    return canonicalDir(a) == canonicalDir(b);
}

QString ServiceClient::socketFor(const QString &library) {
    if (const QString path = qEnvironmentVariable("PINHOLE_SOCKET"); !path.isEmpty())
        return path;
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = QDir::homePath() + "/.cache";
    if (library.isEmpty())
        return runtime + "/pinhole.sock";
    // Another library than the shared service's: a service of its own.
    // The shared one, started for it or asked for it, would show it to
    // every window, Pinhole's own from the launcher too.
    const QByteArray hash = QCryptographicHash::hash(canonicalDir(library).toUtf8(), QCryptographicHash::Sha256).toHex();
    return runtime + "/pinhole-" + QString::fromLatin1(hash.left(12)) + ".sock";
}

void ServiceClient::setHealth(const QString &health) {
    if (health == m_health)
        return;
    m_health = health;
    emit healthChanged();
    emit announced(QStringLiteral("health"), QJsonObject{{"state", health}});
}

void ServiceClient::giveUp() {
    m_retry.setInterval(5000);
    setHealth(QStringLiteral("no_service"));
    failWaiting(QStringLiteral("no_service"));
}

void ServiceClient::failWaiting(const QString &code) {
    m_unsent.clear();
    const auto waiting = std::exchange(m_waiting, {});
    for (const Callback &done : waiting)
        done(false, {}, code);
}

void ServiceClient::connectNow() {
    if (m_socket.state() != QLocalSocket::UnconnectedState)
        return;
    m_socket.connectToServer(m_socketPath);
}

void ServiceClient::startService() {
    // PINHOLED, else the one built in this checkout (app/build/pinhole ->
    // core/target/release/pinholed), else the one installed beside this
    // window (/usr/bin, ~/.local/bin), else whatever is on PATH.
    QString program = qEnvironmentVariable("PINHOLED");
    if (program.isEmpty()) {
        const QString checkout =
            QCoreApplication::applicationDirPath() + "/../../core/target/release/pinholed";
        const QString beside = QCoreApplication::applicationDirPath() + "/pinholed";
        program = QFileInfo::exists(checkout) ? QFileInfo(checkout).canonicalFilePath()
                  : QFileInfo::exists(beside) ? beside
                                              : QStandardPaths::findExecutable("pinholed");
    }
    if (program.isEmpty()) {
        qWarning("pinhole: pinholed not found; larger renditions will not load");
        return;
    }
    // Its own log, not the window's terminal: detached, it outlives the
    // window, and holding the window's stdout open would keep any pipe that
    // started the window from ever closing.
    const QString state = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/pinhole";
    QDir().mkpath(state);
    QProcess process;
    process.setProgram(program);
    QStringList arguments{QStringLiteral("--socket"), m_socketPath};
    QString log = state + "/pinholed.log";
    if (!m_library.isEmpty()) {
        // Another library's (pinhole-fake's, a test's copy): a service of
        // its own that only reads it, never syncs the source into it, and
        // ends after its last window; in a log of its own, named as its
        // socket, begun afresh.
        arguments << QStringLiteral("--library") << m_library << QStringLiteral("--given");
        log = state + '/' + QFileInfo(m_socketPath).completeBaseName() + ".log";
        QFile::remove(log);
    }
    process.setArguments(arguments);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(log, QIODevice::Append);
    process.setStandardErrorFile(log, QIODevice::Append);
    process.startDetached();
}

void ServiceClient::fetch(const QByteArray &asset, const QString &kind, Callback done, bool stream,
                          const std::optional<QString> &source) {
    QJsonObject params{{"asset", QString::fromUtf8(asset)}, {"kind", kind}};
    if (stream)
        params.insert("stream", true);
    if (source)
        params.insert("source", *source);
    else if (m_shownKnown)
        params.insert("source", m_shown);
    call(QStringLiteral("fetch"), params, std::move(done));
}

void ServiceClient::call(const QString &method, const QJsonObject &params, Callback done) {
    // None to ask, or one that speaks another protocol: said at once (after
    // this returns, as an answer would be).
    const bool none = m_health == QLatin1String("no_service") && !connected();
    if (none || m_health == QLatin1String("incompatible")) {
        QTimer::singleShot(0, this, [done = std::move(done), code = m_health] { done(false, {}, code); });
        return;
    }
    const qint64 id = m_nextId++;
    m_waiting.insert(id, std::move(done));
    send(QJsonObject{{"id", id}, {"method", method}, {"params", params}});
}

void ServiceClient::sync() {
    send(QJsonObject{{"id", m_nextId++}, {"method", "sync"}});
}

void ServiceClient::send(const QJsonObject &request) {
    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    // Nothing at all to a service of another protocol (sync too).
    if (m_health == QLatin1String("incompatible"))
        return;
    if (connected() && m_greeted)
        m_socket.write(line);
    else
        m_unsent.append(line);
}

void ServiceClient::readReplies() {
    while (m_socket.canReadLine()) {
        const QJsonObject reply = QJsonDocument::fromJson(m_socket.readLine()).object();
        const QString event = reply.value("event").toString();
        if (!m_greeted) {
            // Its first line says its protocol; one from before there was a
            // hello speaks 0, which no window of now does.
            m_greeted = true;
            m_hello.stop();
            const int protocol = event == QLatin1String("hello") ? reply.value("protocol").toInt() : 0;
            if (protocol != kProtocol) {
                qWarning("pinhole: the service speaks protocol %d, this window %d", protocol, kProtocol);
                setHealth(QStringLiteral("incompatible"));
                failWaiting(QStringLiteral("incompatible"));
            } else {
                for (const QByteArray &line : std::as_const(m_unsent))
                    m_socket.write(line);
                m_unsent.clear();
            }
            if (event == QLatin1String("hello"))
                continue;
        }
        if (reply.contains("event")) {
            if (event == QLatin1String("hello"))
                continue;
            if (event == QLatin1String("source"))
                m_source = reply;
            else if (event == QLatin1String("health")) {
                // Whatever the source is doing, a service that speaks
                // another protocol is the news until another one answers.
                if (m_health != QLatin1String("incompatible"))
                    setHealth(reply.value("state").toString());
                continue;
            }
            emit announced(event, reply);
            continue;
        }
        const auto done = m_waiting.take(reply.value("id").toInteger());
        if (!done)
            continue;
        if (reply.value("ok").toBool())
            done(true, reply.value("result").toObject(), {});
        else
            done(false, reply.value("error").toObject(), reply.value("error").toObject().value("code").toString());
    }
}
