#pragma once

#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace pl::libsync {

// A photo as the remote store holds it.
struct RemoteEntry
{
    QString relPath;     // under the library root, forward slashes
    qint64 size = 0;
    QString version;     // changes whenever the content does (S3 ETag, Drive version)
    QString md5;         // hex; empty when the store's listing doesn't say
    QString sha256;      // hex; empty until describe() fills it, if the store keeps it
    QString uploadedBy;  // who put this version there, when known
    QString remoteId;    // the store's own handle (S3: key; Drive: file id)
};

// Where a shared photo library lives online: an S3-compatible bucket, a Google
// Drive folder ... LibrarySync talks to it through this. Every call blocks, so
// use one from a worker thread; one instance per thread.
class LibraryStore
{
public:
    using ByteProgress = std::function<void(qint64 done, qint64 total)>;
    using CancelFn = std::function<bool()>;

    virtual ~LibraryStore() = default;

    // What went wrong with the last failed call.
    virtual QString error() const = 0;

    void setCancel(CancelFn cancel) { m_cancel = std::move(cancel); }

    // Checks the store can be reached and listed with these settings.
    virtual bool testConnection() = 0;

    // Every photo in the store. nullopt on failure.
    virtual std::optional<QList<RemoteEntry>> list() = 0;

    // Fills in what list() left out (sha256, md5, uploadedBy) from the
    // store's per-file metadata. False on failure.
    virtual bool describe(RemoteEntry &entry) = 0;

    // Writes `entry`'s content to `localPath` (replacing it).
    virtual bool download(const RemoteEntry &entry, const QString &localPath,
                          const ByteProgress &progress = {}) = 0;

    // Stores the file at `localPath` as `relPath` (replacing any file there),
    // returning the new entry. `md5`/`sha256` are the file's hex digests.
    virtual std::optional<RemoteEntry> upload(const QString &localPath, const QString &relPath,
                                              const QString &md5, const QString &sha256,
                                              const QString &uploadedBy,
                                              const ByteProgress &progress = {}) = 0;

    // Parallel transfers: how many workers to run, each with its own clone()
    // -- a copy that already knows what this one has listed (file ids,
    // folders, the sign-in), to use on another thread. A store that can't be
    // cloned returns nullptr and transfers one at a time.
    virtual int parallelTransfers() const { return 1; }
    virtual std::unique_ptr<LibraryStore> clone() const { return nullptr; }

    // Before parallel uploads: makes whatever the uploads of `relPaths` (and
    // of thumbnails) would otherwise create on the fly -- folders -- so the
    // workers never race to create the same one, reporting how many of those
    // are done. False on failure.
    using CountProgress = std::function<void(int done, int total)>;
    virtual bool prepareUploads(const QStringList &relPaths, const CountProgress &progress = {})
    {
        Q_UNUSED(relPaths);
        Q_UNUSED(progress);
        return true;
    }
    virtual bool prepareThumbnails() { return true; }

    // Small JPEG thumbnails kept beside the photos, named "<sha256>_<px>",
    // so photos that are only online can still be browsed.
    virtual std::optional<QSet<QString>> listThumbnails() = 0;
    virtual bool putThumbnail(const QString &name, const QByteArray &jpeg) = 0;
    virtual std::optional<QByteArray> getThumbnail(const QString &name) = 0;

protected:
    bool cancelled() const { return m_cancel && m_cancel(); }
    CancelFn cancelFn() const { return m_cancel; }

private:
    CancelFn m_cancel;
};

} // namespace pl::libsync
