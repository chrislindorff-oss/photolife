#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace pl::app {

// The combined size, in bytes, of every regular file under `dir` (recursing
// into subdirectories). 0 if `dir` doesn't exist. Used to report cache disk
// use to the user; not cheap for a large cache, so call it only when the user
// asks (e.g. a "Storage Usage" dialog), not on a hot path.
qint64 directoryBytes(const QString &dir);

// What a clean-up removed.
struct Removed
{
    int files = 0;
    qint64 bytes = 0;
};

// These three only ever touch PhotoLife's own caches -- files it made and
// can make again -- never photos. Each is safe on a worker thread.

// Brings the files under `dir` down to `maxBytes` in all, oldest (by the
// time they were written) first; to a little under it, so the next few
// don't tip it straight back over. Nothing happens while it's within the limit.
Removed pruneToSize(const QString &dir, qint64 maxBytes);

// Removes every file under `dir`, and the folders that leaves empty; `dir`
// itself stays.
Removed clearDirectory(const QString &dir);

// Removes the thumbnails under `cacheDir` (a thumb::ThumbnailCache's:
// <size>/<xx>/<key>.jpg) whose key isn't in `keep`.
Removed removeThumbnailsExcept(const QString &cacheDir, const QSet<QString> &keep);

// The thumbnail keys of every photo in the SQLite catalogue at `path` (its
// content hash, or for one without, the hash of its path, as
// thumb::ThumbnailCache keys them), opened read-only. False (and `*error`) when it can't be read.
bool catalogueContentHashes(const QString &path, QSet<QString> *hashes, QString *error = nullptr);

} // namespace pl::app
