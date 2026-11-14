#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace pl {

// A copy of the local catalogue made before an upgrade changes it, so a
// step back to the previous PhotoLife stays possible. Consistent even with
// the database open (SQLite's VACUUM INTO). Named
// "PhotoLife catalogue v23 before v24 2026-11-14 093000.db"; only files
// named like that are ever pruned from the folder.
struct CatalogueBackupResult
{
    bool ok = false;
    QString path;     // the backup made
    QString error;    // why not
    QStringList removed;   // older backups pruned
};

CatalogueBackupResult backupCatalogue(const QString &connectionName, const QString &folder, int fromVersion,
                                      int toVersion, int keep = 10,
                                      const QDateTime &now = QDateTime::currentDateTime());

// PhotoLife's own catalogue backups in `folder`, newest first.
QStringList catalogueBackups(const QString &folder);

// Removes all but the newest `keep` of them; returns what it removed.
QStringList pruneCatalogueBackups(const QString &folder, int keep);

} // namespace pl
