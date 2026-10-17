#pragma once

#include "libsync/LibraryState.h"
#include "libsync/LibraryStore.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace pl::libsync {

struct LibrarySyncOptions
{
    bool upload = true;
    bool download = true;
    QString who;   // recorded as the uploader; names conflict copies
    // Which photos only online should be downloaded (relative paths); empty:
    // all of them. Never stops a photo already here being updated.
    std::function<bool(const QString &relPath)> wanted;
    // Also move the copies here of photos outside `wanted` that are safely
    // online (LibraryPlan::freeable) into kTrashDir -- never deleted.
    bool freeUp = false;
    // Also take the photos deleted here out of the store
    // (LibraryPlan::deletedHere) -- unless that looks like a mistake
    // (LibraryPlan::deletionsLookSuspicious).
    bool removeDeletedHere = false;
};

// What a sync would do, worked out without transferring anything.
struct LibraryPlan
{
    QStringList toUpload;            // new or changed here
    QList<RemoteEntry> toDownload;   // new or changed online
    QList<RemoteEntry> conflicts;    // changed in both places
    QStringList toTrash;             // deleted online: the copies here get moved aside
    QStringList removedHere;         // deleted here: left online, not downloaded again
    int notWanted = 0;               // only online, outside `wanted`
    int onlineOnly = 0;              // deleted here earlier (or never wanted), still online
    int inSync = 0;
    qint64 uploadBytes = 0;
    qint64 downloadBytes = 0;
    // Here and outside `wanted`, unchanged since they were last synced and
    // still the same online: their copies here can go without losing anything.
    QStringList freeable;
    qint64 freeableBytes = 0;
    // Deleted on this computer after they were synced (now or at an earlier
    // sync), and unchanged online since: what may be taken out online too.
    // Photos freed up for space, or never downloaded, are never among them.
    QList<RemoteEntry> deletedHere;
    QList<RemoteEntry> keptOnline;   // ...that the user chose to keep online
    // So many of the photos synced here have vanished that it's more likely
    // a drive not connected, or a folder moved, than deleting them: nothing
    // is removed online automatically.
    bool deletionsLookSuspicious = false;

    bool nothingToDo() const
    {
        return toUpload.isEmpty() && toDownload.isEmpty() && conflicts.isEmpty()
               && toTrash.isEmpty() && removedHere.isEmpty();
    }
};

struct LibrarySyncResult
{
    LibraryPlan plan;
    int uploaded = 0;
    int downloaded = 0;
    int conflicts = 0;       // each left two files: ours, and theirs as "name (from X)"
    int trashed = 0;
    int freed = 0;
    int removedOnline = 0;   // taken out of the store (deletedHere)
    qint64 bytesFreed = 0;
    qint64 bytesUp = 0;
    qint64 bytesDown = 0;
    QStringList failures;    // "<relative path>: <why>"
    bool cancelled = false;
};

// Keeps the photo library folder on this computer and a LibraryStore in
// step. Files are compared by content (MD5, which the stores report
// without downloading anything) against the state both sides were in at the
// last sync (LibraryState), so it can tell "changed here" from "changed
// online":
//
//  - new or changed here            -> upload
//  - new or changed online          -> download (if wanted, for new ones)
//  - changed in both places         -> keep both: theirs saved beside ours
//                                      as "name (from <who>).ext", both uploaded
//  - deleted online, unchanged here -> moved to <root>/.photolife-trash/<date>/
//  - freed up (not wanted here)     -> moved there too: LibrarySync never
//                                      deletes a photo on this computer
//  - deleted here                   -> left online, and not downloaded again
//
// Transfers run several at a time when the store allows (see
// LibraryStore::parallelTransfers()); the state is only ever written from
// the calling thread. Nothing is ever deleted online. Downloads land in a ".photolife-partial"
// file, are checked against the store's hash, then renamed into place; the
// state is saved after every file, so an interrupted sync just carries on.
// Only the files the scanner would catalogue (images and RAWs, outside hidden
// folders) take part.
//
// Synchronous: run it on a worker thread.
class LibrarySync
{
public:
    using Progress = std::function<void(const QString &phase, int done, int total)>;
    using CancelFn = std::function<bool()>;

