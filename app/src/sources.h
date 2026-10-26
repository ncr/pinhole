#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class ServiceClient;

// Every source added (Apple IDs and folders), the one shown, and how to add
// more: the window's side of the service's sources, set_source,
// remove_source, discover, sign_in and submit_code, and of its sources and
// progress events (docs/service.md).
class Sources : public QObject {
    Q_OBJECT
    // Each source as the service lists it (id, kind, name, account or path,
    // count, thumbs), in the order they were added: 1 to 9 show them.
    Q_PROPERTY(QVariantList list READ list NOTIFY listChanged)
    // The id of the one shown; empty when there is none.
    Q_PROPERTY(QString current READ current NOTIFY listChanged)
    // Its place in the list, -1 when none.
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY listChanged)
    // The service has said what there is: until then nothing is known to be
    // missing, so the window shows no first-run screen.
    Q_PROPERTY(bool known READ known NOTIFY listChanged)
    // A library being built: phase (index, thumbs), done, total, source.
    // Empty when none is.
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY progressChanged)
    // When the source shown was last looked at for new photos and when it
    // is next: source (its id), last and next (milliseconds since 1970;
    // no last before the first round that worked, no next while one runs),
    // and push, whether it tells of changes itself as they happen.
    Q_PROPERTY(QVariantMap rounds READ rounds NOTIFY roundsChanged)
    // What could be added: the Apple IDs icloudpd is signed in to here (as
    // its file names), and folders with photos (path, name, assets, live,
    // bytes).
    Q_PROPERTY(QStringList icloudpd READ icloudpd NOTIFY discoveredChanged)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY discoveredChanged)
    // Signing in: "" (nothing under way), "busy" (Apple is asked),
    // "needs_code"; and what went wrong, said for people.
    Q_PROPERTY(QString signInState READ signInState NOTIFY signInChanged)
    Q_PROPERTY(QString signInError READ signInError NOTIFY signInChanged)
    // What went wrong with the last change to the list.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit Sources(ServiceClient *service, QObject *parent = nullptr);

    QVariantList list() const { return m_list; }
    QString current() const { return m_current; }
    int currentIndex() const;
    bool known() const { return m_known; }
    QVariantMap progress() const { return m_progress; }
    QVariantMap rounds() const { return m_rounds; }
    QStringList icloudpd() const { return m_icloudpd; }
    QVariantList folders() const { return m_folders; }
    QString signInState() const { return m_signInState; }
    QString signInError() const { return m_signInError; }
    QString error() const { return m_error; }

    // Show a source, by id or by its place in the list.
    Q_INVOKABLE void show(const QString &id);
    Q_INVOKABLE bool showAt(int index);
    // Take it out of the list, its library with it.
    Q_INVOKABLE void remove(const QString &id);
    // Sign out of an Apple ID: its session goes, its library stays.
    Q_INVOKABLE void signOut(const QString &id);
    // Add a folder and show it: one given, or one chosen in the desktop's
    // picker.
    Q_INVOKABLE void addFolder(const QString &path);
    Q_INVOKABLE void chooseFolder();
    // Look again for what could be added.
    Q_INVOKABLE void discover();
    // Whether icloudpd is signed in to this Apple ID here, so no password is
    // needed (its file names are the Apple ID with only letters, digits and
    // _ kept).
    Q_INVOKABLE bool icloudpdHas(const QString &appleId) const;
    // Sign in to an Apple ID: with its password, or without one through
    // icloudpd's session; then the code Apple sends, if it asks for one.
    Q_INVOKABLE void signIn(const QString &appleId, const QString &password);
    Q_INVOKABLE void submitCode(const QString &appleId, const QString &code);
    Q_INVOKABLE void cancelSignIn();

signals:
    void listChanged();
    void progressChanged();
    void roundsChanged();
    void discoveredChanged();
    void signInChanged();
    void errorChanged();
    // Signed in and shown: the sign-in can close.
    void signedIn();
    // Signing out of an Apple ID, as asked here: said as it is asked.
    void signedOut(const QString &id);
    // Another source is shown, as it was asked for here: its name, for the
    // window to say. Said as it is asked for when the source is known, else
    // (one just added) once the service has made it.
    void switched(const QString &name);

private:
    void take(const QJsonObject &sources);
    void setSignIn(const QString &state, const QString &errorCode = {});
    void setError(const QString &code);

    ServiceClient *m_service;
    QVariantList m_list;
    QString m_current;
    bool m_known = false;
    QVariantMap m_progress;
    QVariantMap m_rounds;
    QStringList m_icloudpd;
    QVariantList m_folders;
    QString m_signInState, m_signInError, m_error;
    // A source added here, announced once the service shows it.
    QString m_switching;
};
