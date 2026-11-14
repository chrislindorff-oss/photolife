#include "db/CatalogueBackup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace pl {
namespace {

const QRegularExpression &backupName()
{
    static const QRegularExpression re(
        QStringLiteral("^PhotoLife catalogue v\\d+ before v\\d+ \\d{4}-\\d{2}-\\d{2} \\d{6}(?: \\(\\d+\\))?\\.db$"));
    return re;
}

} // namespace

QStringList catalogueBackups(const QString &folder)
{
    // Newest first, by the time in the name (and a same-second " (2)" after).
    static const QRegularExpression when(QStringLiteral("(\\d{4}-\\d{2}-\\d{2} \\d{6})(?: \\((\\d+)\\))?\\.db$"));
    QList<QPair<QString, QString>> keyed;
    const QFileInfoList files = QDir(folder).entryInfoList({QStringLiteral("PhotoLife catalogue v*.db")}, QDir::Files);
    for (const QFileInfo &f : files) {
        if (!backupName().match(f.fileName()).hasMatch())
            continue;
        const QRegularExpressionMatch m = when.match(f.fileName());
        keyed.append({m.captured(1) + QStringLiteral("%1").arg(m.captured(2).toInt(), 4, 10, QLatin1Char('0')),
                      f.absoluteFilePath()});
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QStringList out;
    for (const auto &k : keyed)
        out << k.second;
    return out;
}

QStringList pruneCatalogueBackups(const QString &folder, int keep)
{
    QStringList removed;
    const QStringList backups = catalogueBackups(folder);
    for (qsizetype i = std::max(0, keep); i < backups.size(); ++i) {
        if (QFile::remove(backups.at(i)))
            removed << backups.at(i);
    }
    return removed;
}

CatalogueBackupResult backupCatalogue(const QString &connectionName, const QString &folder, int fromVersion,
                                      int toVersion, int keep, const QDateTime &now)
{
    CatalogueBackupResult result;
    if (folder.isEmpty() || !QDir().mkpath(folder)) {
        result.error = QObject::tr("the folder can't be made or written to");
        return result;
    }
    const QString stem = QStringLiteral("PhotoLife catalogue v%1 before v%2 %3")
                             .arg(fromVersion)
                             .arg(toVersion)
                             .arg(now.toString(QStringLiteral("yyyy-MM-dd HHmmss")));
    QString path = QDir(folder).filePath(stem + QStringLiteral(".db"));
    for (int n = 2; QFile::exists(path); ++n)
        path = QDir(folder).filePath(QStringLiteral("%1 (%2).db").arg(stem).arg(n));

    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    // VACUUM INTO takes the file name as a literal: quotes doubled.
    const QString literal = QString(QDir::toNativeSeparators(path)).replace(QLatin1Char('\''), QStringLiteral("''"));
    if (!q.exec(QStringLiteral("VACUUM INTO '%1'").arg(literal))) {
        result.error = q.lastError().text();
        QFile::remove(path);
        return result;
    }
    if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) {
        result.error = QObject::tr("the copy wasn't written");
        QFile::remove(path);
        return result;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    result.ok = true;
    result.path = path;
    result.removed = pruneCatalogueBackups(folder, keep);
    return result;
}

} // namespace pl
