#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class ServiceClient;
class PortalFolderPicker;

// Where the photos shown come from (iCloud, or a folder on this computer),
// where Pinhole keeps everything (the library and the photos fetched from
// the source, in one folder) and how much room it may take: the settings
// panel's side of the service's source, storage and set_storage
// (docs/service.md), with the free space on that disk. Adding and choosing
// sources is Sources'.
class Storage : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    // The source: "icloud" or "folder"; what to call it; a folder's path,
    // with ~ for the home folder; whether it is one that needs signing in.
    Q_PROPERTY(QString sourceKind READ sourceKind NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY sourceChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourceChanged)
    // An iCloud source's Apple ID.
    Q_PROPERTY(QString sourceAccount READ sourceAccount NOTIFY sourceChanged)
    Q_PROPERTY(bool sourceNeedsSignIn READ sourceNeedsSignIn NOTIFY sourceChanged)
    // The folder, with ~ for the home folder.
    Q_PROPERTY(QString location READ location NOTIFY changed)
    // The limit for all of it; the least it can be (the library always
    // fits); the one it is unless set.
    Q_PROPERTY(double limit READ limit NOTIFY changed)
    Q_PROPERTY(double minLimit READ minLimit NOTIFY changed)
    Q_PROPERTY(double defaultLimit READ defaultLimit NOTIFY changed)
    Q_PROPERTY(bool limitSet READ limitSet NOTIFY changed)
    Q_PROPERTY(double used READ used NOTIFY changed)
    // Bytes of each part: library (the index and thumbnails), and of the
    // photos medium, full, original, video, live.
    Q_PROPERTY(QVariantMap kinds READ kinds NOTIFY changed)
    // Free on the disk the folder is on.
    Q_PROPERTY(double free READ free NOTIFY changed)
    Q_PROPERTY(bool freeing READ freeing NOTIFY changed)
    Q_PROPERTY(bool moving READ moving NOTIFY changed)
    Q_PROPERTY(double moved READ moved NOTIFY changed)
    Q_PROPERTY(double moveTotal READ moveTotal NOTIFY changed)
    // What went wrong with the last change, for the panel to say.
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // The service running is from before these settings: it has to be
    // restarted (an update installed while it ran leaves the old one).
    Q_PROPERTY(bool outdated READ outdated NOTIFY changed)
    // The panel is open: what is under way is followed every second.
    Q_PROPERTY(bool watching READ watching WRITE setWatching NOTIFY watchingChanged)

public:
    explicit Storage(ServiceClient *service, QObject *parent = nullptr);

    // The folder Pinhole keeps everything in, as the service's settings say
    // (~/.config/pinhole/settings.json), else ~/.local/share/pinhole.
    static QString folder();
    // The source the settings name, as the service describes one (kind,
    // name, needs_sign_in, path, account); iCloud unless they name another.
    static QJsonObject configuredSource();
    // That source's library in the folder, one to a source:
    // library/icloud/<apple id>, library/folder/home-me-Pictures
    // (docs/sources.md).
    static QString library();
    // The Apple IDs signed in to on this computer, each with a library of
    // its own.
    static QStringList accounts();

    bool loaded() const { return m_loaded; }
    QString sourceKind() const;
    QString sourceName() const;
    QString sourcePath() const;
    QString sourceAccount() const;
    bool sourceNeedsSignIn() const;
    QString location() const;
    double limit() const { return m_limit; }
    double minLimit() const { return m_minLimit; }
    double defaultLimit() const { return m_defaultLimit; }
    bool limitSet() const { return m_limitSet; }
    double used() const { return m_used; }
    QVariantMap kinds() const { return m_kinds; }
    double free() const { return m_free; }
    bool freeing() const { return m_freeing; }
    bool moving() const { return m_moving; }
    double moved() const { return m_moved; }
    double moveTotal() const { return m_moveTotal; }
    QString error() const { return m_error; }
    bool outdated() const { return m_outdated; }
    bool watching() const { return m_watching; }
    void setWatching(bool watching);

    Q_INVOKABLE void refresh();
    // A new limit, in bytes. Below what is kept, the files viewed longest
    // ago go now (the panel asks first).
    Q_INVOKABLE void setLimit(double bytes);
    // The desktop's folder picker, then everything moved to the folder chosen.
    Q_INVOKABLE void chooseLocation();
    // Stops the service; the window starts the new one at once.
    Q_INVOKABLE void restartService();
    // Bytes as people read them: "6.2 GB".
    Q_INVOKABLE QString size(double bytes) const;

signals:
    void changed();
    void sourceChanged();
    void watchingChanged();

private:
    void take(const QJsonObject &report);
    void failed(const QString &code, const QString &message);
    void moveTo(const QString &folder);

    ServiceClient *m_service;
    QTimer m_poll;
    // Followed a few seconds after a change too: freeing starts a moment
    // after the service answers.
    qint64 m_followUntil = 0;
    bool m_loaded = false;
    QString m_location, m_error;
    double m_limit = 0, m_minLimit = 0, m_defaultLimit = 0, m_used = 0, m_free = 0, m_moved = 0, m_moveTotal = 0;
    bool m_limitSet = false;
    QVariantMap m_kinds;
    bool m_freeing = false, m_moving = false, m_watching = false, m_outdated = false;
};
