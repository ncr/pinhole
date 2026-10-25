#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

#include <functional>

// The window's line to pinholed (docs/service.md): JSON lines over a Unix
// socket. Requests made before the service answers are queued; if nothing
// answers at all, the service is started.
class ServiceClient : public QObject {
    Q_OBJECT
public:
    // `library` is the directory the window shows; a service this client
    // starts serves that one.
    explicit ServiceClient(const QString &library, QObject *parent = nullptr);
    ~ServiceClient() override;

    using Callback = std::function<void(bool ok, const QJsonObject &result, const QString &errorCode)>;
    // `kind` as in docs/service.md: medium, full, original, video, live. With
    // `stream`, a video is answered as soon as it starts downloading, with a
    // `stream` URL to play it from.
    void fetch(const QByteArray &asset, const QString &kind, Callback done, bool stream = false);

    // Any other method of docs/service.md, such as storage and set_storage.
    void call(const QString &method, const QJsonObject &params, Callback done);

    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    // Ask the service to look for changes in iCloud now.
    void sync();

signals:
    // Something the service announced: "library_changed", with its details.
    void announced(const QString &name, const QJsonObject &details);

private:
    void connectNow();
    void startService();
    void send(const QJsonObject &request);
    void readReplies();
    static QString socketPath();

    QString m_library;
    QLocalSocket m_socket;
    QTimer m_retry;
    int m_attempts = 0;
    bool m_started = false;
    qint64 m_nextId = 1;
    QHash<qint64, Callback> m_waiting;
    QList<QByteArray> m_unsent;
};
