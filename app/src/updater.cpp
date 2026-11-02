#include "updater.h"

#include <QCoreApplication>
#include <QDesktopServices>
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
    if (QFileInfo::exists(m_metadata) && QFileInfo(helper).isExecutable()) {
        m_helper = helper;
        m_state = "idle";
        QFile file(m_metadata);
        if (file.open(QIODevice::ReadOnly))
            m_perUser = QJsonDocument::fromJson(file.readAll()).object().value("mode").toString() == "package";
    }
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
            if (m_applying && !result.value("metadata").toString().isEmpty()) {
                m_metadata = result.value("metadata").toString();
                // A system package updates into the user's own prefix.
                const QString prefix = QFileInfo(m_metadata).dir().absoluteFilePath("../..");
                m_helper = QDir(prefix).filePath("bin/pinhole-update");
            }
            m_state = m_applying ? "ready" : result.value("available").toBool() ? "available" : "current";
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

void Updater::run(bool apply) {
    if (!supported() || m_process.state() != QProcess::NotRunning || m_state == "ready")
        return;
    m_applying = apply;
    m_state = apply ? "updating" : "checking";
    m_error.clear();
    emit changed();
    m_process.start(m_helper, {apply ? "--apply" : "--check", "--metadata", m_metadata});
    if (!apply)
        m_timeout.start(45000);
}

void Updater::check() { run(false); }
void Updater::apply() { if (m_state == "available") run(true); }
void Updater::showLog() { if (!m_log.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_log)); }
void Updater::restart() {
    if (m_state != "ready")
        return;
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
