#include "tickbundle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace lt {

QString bundledTickDir() {
    QStringList dirs{qEnvironmentVariable("LIBRETAP_TICK_DIR")};
    if (QCoreApplication::instance()) {
        const QString app = QCoreApplication::applicationDirPath();
        dirs << app + QStringLiteral("/../share/libretap/tick")   // Linux package
             << app + QStringLiteral("/tick")                     // Windows
             << app + QStringLiteral("/../Resources/tick");       // macOS bundle
    }
    for (const QString& d : dirs)
        if (!d.isEmpty() && QFileInfo::exists(d + QStringLiteral("/Tick.exe"))) return QDir(d).canonicalPath();
    return {};
}

QStringList bundledTickFiles() {
    const QString d = bundledTickDir();
    return d.isEmpty() ? QStringList() : QDir(d).entryList(QDir::Files, QDir::Name);
}

bool copyTick(const QString& destination, QStringList* copied, QString* error) {
    const QString src = bundledTickDir();
    if (src.isEmpty()) {
        if (error) *error = QStringLiteral("this LibreTap has no Tick inside; download Tick from https://github.com/ohlincha/libretap/releases");
        return false;
    }
    if (!QFileInfo(destination).isDir()) {
        if (error) *error = QStringLiteral("%1 is not a folder").arg(destination);
        return false;
    }
    QDir dest(destination);
    if (!dest.mkpath(QStringLiteral("Tick"))) {
        if (error) *error = QStringLiteral("cannot make the folder Tick in %1 (write-protected or full?)").arg(destination);
        return false;
    }
    dest.cd(QStringLiteral("Tick"));
    for (const QString& name : QDir(src).entryList(QDir::Files, QDir::Name)) {
        const QString to = dest.filePath(name);
        if (QFileInfo::exists(to) && !QFile::remove(to)) {
            if (error) *error = QStringLiteral("cannot replace %1 (is Tick running from there?)").arg(to);
            return false;
        }
        if (!QFile::copy(QDir(src).filePath(name), to)) {
            if (error) *error = QStringLiteral("cannot write %1 (write-protected or full?)").arg(to);
            return false;
        }
        QFile f(to);
        QFile::Permissions p = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
        if (name == QLatin1String("tick")) p |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
        f.setPermissions(p);   // read-only sources (/usr/share) would otherwise stay read-only on the stick
        if (copied) *copied << to;
    }
    return true;
}

}  // namespace lt
