#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class ServiceClient;

// Every source added, the one shown, the kinds there are, and what could be
// added: the window's side of the service's sources, kinds, set_source,
// remove_source, discover, sign_in, submit_code and sign_out, and of its
// sources, kinds and progress events (docs/service.md). What is particular
// to a kind (iCloud's sign-in, a folder chosen) is its plugin's: it asks
// through pinhole.sources (PluginSources, plugins.h), which comes here.
class Sources : public QObject {
    Q_OBJECT
    // Each source as the service lists it (id, kind, plugin, name, count,
    // thumbs, and its kind's own fields: account, path, key), in the order
    // they were added: 1 to 9 show them. Only those whose kind is on.
    Q_PROPERTY(QVariantList list READ list NOTIFY listChanged)
    // The id of the one shown; empty when there is none.
    Q_PROPERTY(QString current READ current NOTIFY listChanged)
    // Its place in the list, -1 when none.
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY listChanged)
    // The service has said what there is: until then nothing is known to be
    // missing, so the window shows no first-run screen.
    Q_PROPERTY(bool known READ known NOTIFY listChanged)
    // Every kind of source the service knows, each { kind, plugin, on,
    // needs_sign_in, accepts }; empty until it has said.
    Q_PROPERTY(QVariantList kinds READ kinds NOTIFY kindsChanged)
    // A library being built: phase (index, thumbs), done, total, source.
    // Empty when none is.
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY progressChanged)
    // When the source shown was last looked at for new photos and when it
    // is next: source (its id), last and next (milliseconds since 1970;
    // no last before the first round that worked, no next while one runs),
    // and push, whether it tells of changes itself as they happen.
    Q_PROPERTY(QVariantMap rounds READ rounds NOTIFY roundsChanged)
    // What could be added, by kind, as each kind that is on says
    // (discover): iCloud's { icloudpd: [...] }, folders' { folders: [...] }.
    Q_PROPERTY(QVariantMap discovered READ discovered NOTIFY discoveredChanged)
    // What went wrong with the last change to the list.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit Sources(ServiceClient *service, QObject *parent = nullptr);

    QVariantList list() const { return m_list; }
    QString current() const { return m_current; }
    int currentIndex() const;
    bool known() const { return m_known; }
    QVariantList kinds() const { return m_kinds; }
    QVariantMap progress() const { return m_progress; }
    QVariantMap rounds() const { return m_rounds; }
    QVariantMap discovered() const { return m_discovered; }
    QString error() const { return m_error; }
    // The list entry of a source, empty if it is not listed.
    QVariantMap entry(const QString &id) const;

    // Show a source, by id or by its place in the list.
    Q_INVOKABLE void show(const QString &id);
    Q_INVOKABLE bool showAt(int index);
    // Take it out of the list, its library with it.
    Q_INVOKABLE void remove(const QString &id);
    // Look again for what could be added.
    Q_INVOKABLE void discover();
    // The kinds of the sources added, whether their plugins are on or not
    // (the settings'): the window says which plugin to turn on for them.
    Q_INVOKABLE QStringList addedKinds() const;

    // What a kind's plugin asks (PluginSources), answered with the
    // service's result, or its error code and message.
    using Answer = std::function<void(bool ok, const QJsonObject &result, const QString &code, const QString &message)>;
    // A source of `kind` added from what its plugin gives (set_source),
    // and shown.
    void add(const QString &kind, const QJsonObject &params, Answer done);
    // Signing in to a source of `kind` (sign_in, submit_code): signed in,
    // it is added and shown.
    void signIn(const QString &kind, const QJsonObject &params, Answer done);
    void submitCode(const QString &kind, const QJsonObject &params, Answer done);
    // Signing out: its session goes, it stays with its library.
    void signOut(const QString &id, Answer done);

signals:
    void listChanged();
    void kindsChanged();
    void progressChanged();
    void roundsChanged();
    void discoveredChanged();
    void errorChanged();
    // Signing out of a source, as asked here: said as it is asked.
    void signedOut(const QString &id);
    // Another source is shown, as it was asked for here: its name, for the
    // window to say. Said as it is asked for when the source is known, else
    // (one just added, or signed in to) once the service has shown it.
    void switched(const QString &name);

private:
    void take(const QJsonObject &sources);
    void setError(const QString &code);
    // Signed in, or added: say so once the service shows `id` (now, if it
    // does already: its sources come before its answer).
    void announce(const QString &id);

    ServiceClient *m_service;
    QVariantList m_list;
    QString m_current;
    bool m_known = false;
    QVariantList m_kinds;
    QVariantMap m_progress;
    QVariantMap m_rounds;
    QVariantMap m_discovered;
    QString m_error;
    // A source added here, announced once the service shows it.
    QString m_switching;
};
