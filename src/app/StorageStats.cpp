#include "app/StorageStats.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>

namespace pl::app {
namespace {

// Folders under `dir` left with nothing in them, deepest first.
void removeEmptyFolders(const QString &dir)
{
    QStringList folders;
    QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        folders << it.next();
    std::sort(folders.begin(), folders.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const QString &folder : std::as_const(folders))
        QDir().rmdir(folder);   // fails, harmlessly, unless it's empty
}

} // namespace

qint64 directoryBytes(const QString &dir)
{
    qint64 total = 0;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

Removed pruneToSize(const QString &dir, qint64 maxBytes)
{
    Removed out;
    if (dir.isEmpty() || maxBytes < 0)
        return out;
    struct Entry
    {
        QString path;
        qint64 size;
        QDateTime written;
    };
    QList<Entry> files;
    qint64 total = 0;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        files.append({info.absoluteFilePath(), info.size(), info.lastModified()});
        total += info.size();
    }
    if (total <= maxBytes)
        return out;
    std::sort(files.begin(), files.end(), [](const Entry &a, const Entry &b) { return a.written < b.written; });
    const qint64 target = maxBytes - maxBytes / 10;
    for (const Entry &f : std::as_const(files)) {
        if (total <= target)
            break;
        if (QFile::remove(f.path)) {
            total -= f.size;
            out.bytes += f.size;
            ++out.files;
        }
    }
    removeEmptyFolders(dir);
    return out;
}

Removed clearDirectory(const QString &dir)
{
    Removed out;
    if (dir.isEmpty())
        return out;
    QStringList files;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    for (const QString &path : std::as_const(files)) {
        const qint64 size = QFileInfo(path).size();
        if (QFile::remove(path)) {
            out.bytes += size;
            ++out.files;
        }
    }
    removeEmptyFolders(dir);
    return out;
}

Removed removeThumbnailsExcept(const QString &cacheDir, const QSet<QString> &keep)
{
    Removed out;
    if (cacheDir.isEmpty())
        return out;
    QStringList doomed;
    QDirIterator it(cacheDir, {QStringLiteral("*.jpg")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!keep.contains(it.fileInfo().completeBaseName()))
            doomed << it.filePath();
    }
    for (const QString &path : std::as_const(doomed)) {
        const qint64 size = QFileInfo(path).size();
        if (QFile::remove(path)) {
            out.bytes += size;
            ++out.files;
        }
    }
    removeEmptyFolders(cacheDir);
    return out;
}

bool catalogueContentHashes(const QString &path, QSet<QString> *hashes, QString *error)
{
    const QString name = QStringLiteral("storage-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!QFileInfo::exists(path)) {
            if (error)
                *error = QStringLiteral("no such file");
        } else if (!db.open()) {
            if (error)
                *error = db.lastError().text();
        } else {
            QSqlQuery q(db);
            q.setForwardOnly(true);
            if (q.exec(QStringLiteral("SELECT content_hash, path FROM rendition"))) {
                while (q.next()) {
                    const QString hash = q.value(0).toString();
                    // Without a content hash, ThumbnailCache keys it by its path.
                    hashes->insert(!hash.isEmpty() ? hash
                                                   : QString::fromLatin1(QCryptographicHash::hash(
                                                         q.value(1).toString().toUtf8(), QCryptographicHash::Sha1)
                                                                             .toHex()));
                }
                ok = true;
            } else if (error) {
                *error = q.lastError().text();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

} // namespace pl::app