    static constexpr const char *kTrashDir = ".photolife-trash";
    static constexpr const char *kPartialSuffix = ".photolife-partial";

    LibrarySync(LibraryStore &store, LibraryState &state, QString libraryRoot);

    void setProgress(Progress progress) { m_progress = std::move(progress); }
    void setCancel(CancelFn cancel);
    QString error() const { return m_error; }

    // Compares the two sides. Records files that already agree (so a first
    // sync of a library that's already in both places transfers nothing).
    // nullopt if the store can't be listed.
    std::optional<LibraryPlan> plan(const LibrarySyncOptions &options = {});

    // plan(), then does it. nullopt only if planning failed; failures of
    // single files are listed in the result.
    std::optional<LibrarySyncResult> run(const LibrarySyncOptions &options = {});

    // Downloads just these photos (relative paths), as last listed online
    // (LibraryState::remote()) -- no listing, so it's quick -- and pins them
    // so they stay whatever `wanted` says. Already-current ones are skipped.
    LibrarySyncResult fetch(const QStringList &relPaths);

    // Takes just these photos (relative paths) out of the store, as last
    // listed -- each only if it's still deleted here and unchanged online
    // since it was synced. Nothing on this computer is touched.
    LibrarySyncResult removeOnline(const QStringList &relPaths);

    // Remembers that these photos, deleted here, stay online: they're no
    // longer offered for removal until they change online.
    bool keepOnline(const QStringList &relPaths);

    // Above this share of the photos synced here gone at once (and at least
    // kSuspiciousMinimum of them), deletions look like a mistake.
    static constexpr int kSuspiciousPercent = 20;
    static constexpr int kSuspiciousMinimum = 20;

    // Whether a relative path is one LibrarySync handles (an image or RAW,
    // not in a hidden folder, no ".." tricks).
    static bool isSyncable(const QString &relPath);

    // "dir/name (from Alice).nef", or "... 2" etc. if that's taken.
    QString conflictPath(const QString &relPath, const QString &who) const;

private:
    struct LocalFile
    {
        QString relPath;
        qint64 size = 0;
        qint64 mtime = 0;
        QString md5;
        QString sha256;
    };

    // One upload, as done by a worker: what was sent, or why not.
    struct Sent
    {
        LocalFile file;
        std::optional<RemoteEntry> entry;
        QString why;
    };
    // One download, likewise.
    struct Fetched
    {
        LocalFile file;
        bool ok = false;
        QString why;
    };

    QString absPath(const QString &relPath) const;
    QList<LocalFile> walk();
    static bool hashFile(const QString &path, QString *md5, QString *sha256);
    std::optional<LocalFile> describeLocal(const QString &relPath) const;
    bool sameContent(const LocalFile &local, const RemoteEntry &remote) const;
    bool moveToTrash(const QString &relPath, QString *why) const;
    // Safe on worker threads: they touch only the files and `store`.
    Fetched fetchWith(LibraryStore &store, const RemoteEntry &remote, const QString &relPath) const;
    Sent sendWith(LibraryStore &store, const LocalFile &known, const QString &who) const;
    // Removes `entries` from the store (in parallel), forgetting each one
    // removed; failures are added to `result`.
    void removeEntries(const QList<RemoteEntry> &entries, LibrarySyncResult &result);
    // Records a finished upload (on the calling thread).
    void recordSent(const Sent &sent);
    bool send(const QString &relPath, const QString &who, QString *why);
    bool isCancelled() const { return m_cancel && m_cancel(); }
    void report(const QString &phase, int done, int total);

    LibraryStore &m_store;
    LibraryState &m_state;
    QString m_root;
    Progress m_progress;
    CancelFn m_cancel;
    QString m_error;

    // Filled by plan(), used by run().
    QHash<QString, LocalFile> m_local;
    QHash<QString, SyncedFile> m_base;
    QString m_trashStamp;
};

} // namespace pl::libsync
