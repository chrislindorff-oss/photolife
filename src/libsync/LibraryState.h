#pragma once

#include "libsync/LibraryStore.h"

#include <QHash>
#include <QSet>
#include <QString>

namespace pl::libsync {

// A photo as it was when this computer and the store last agreed on it.
struct SyncedFile
{
    QString relPath;
    QString md5;
    QString sha256;
    qint64 size = 0;
    qint64 mtime = 0;          // local modification time (ms since epoch) then
    QString remoteVersion;     // the store's version of it then
    bool local = true;         // false: deliberately not on this computer
};

// What LibrarySync remembers between runs -- the common base of a three-way
// comparison between this computer, the store and last time -- in its own
// small SQLite file (one per server profile), never in the catalogue: it
// describes this computer, not the shared library.
class LibraryState
{
public:
    explicit LibraryState(QString path);   // ":memory:" for tests
    ~LibraryState();

    LibraryState(const LibraryState &) = delete;
    LibraryState &operator=(const LibraryState &) = delete;

    bool open();
    QString error() const { return m_error; }

    QHash<QString, SyncedFile> load();
    bool put(const SyncedFile &file);
    bool remove(const QString &relPath);

    // The store's photos as last listed (by LibrarySync::plan()), so what's
    // online can be known without asking it -- e.g. which photos are online
    // only, or to fetch one on demand.
    bool replaceRemote(const QList<RemoteEntry> &entries);
    QHash<QString, RemoteEntry> remote();

    // Photos fetched on demand: kept on this computer whatever the selection.
    bool pin(const QString &relPath);
    QSet<QString> pinned();

    // Photos deleted on this computer that the user chose to keep online, at
    // the version they were kept at (a newer one online asks again).
    bool keepOnline(const QString &relPath, const QString &remoteVersion);
    QHash<QString, QString> keptOnline();

    // Everything known about a photo, once it's gone from the store too.
    bool forget(const QString &relPath);

    // Groups many put()/remove() calls into one write.
    bool begin();
    bool commit();

private:
    bool exec(const QString &sql);

    QString m_path;
    QString m_connection;
    QString m_error;
};

} // namespace pl::libsync
