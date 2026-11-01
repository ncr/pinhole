// What keeps a self-test off the person's own Pictures and clipboard
// (selftest.h, "Safe by construction").

#include "selftest.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QStandardPaths>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <pwd.h>
#include <unistd.h>

namespace {

QString clean(const QString &path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? path : canonical);
}

// The account's home from the password file: $HOME is what a test moves.
QString accountHome() {
    if (const passwd *pw = getpwuid(getuid()); pw && pw->pw_dir)
        return QString::fromLocal8Bit(pw->pw_dir);
    return {};
}

// The account's own Pictures: ~/Pictures, and the folder its real
// ~/.config/user-dirs.dirs names, whatever this run's environment says.
QStringList accountPictures() {
    const QString home = accountHome();
    if (home.isEmpty())
        return {};
    QStringList found = {clean(home + "/Pictures")};
    QFile dirs(home + "/.config/user-dirs.dirs");
    if (dirs.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QByteArray &raw : dirs.readAll().split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (!line.startsWith(QLatin1String("XDG_PICTURES_DIR=")))
                continue;
            QString value = line.section('=', 1);
            if (value.startsWith('"') && value.endsWith('"') && value.size() >= 2)
                value = value.mid(1, value.size() - 2);
            value.replace(QLatin1String("$HOME"), home);
            if (QDir::isAbsolutePath(value))
                found << clean(value);
        }
    }
    found.removeDuplicates();
    return found;
}

[[noreturn]] void refuse(const QString &why) {
    std::fprintf(stderr,
                 "\nselftest: REFUSED: %s\n"
                 "selftest: A self-test never touches the person's own Pictures or clipboard (selftest.h). Run it\n"
                 "selftest: with HOME and XDG_PICTURES_DIR pointed at scratch folders.\n\n",
                 qPrintable(why));
    std::fprintf(stdout, "selftest: REFUSED: %s\n", qPrintable(why));
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
}

// The self-test's clipboard: what Pinhole last copied, in this process only.
std::unique_ptr<QMimeData> &privateClipboard() {
    static std::unique_ptr<QMimeData> data;
    return data;
}

} // namespace

bool selfTesting() { return qEnvironmentVariableIsSet("PINHOLE_SELFTEST"); }

bool isAccountPictures(const QString &path) {
    const QString p = clean(path);
    for (const QString &real : accountPictures())
        if (p == real || p.startsWith(real + '/'))
            return true;
    return false;
}

// Where Pictures is: in a self-test, XDG_PICTURES_DIR when it is given
// (Qt reads only user-dirs.dirs), else as Qt says.
QString picturesLocation() {
    if (selfTesting()) {
        const QString given = qEnvironmentVariable("XDG_PICTURES_DIR");
        const QString dir = QDir::isAbsolutePath(given) ? QDir::cleanPath(given)
                                                         : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (isAccountPictures(dir))
            refuse(QStringLiteral("this self-test would write to %1, the account's own Pictures").arg(dir));
        return dir;
    }
    return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
}

void setClipboard(QMimeData *data) {
    if (!selfTesting()) {
        QGuiApplication::clipboard()->setMimeData(data);
        return;
    }
    privateClipboard().reset(data);
}

const QMimeData *clipboardNow() {
    if (!selfTesting())
        return QGuiApplication::clipboard()->mimeData();
    return privateClipboard().get();
}

void guardSelfTest() {
    if (!selfTesting())
        return;
    // Every self-test, before anything loads: refused, exit 3, unless
    // Pictures is a scratch folder.
    const QString dir = picturesLocation();
    std::printf("selftest: Pictures is %s, the clipboard private to this window\n", qPrintable(dir));
    std::fflush(stdout);
    // Anything that still reaches the session's clipboard (a text field's
    // copy, a Qt default) ends the run at once, loudly.
    QClipboard *session = QGuiApplication::clipboard();
    QObject::connect(session, &QClipboard::dataChanged, session, [session] {
        if (session->ownsClipboard())
            refuse(QStringLiteral("this self-test set the session's clipboard"));
    });
}
