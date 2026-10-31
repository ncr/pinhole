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
    // As health(), for the window (the corner that says what is wrong).
    Q_PROPERTY(QString health READ health NOTIFY healthChanged)
public:
    // `library`: the directory a service this client starts is to serve
    // (the window's --library); empty, the service finds the source's own.
    explicit ServiceClient(const QString &library, QObject *parent = nullptr);
    ~ServiceClient() override;

    using Callback = std::function<void(bool ok, const QJsonObject &result, const QString &errorCode)>;
    // `kind` as in docs/service.md: medium, full, original, video, live. With
    // `stream`, a video is answered as soon as it starts downloading, with a
    // `stream` URL to play it from.
    void fetch(const QByteArray &asset, const QString &kind, Callback done, bool stream = false);
    // The source of the library the window shows (Library::source): each
    // fetch says it, so a service showing another's (switched from
    // elsewhere, not yet read here) answers other_source rather than its
    // photo of the same id.
    void setShown(const QString &source) {
        m_shown = source;
        m_shownKnown = true;
    }

    // Any other method of docs/service.md, such as storage and set_storage.
    void call(const QString &method, const QJsonObject &params, Callback done);

    // Where the photos come from, as the service last said (its "source"
    // event: kind, name, needs_sign_in, path), else as the settings say.
    QJsonObject source() const { return m_source; }
    void setSource(const QJsonObject &source) { m_source = source; }
    // "iCloud", or a folder's name: what to call it in messages.
    QString sourceName() const { return m_source.value("name").toString(); }
    // How the source is doing, as the last "health" event said: fine,
    // needs_sign_in, offline, failing; or of the client's own, no_service
    // (none answers: requests are answered so at once) or incompatible (it
    // speaks another protocol, as its "hello" says).
    QString health() const { return m_health; }

    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    // The protocol this window speaks (docs/service.md, Transport).
    static constexpr int kProtocol = 1;
    // Ask the service to look for changes in the source now.
    void sync();

signals:
    void healthChanged();
    // Something the service announced: "library_changed", with its details.
    void announced(const QString &name, const QJsonObject &details);

private:
    void connectNow();
    void startService();
    void send(const QJsonObject &request);
    void readReplies();
    void setHealth(const QString &health);
    // No service, after trying for a while: whatever waits is answered so.
    void giveUp();
    // Whatever waits is answered `code`, unsent requests too.
    void failWaiting(const QString &code);
    static QString socketPath();

    QString m_library;
    QString m_shown;
    bool m_shownKnown = false;
    QJsonObject m_source;
    QString m_health = QStringLiteral("fine");
    QLocalSocket m_socket;
    QTimer m_retry;
    int m_attempts = 0;
    bool m_started = false;
    // The service connected to has said its protocol (its first line):
    // until then requests wait in m_unsent.
    bool m_greeted = false;
    qint64 m_nextId = 1;
    QHash<qint64, Callback> m_waiting;
    QList<QByteArray> m_unsent;
};
