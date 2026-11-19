#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>

// A background check only; fetching/building a new main always needs the
// person's Update button. The running window stays alive until Restart.
class Updater : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString revision READ revision NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString logPath READ logPath NOTIFY changed)
    Q_PROPERTY(bool supported READ supported NOTIFY changed)
public:
    explicit Updater(QObject *parent = nullptr);
    QString state() const { return m_state; }
    QString revision() const { return m_revision.left(8); }
    QString error() const { return m_error; }
    QString logPath() const { return m_log; }
    bool supported() const { return !m_helper.isEmpty(); }
    // An update of this installation's checkout is switching it (bin/update's
    // update-<key>.switch is there), whichever window started it, or one
    // stopped part way left it switched only in part.
    bool switching() const { return m_switching; }
    // That marker, for a checkout: bin/update's name for it.
    static QString switchMarker(const QString &checkout);
    Q_INVOKABLE void check();
    Q_INVOKABLE void apply();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void showLog();
signals:
    void changed();
private:
    void lookForSwitch();
    void run(bool apply);
    void readJob();
    QProcess m_process;
    QTimer m_checks;
    QTimer m_timeout;
    QTimer m_jobPoll;
    QString m_state = "unavailable", m_revision, m_error, m_log;
    QString m_helper, m_metadata, m_job;
    bool m_applying = false;
    QString m_switchMarker;
    QFileSystemWatcher m_stateWatcher;
    bool m_switching = false;
    QString m_runningRevision;
};
