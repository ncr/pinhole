#include "encoder.h"

FrameEncoder::FrameEncoder(const QString &program, const QStringList &args, QSize size, QObject *parent)
    : QObject(parent), m_program(program), m_args(args), m_size(size) {
    connect(&m_process, &QProcess::bytesWritten, this, [this] {
        if (m_waiting && !behind()) {
            m_waiting = false;
            emit ready();
        }
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(QStringLiteral("%1 did not start").arg(m_program));
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0)
            return fail(QString::fromUtf8(m_process.readAllStandardError()).trimmed());
        if (!m_ended) {
            m_ended = true;
            emit finished({});
        }
    });
}

// Not left behind, writing half a file.
FrameEncoder::~FrameEncoder() { stop(); }

void FrameEncoder::stop() {
    m_ended = true;
    disconnect(&m_process, nullptr, this, nullptr);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void FrameEncoder::start() { m_process.start(m_program, m_args); }

// Behind once more than eight frames wait for it: nine at most, the one
// that made it so among them.
bool FrameEncoder::behind() const { return m_process.bytesToWrite() > qint64(m_size.width()) * m_size.height() * 4 * 8; }

bool FrameEncoder::add(QImage frame) {
    if (m_ended)
        return false;
    if (frame.size() != m_size)
        frame = frame.scaled(m_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    frame = frame.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < frame.height(); ++y)
        m_process.write(reinterpret_cast<const char *>(frame.constScanLine(y)), qint64(frame.width()) * 4);
    m_waiting = behind();
    return !m_waiting;
}

void FrameEncoder::end() { m_process.closeWriteChannel(); }

void FrameEncoder::fail(const QString &error) {
    if (m_ended)
        return;
    stop();
    emit finished(error.isEmpty() ? QStringLiteral("%1 failed").arg(m_program) : error);
}
