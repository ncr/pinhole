#include "buildversion.h"
#include "updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

Updater::Updater(QObject *parent) : QObject(parent) {
    QString prefix = qEnvironmentVariable("PINHOLE_INSTALL_PREFIX");
    if (prefix.isEmpty())
        prefix = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");
    m_metadata = QDir(prefix).filePath("share/pinhole/update.json");
    const QString helper = QDir(prefix).filePath("bin/pinhole-update");
    QFile file(m_metadata);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject installed = QJsonDocument::fromJson(file.readAll()).object();
        // Distributed packages and copied installs have no self-updater.
        if (installed.value("mode").toString() == "developer" && QFileInfo(helper).isExecutable()) {
            m_helper = helper;
            m_state = "idle";
#ifdef PINHOLE_BUILD_COMMIT
            m_runningRevision = QStringLiteral(PINHOLE_BUILD_COMMIT);
#else
            m_runningRevision = installed.value("commit").toString();
#endif
            if (const QString source = installed.value("source").toString(); !source.isEmpty()) {
                m_switchMarker = switchMarker(source);
                const QString state = QFileInfo(m_switchMarker).path();
                QDir().mkpath(state);
                m_stateWatcher.addPath(state);
                connect(&m_stateWatcher, &QFileSystemWatcher::directoryChanged, this, &Updater::lookForSwitch);
                lookForSwitch();
            }
        }
    }
    m_jobPoll.setInterval(1000);
    connect(&m_jobPoll, &QTimer::timeout, this, &Updater::readJob);
    m_checks.setInterval(60 * 60 * 1000);
    connect(&m_checks, &QTimer::timeout, this, &Updater::check);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        // Checks have a strict deadline. Builds run as long as needed.
        if (!m_applying && m_process.state() != QProcess::NotRunning)
            m_process.kill();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_timeout.stop();
            m_state = "error";
            m_error = "failed";
            emit changed();
        }
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        m_timeout.stop();
        const QJsonObject result = QJsonDocument::fromJson(m_process.readAllStandardOutput()).object();
        if (code != 0 || status != QProcess::NormalExit || result.isEmpty()) {
            m_state = "error";
            m_error = result.value("error").toString("failed");
            m_log = result.value("log").toString();
        } else {
            m_revision = result.value("revision").toString();
            m_log = result.value("log").toString();
            m_error.clear();
            m_state = result.value("state").toString();
            if (m_state.isEmpty())
                m_state = m_applying ? "ready" : result.value("available").toBool() ? "available" : "current";
            if (m_state == "updating" && !result.value("job").toString().isEmpty()) {
                m_job = result.value("job").toString();
                m_jobPoll.start();
            }
        }
        emit changed();
    });
    // Test/scratch windows never make network requests on their own.
    if (supported() && !qEnvironmentVariableIsSet("PINHOLE_SELFTEST")
        && !QCoreApplication::arguments().contains("--bench")
        && !QCoreApplication::arguments().contains("--library")) {
        m_checks.start();
        QTimer::singleShot(15000, this, &Updater::check);
    }
}

QString Updater::switchMarker(const QString &checkout) {
    // As bin/update names it: by the checkout's real path, in Pinhole's
    // state folder.
    QString real = QFileInfo(checkout).canonicalFilePath();
    if (real.isEmpty())
        real = QDir::cleanPath(QDir(checkout).absolutePath());
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(QFile::encodeName(real), QCryptographicHash::Sha256).toHex().left(16));
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + QStringLiteral("/.local/state");
    return state + QStringLiteral("/pinhole/update-") + key + QStringLiteral(".switch");
}

void Updater::lookForSwitch() {
    // Once seen, for good: the checkout this window's binary was built from
    // is gone, whether the switch finished (its marker removed) or not.
    if (m_switched || m_switchMarker.isEmpty() || !QFileInfo::exists(m_switchMarker))
        return;
    m_switched = true;
    emit changed();
}

void Updater::run(bool apply) {
    if (!supported() || m_process.state() != QProcess::NotRunning || m_state == "ready" || m_state == "updating")
        return;
    m_applying = apply;
    m_state = apply ? "updating" : "checking";
    m_error.clear();
    emit changed();
    QStringList args{apply ? "--launch" : "--check", "--metadata", m_metadata};
    if (apply) args.append({"--revision", m_revision});
    else if (!m_runningRevision.isEmpty()) args.append({"--running-revision", m_runningRevision});
    m_process.start(m_helper, args);
    if (!apply)
        m_timeout.start(60000);
}

void Updater::readJob() {
    QFile file(m_job);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject result = QJsonDocument::fromJson(file.readAll()).object();
    const QString state = result.value("state").toString();
    if (state == "updating") {
        const qint64 pid = result.value("pid").toInteger();
        if (pid && QFileInfo::exists("/proc/" + QString::number(pid)))
            return;
        if (!pid && QDateTime::currentSecsSinceEpoch() - result.value("started").toDouble() < 60)
            return;
        m_state = "error";
        m_error = "interrupted";
    } else if (state == "ready" || state == "error") {
        m_state = state;
        m_error = result.value("error").toString();
        m_log = result.value("log").toString();
        m_revision = result.value("revision").toString(m_revision);
    } else {
        return;
    }
    m_jobPoll.stop();
    emit changed();
}

void Updater::check() { run(false); }
void Updater::apply() { if (m_state == "available") run(true); }
void Updater::showLog() { if (!m_log.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_log)); }
void Updater::restart() {
    if (m_state != "ready")
        return;
    QFile file(m_metadata);
    QJsonObject installed;
    if (file.open(QIODevice::ReadOnly))
        installed = QJsonDocument::fromJson(file.readAll()).object();
    if (!QFileInfo(installed.value("prefix").toString() + "/bin/pinhole").isExecutable()
        || !QFileInfo(installed.value("source").toString() + "/app/build/pinhole").isExecutable()) {
        m_error = "restart";
        m_state = "error";
        emit changed();
        return;
    }
    QStringList args{"--restart", "--metadata", m_metadata, "--pid",
                     QString::number(QCoreApplication::applicationPid()), "--"};
    args.append(QCoreApplication::arguments().mid(1));
    if (QProcess::startDetached(m_helper, args))
        QCoreApplication::quit();
    else {
        m_error = "restart";
        m_state = "error";
        emit changed();
    }
}
