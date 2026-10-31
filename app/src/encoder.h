#pragma once

#include <QImage>
#include <QObject>
#include <QProcess>

// Frames of one size into an encoder's input (ffmpeg's, for a plugin's
// recording), never waiting for it on the window's thread: while it is
// more than a few frames behind, add() says so, and ready() comes once it
// has caught up. finished() once, with what went wrong if anything did.
class FrameEncoder : public QObject {
    Q_OBJECT
public:
    FrameEncoder(const QString &program, const QStringList &args, QSize size, QObject *parent = nullptr);
    ~FrameEncoder() override;

    void start();
    // The frame, as 32-bit pixels of the size given; false while it is
    // too far behind to be given another (wait for ready()).
    bool add(QImage frame);
    // No more frames: finished() once it has made its file.
    void end();
    // Stopped where it is, and nothing more said.
    void stop();
    bool behind() const;

signals:
    void ready();
    void finished(const QString &error);

private:
    void fail(const QString &error);

    QString m_program;
    QStringList m_args;
    QSize m_size;
    QProcess m_process;
    bool m_waiting = false, m_ended = false;
};
