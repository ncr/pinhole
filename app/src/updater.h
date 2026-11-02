#pragma once

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
    Q_PROPERTY(bool perUser READ perUser CONSTANT)
public:
    explicit Updater(QObject *parent = nullptr);
    QString state() const { return m_state; }
    QString revision() const { return m_revision.left(8); }
    QString error() const { return m_error; }
    QString logPath() const { return m_log; }
    bool supported() const { return !m_helper.isEmpty(); }
    bool perUser() const { return m_perUser; }
    Q_INVOKABLE void check();
    Q_INVOKABLE void apply();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void showLog();
signals:
    void changed();
private:
    void run(bool apply);
    QProcess m_process;
    QTimer m_checks;
    QTimer m_timeout;
    QString m_state = "unavailable", m_revision, m_error, m_log;
    QString m_helper, m_metadata;
    bool m_applying = false;
    bool m_perUser = false;
};
