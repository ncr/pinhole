#include "serviceclient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

ServiceClient::ServiceClient(QObject *parent) : QObject(parent) {
    connect(&m_socket, &QLocalSocket::readyRead, this, &ServiceClient::readReplies);
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_attempts = 0;
        for (const QByteArray &line : std::as_const(m_unsent))
            m_socket.write(line);
        m_unsent.clear();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        // Nothing listening: start the service once, then keep trying for a
        // little while as it comes up.
        if (!m_started) {
            startService();
            m_started = true;
        }
        if (m_attempts++ < 40)
            m_retry.start();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        // The service went away: whatever was asked will not be answered.
        const auto waiting = std::exchange(m_waiting, {});
        for (const Callback &done : waiting)
            done(false, {}, QStringLiteral("unavailable"));
        m_started = false;
        m_retry.start();
    });
    m_retry.setSingleShot(true);
    m_retry.setInterval(250);
    connect(&m_retry, &QTimer::timeout, this, &ServiceClient::connectNow);
    connectNow();
}

QString ServiceClient::socketPath() {
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = QDir::homePath() + "/.cache";
    return runtime + "/pinhole.sock";
}

void ServiceClient::connectNow() {
    if (m_socket.state() != QLocalSocket::UnconnectedState)
        return;
    m_socket.connectToServer(socketPath());
}

void ServiceClient::startService() {
    // PINHOLED, else the one built in this checkout (app/build/pinhole ->
    // core/target/release/pinholed), else whatever is on PATH.
    QString program = qEnvironmentVariable("PINHOLED");
    if (program.isEmpty()) {
        const QString checkout =
            QCoreApplication::applicationDirPath() + "/../../core/target/release/pinholed";
        program = QFileInfo::exists(checkout) ? QFileInfo(checkout).canonicalFilePath()
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
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(state + "/pinholed.log", QIODevice::Append);
    process.setStandardErrorFile(state + "/pinholed.log", QIODevice::Append);
    process.startDetached();
}

void ServiceClient::fetch(const QByteArray &asset, const QString &kind, Callback done) {
    const qint64 id = m_nextId++;
    m_waiting.insert(id, std::move(done));
    send(QJsonObject{{"id", id},
                     {"method", "fetch"},
                     {"params", QJsonObject{{"asset", QString::fromUtf8(asset)}, {"kind", kind}}}});
}

void ServiceClient::send(const QJsonObject &request) {
    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (connected())
        m_socket.write(line);
    else
        m_unsent.append(line);
}

void ServiceClient::readReplies() {
    while (m_socket.canReadLine()) {
        const QJsonObject reply = QJsonDocument::fromJson(m_socket.readLine()).object();
        const auto done = m_waiting.take(reply.value("id").toInteger());
        if (!done)
            continue;
        if (reply.value("ok").toBool())
            done(true, reply.value("result").toObject(), {});
        else
            done(false, {}, reply.value("error").toObject().value("code").toString());
    }
}
