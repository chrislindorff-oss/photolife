#include <QtTest>

#include <QCryptographicHash>
#include <QTemporaryDir>

#include "libsync/LibraryState.h"
#include "libsync/LibraryStore.h"
#include "libsync/LibrarySync.h"
#include "libsync/ThumbnailSync.h"

#include <QDirIterator>

using namespace pl::libsync;

namespace {

QString md5Of(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
}

QString shaOf(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

// A store held in memory. Versions count up like a real store's ETags would
// change; `md5InListing` false makes list() leave hashes out (as a multipart
// S3 upload or a store without them would), so describe() must be used.
class FakeStore : public LibraryStore
{
public:
    struct Object
    {
        QByteArray data;
        QString version;
        QString uploadedBy;
    };

    QMap<QString, Object> objects;
    bool md5InListing = true;
    int describes = 0;
    int uploads = 0;
    int downloads = 0;
    QSet<QString> corrupt;      // downloads of these come back damaged
    QSet<QString> failUpload;   // uploads of these fail
    std::function<bool()> abortUpload;   // true: this upload is cut off (as by cancelling)

    void put(const QString &rel, const QByteArray &data, const QString &by = QStringLiteral("Bob"))
    {
        objects[rel] = Object{data, QStringLiteral("v%1").arg(++m_counter), by};
    }

    QString error() const override { return m_error; }
    bool testConnection() override { return true; }

    std::optional<QList<RemoteEntry>> list() override
    {
        QList<RemoteEntry> out;
        for (auto it = objects.cbegin(); it != objects.cend(); ++it)
            out << entry(it.key(), md5InListing);
        return out;
    }

    bool describe(RemoteEntry &e) override
    {
        ++describes;
        if (!objects.contains(e.relPath)) {
            m_error = QStringLiteral("missing");
            return false;
        }
        const Object &o = objects[e.relPath];
        e.version = o.version;
        e.sha256 = shaOf(o.data);
        e.uploadedBy = o.uploadedBy;
        return true;
    }

    bool download(const RemoteEntry &e, const QString &localPath, const ByteProgress & = {}) override
    {
        ++downloads;
        QFile f(localPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        QByteArray data = objects.value(e.relPath).data;
        if (corrupt.contains(e.relPath))
            data += "garbage";
        f.write(data);
        return true;
    }

    std::optional<RemoteEntry> upload(const QString &localPath, const QString &relPath,
                                      const QString &md5, const QString &, const QString &who,
                                      const ByteProgress & = {}) override
    {
        if (failUpload.contains(relPath)) {
            m_error = QStringLiteral("refused");
            return std::nullopt;
        }
        if (abortUpload && abortUpload()) {
            m_error = QStringLiteral("Cancelled");
            return std::nullopt;
        }
        ++uploads;
        QFile f(localPath);
        f.open(QIODevice::ReadOnly);
        const QByteArray data = f.readAll();
        if (md5Of(data) != md5) {
            m_error = QStringLiteral("md5 mismatch");
            return std::nullopt;
        }
        put(relPath, data, who);
        return entry(relPath, true);
    }

    QStringList trashed;        // removed, in order
    QSet<QString> failRemove;   // removals of these fail

    bool remove(const RemoteEntry &e) override
    {
        if (failRemove.contains(e.relPath) || !objects.contains(e.relPath)) {
            m_error = QStringLiteral("can't remove");
            return false;
        }
        objects.remove(e.relPath);
        trashed << e.relPath;
        return true;
    }

    QMap<QString, QByteArray> thumbs;

    std::optional<QSet<QString>> listThumbnails() override
    {
        const QStringList names = thumbs.keys();
        return QSet<QString>(names.cbegin(), names.cend());
    }
    bool putThumbnail(const QString &name, const QByteArray &jpeg) override
    {
        thumbs[name] = jpeg;
        return true;
    }
    std::optional<QByteArray> getThumbnail(const QString &name) override
    {
        if (!thumbs.contains(name))
            return std::nullopt;
        return thumbs.value(name);
    }

private:
    RemoteEntry entry(const QString &rel, bool withMd5) const
    {
        const Object &o = objects[rel];
        RemoteEntry e;
        e.relPath = rel;
        e.remoteId = rel;
        e.size = o.data.size();
        e.version = o.version;
        if (withMd5)
            e.md5 = md5Of(o.data);
        return e;
    }

    int m_counter = 0;
    QString m_error;
};

// One collaborator's computer: a library folder and its sync state.
struct Machine
{
    QTemporaryDir dir;
    LibraryState state{QStringLiteral(":memory:")};
    QString who;

    explicit Machine(QString name) : who(std::move(name)) { state.open(); }

    QString path(const QString &rel) const { return dir.path() + QLatin1Char('/') + rel; }

    void write(const QString &rel, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path(rel)).path());
        QFile f(path(rel));
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(data);
        f.close();
        // A new mtime even within the same millisecond as the last write.
        static qint64 bump = 0;
        f.open(QIODevice::ReadWrite);
        f.setFileTime(QDateTime::currentDateTime().addSecs(++bump), QFileDevice::FileModificationTime);
    }

    QByteArray read(const QString &rel) const
    {
        QFile f(path(rel));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    bool has(const QString &rel) const { return QFileInfo::exists(path(rel)); }

    LibrarySyncResult sync(LibraryStore &store, LibrarySyncOptions options = {})
    {
        options.who = who;
        LibrarySync engine(store, state, dir.path());
        const auto result = engine.run(options);
        if (!result)
            qWarning() << "sync failed:" << engine.error();
        return result.value_or(LibrarySyncResult());
    }
};

const QString kOrchid = QStringLiteral("Orchids/Caladenia/IMG_0001.jpg");
const QString kRaw = QStringLiteral("Orchids/Caladenia/IMG_0001.nef");

} // namespace

class TestLibrarySync : public QObject
{
    Q_OBJECT

private slots:
    void syncable()
    {
        QVERIFY(LibrarySync::isSyncable(kOrchid));
        QVERIFY(LibrarySync::isSyncable(QStringLiteral("a.NEF")));
        QVERIFY(!LibrarySync::isSyncable(QStringLiteral("notes.txt")));
        QVERIFY(!LibrarySync::isSyncable(QStringLiteral("../outside.jpg")));
        QVERIFY(!LibrarySync::isSyncable(QStringLiteral("a/../../b.jpg")));
        QVERIFY(!LibrarySync::isSyncable(QStringLiteral("/etc/x.jpg")));
        QVERIFY(!LibrarySync::isSyncable(QStringLiteral(".photolife-trash/2026-10-08/x.jpg")));
        QVERIFY(!LibrarySync::isSyncable(QStringLiteral("a/.hidden/x.jpg")));
    }

    void uploadsThenDownloadsToAnotherMachine()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        Machine bob(QStringLiteral("Bob"));
        alice.write(kOrchid, "jpeg-bytes");
        alice.write(kRaw, "raw-bytes");
        alice.write(QStringLiteral("Orchids/notes.txt"), "ignored");

        const auto up = alice.sync(store);
        QCOMPARE(up.uploaded, 2);
        QVERIFY(up.failures.isEmpty());
        QCOMPARE(store.objects.size(), 2);
        QCOMPARE(store.objects.value(kOrchid).uploadedBy, QStringLiteral("Alice"));

        const auto down = bob.sync(store);
        QCOMPARE(down.downloaded, 2);
        QCOMPARE(bob.read(kOrchid), QByteArray("jpeg-bytes"));
        QCOMPARE(bob.read(kRaw), QByteArray("raw-bytes"));
        QVERIFY(!bob.has(QStringLiteral("Orchids/notes.txt")));

        // Nothing left to do on either side.
        QVERIFY(alice.sync(store).plan.nothingToDo());
        QVERIFY(bob.sync(store).plan.nothingToDo());
        QCOMPARE(store.uploads, 2);
        QCOMPARE(store.downloads, 2);
    }

    // Moving off rclone: both sides already hold the same files.
    void existingCopiesTransferNothing()
    {
        FakeStore store;
        store.put(kOrchid, "same");
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "same");

        const auto r = alice.sync(store);
        QCOMPARE(r.plan.inSync, 1);
        QCOMPARE(r.uploaded, 0);
        QCOMPARE(r.downloaded, 0);
    }

    void changedHereIsUploaded()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "v1");
        alice.sync(store);
        alice.write(kOrchid, "v2 edited");
        const auto r = alice.sync(store);
        QCOMPARE(r.uploaded, 1);
        QCOMPARE(store.objects.value(kOrchid).data, QByteArray("v2 edited"));
    }

    void changedOnlineIsDownloadedAndOldKept()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "v1");
        alice.sync(store);
        store.put(kOrchid, "v2 from bob");
        const auto r = alice.sync(store);
        QCOMPARE(r.downloaded, 1);
        QCOMPARE(alice.read(kOrchid), QByteArray("v2 from bob"));
        // The replaced copy went to the trash folder, not away.
        QDirIterator it(alice.path(QLatin1String(LibrarySync::kTrashDir)), QDir::Files,
                        QDirIterator::Subdirectories);
        QVERIFY(it.hasNext());
    }

    void conflictKeepsBoth()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "v1");
        alice.sync(store);
        alice.write(kOrchid, "alice's edit");
        store.put(kOrchid, "bob's edit", QStringLiteral("Bob"));

        const auto r = alice.sync(store);
        QCOMPARE(r.conflicts, 1);
        QVERIFY(r.failures.isEmpty());
        const QString copy = QStringLiteral("Orchids/Caladenia/IMG_0001 (from Bob).jpg");
        QCOMPARE(alice.read(kOrchid), QByteArray("alice's edit"));
        QCOMPARE(alice.read(copy), QByteArray("bob's edit"));
        QCOMPARE(store.objects.value(kOrchid).data, QByteArray("alice's edit"));
        QCOMPARE(store.objects.value(copy).data, QByteArray("bob's edit"));
        QVERIFY(alice.sync(store).plan.nothingToDo());
    }

    void deletedOnlineGoesToTrash()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "v1");
        alice.sync(store);
        store.objects.remove(kOrchid);

        const auto r = alice.sync(store);
        QCOMPARE(r.trashed, 1);
        QVERIFY(!alice.has(kOrchid));
        QVERIFY(QFileInfo::exists(alice.path(QStringLiteral(".photolife-trash/")
                                             + QDate::currentDate().toString(Qt::ISODate)
                                             + QLatin1Char('/') + kOrchid)));
        QCOMPARE(r.uploaded, 0);
    }

    void deletedHereStaysOnlineAndIsNotFetchedBack()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "v1");
        alice.write(QStringLiteral("other.jpg"), "other");   // not an empty folder (see vanishedLibrary...)
        alice.sync(store);
        QFile::remove(alice.path(kOrchid));

        const auto r = alice.sync(store);
        QCOMPARE(r.plan.removedHere.size(), 1);
        QVERIFY(store.objects.contains(kOrchid));
        QVERIFY(!alice.has(kOrchid));

        const auto again = alice.sync(store);
        QCOMPARE(again.downloaded, 0);
        QCOMPARE(again.plan.onlineOnly, 1);

        // ...until someone changes it online.
        store.put(kOrchid, "v2");
        QCOMPARE(alice.sync(store).downloaded, 1);
    }

    // Deleted here: offered for removal online, removed only when asked, and
    // then moved aside -- never deleted -- on collaborators' computers.
    void deletedHereCanBeRemovedOnline()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        Machine bob(QStringLiteral("Bob"));
        alice.write(kOrchid, "orchid");
        alice.write(QStringLiteral("a.jpg"), "a");
        alice.write(QStringLiteral("b.jpg"), "b");
        QCOMPARE(alice.sync(store).uploaded, 3);
        QCOMPARE(bob.sync(store).downloaded, 3);

        QVERIFY(QFile::remove(alice.path(kOrchid)));
        const LibrarySyncResult r = alice.sync(store);
        QCOMPARE(r.plan.deletedHere.size(), 1);
        QCOMPARE(r.plan.deletedHere.first().relPath, kOrchid);
        QVERIFY(!r.plan.deletionsLookSuspicious);
        QVERIFY(store.objects.contains(kOrchid));   // asked, not done
        // ...and still offered at the next sync.
        QCOMPARE(alice.sync(store).plan.deletedHere.size(), 1);

        LibrarySync engine(store, alice.state, alice.dir.path());
        const LibrarySyncResult removed = engine.removeOnline({kOrchid});
        QVERIFY2(removed.failures.isEmpty(), qPrintable(removed.failures.join(QLatin1Char('\n'))));
        QCOMPARE(removed.removedOnline, 1);
        QCOMPARE(store.trashed, QStringList{kOrchid});
        QVERIFY(alice.has(QStringLiteral("a.jpg")) && alice.has(QStringLiteral("b.jpg")));
        const LibrarySyncResult after = alice.sync(store);
        QVERIFY(after.plan.nothingToDo());
        QVERIFY(after.plan.deletedHere.isEmpty());

        // Bob's copy is moved into the trash folder, not deleted.
        QCOMPARE(bob.sync(store).trashed, 1);
        QVERIFY(!bob.has(kOrchid));
        QDirIterator it(bob.path(QLatin1String(LibrarySync::kTrashDir)), QDir::Files,
                        QDirIterator::Subdirectories);
        QVERIFY(it.hasNext());
        QFile kept(it.next());
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("orchid"));
    }

    void removeDeletedHereOptionRemovesThemDuringTheSync()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        for (int i = 0; i < 10; ++i)
            alice.write(QStringLiteral("p%1.jpg").arg(i), QByteArray::number(i));
        alice.sync(store);
        QVERIFY(QFile::remove(alice.path(QStringLiteral("p3.jpg"))));
        LibrarySyncOptions options;
        options.removeDeletedHere = true;
        const LibrarySyncResult r = alice.sync(store, options);
        QCOMPARE(r.removedOnline, 1);
        QCOMPARE(store.trashed, QStringList{QStringLiteral("p3.jpg")});
        QCOMPARE(store.objects.size(), 9);
        QVERIFY(alice.sync(store, options).plan.nothingToDo());
    }

    void keptOnlineIsNoLongerOffered()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        alice.write(QStringLiteral("a.jpg"), "a");
        alice.write(QStringLiteral("b.jpg"), "b");
        alice.sync(store);
        QFile::remove(alice.path(QStringLiteral("a.jpg")));
        QCOMPARE(alice.sync(store).plan.deletedHere.size(), 1);
        LibrarySync engine(store, alice.state, alice.dir.path());
        QVERIFY(engine.keepOnline({QStringLiteral("a.jpg")}));
        const LibrarySyncResult r = alice.sync(store);
        QVERIFY(r.plan.deletedHere.isEmpty());
        QCOMPARE(r.plan.keptOnline.size(), 1);   // still there for a review
        LibrarySyncOptions options;
        options.removeDeletedHere = true;
        QCOMPARE(alice.sync(store, options).removedOnline, 0);
        QVERIFY(store.objects.contains(QStringLiteral("a.jpg")));
    }

    // Copies freed up for space, and photos never downloaded, weren't
    // deleted here: they're never offered.
    void freedUpAndNeverFetchedAreNotDeletions()
    {
        FakeStore store;
        store.put(QStringLiteral("keep.jpg"), "keep");
        store.put(QStringLiteral("free.jpg"), "free");
        store.put(QStringLiteral("never.jpg"), "never");
        Machine bob(QStringLiteral("Bob"));
        LibrarySyncOptions first;
        first.wanted = [](const QString &rel) { return rel != QLatin1String("never.jpg"); };
        QCOMPARE(bob.sync(store, first).downloaded, 2);

        LibrarySyncOptions freeUp;
        freeUp.upload = freeUp.download = false;
        freeUp.freeUp = true;
        freeUp.wanted = [](const QString &rel) { return rel == QLatin1String("keep.jpg"); };
        QCOMPARE(bob.sync(store, freeUp).freed, 1);

        LibrarySyncOptions options;
        options.wanted = freeUp.wanted;
        options.removeDeletedHere = true;
        const LibrarySyncResult r = bob.sync(store, options);
        QVERIFY(r.plan.deletedHere.isEmpty());
        QCOMPARE(r.removedOnline, 0);
        QCOMPARE(store.objects.size(), 3);
    }

    // Many photos gone at once looks like a drive not connected: nothing is
    // removed online; and a library folder with nothing in it stops the sync.
    void vanishedLibraryIsNotTakenAsDeletions()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        for (int i = 0; i < 40; ++i)
            alice.write(QStringLiteral("Frogs/p%1.jpg").arg(i), QByteArray::number(i));
        alice.write(QStringLiteral("Birds/robin.jpg"), "robin");
        alice.sync(store);

        QVERIFY(QDir(alice.path(QStringLiteral("Frogs"))).removeRecursively());
        LibrarySyncOptions options;
        options.removeDeletedHere = true;
        const LibrarySyncResult r = alice.sync(store, options);
        QCOMPARE(r.plan.deletedHere.size(), 40);
        QVERIFY(r.plan.deletionsLookSuspicious);
        QCOMPARE(r.removedOnline, 0);
        QVERIFY(!r.failures.isEmpty());
        QCOMPARE(store.objects.size(), 41);
        // Still suspicious next time: earlier deletions count too.
        QVERIFY(alice.sync(store, options).plan.deletionsLookSuspicious);
        QCOMPARE(store.objects.size(), 41);

        QVERIFY(QFile::remove(alice.path(QStringLiteral("Birds/robin.jpg"))));
        LibrarySync engine(store, alice.state, alice.dir.path());
        QVERIFY(!engine.run(options));
        QVERIFY2(engine.error().contains(QLatin1String("drive connected")), qPrintable(engine.error()));
        QCOMPARE(store.objects.size(), 41);
    }

    void removalRefusesWhatChangedOrCameBack()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        alice.write(QStringLiteral("a.jpg"), "a");
        alice.write(QStringLiteral("b.jpg"), "b");
        alice.write(QStringLiteral("c.jpg"), "c");
        alice.sync(store);
        QFile::remove(alice.path(QStringLiteral("a.jpg")));
        QFile::remove(alice.path(QStringLiteral("b.jpg")));
        alice.sync(store);
        store.put(QStringLiteral("a.jpg"), "a, edited by Bob");   // changed online since
        alice.write(QStringLiteral("b.jpg"), "b");                // back here
        LibrarySync engine(store, alice.state, alice.dir.path());
        const LibrarySyncResult r = engine.removeOnline({QStringLiteral("a.jpg"), QStringLiteral("b.jpg")});
        QCOMPARE(r.removedOnline, 0);
        QCOMPARE(r.failures.size(), 2);
        QVERIFY(store.trashed.isEmpty());
    }

    void damagedDownloadIsRejected()
    {
        FakeStore store;
        store.put(kOrchid, "good");
        store.corrupt.insert(kOrchid);
        Machine bob(QStringLiteral("Bob"));

        const auto r = bob.sync(store);
        QCOMPARE(r.downloaded, 0);
        QCOMPARE(r.failures.size(), 1);
        QVERIFY(!bob.has(kOrchid));
        QVERIFY(!bob.has(kOrchid + QLatin1String(LibrarySync::kPartialSuffix)));

        store.corrupt.clear();
        QCOMPARE(bob.sync(store).downloaded, 1);
    }

    void failedUploadIsRetriedNextTime()
    {
        FakeStore store;
        store.failUpload.insert(kOrchid);
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "v1");
        QCOMPARE(alice.sync(store).failures.size(), 1);
        store.failUpload.clear();
        QCOMPARE(alice.sync(store).uploaded, 1);
    }

    void leftoverPartialFilesAreCleanedUp()
    {
        FakeStore store;
        Machine bob(QStringLiteral("Bob"));
        bob.write(kOrchid + QLatin1String(LibrarySync::kPartialSuffix), "half");
        bob.sync(store);
        QVERIFY(!bob.has(kOrchid + QLatin1String(LibrarySync::kPartialSuffix)));
    }

    void hashesComeFromDescribeWhenListingLacksThem()
    {
        FakeStore store;
        store.md5InListing = false;
        store.put(kOrchid, "same");
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "same");

        QCOMPARE(alice.sync(store).plan.inSync, 1);
        QCOMPARE(store.describes, 1);
        // Known version next time: no more lookups.
        alice.sync(store);
        QCOMPARE(store.describes, 1);
    }

    void unchangedFilesAreNotRehashed()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        alice.write(kOrchid, "v1");
        alice.sync(store);
        // Change the content but keep size and mtime: trusted as unchanged.
        const QDateTime mtime = QFileInfo(alice.path(kOrchid)).lastModified();
        {
            QFile f(alice.path(kOrchid));
            f.open(QIODevice::ReadWrite);
            f.write("v2");
            f.flush();
            f.setFileTime(mtime, QFileDevice::FileModificationTime);
        }
        QCOMPARE(alice.sync(store).uploaded, 0);
    }

    void wantedLimitsNewDownloadsOnly()
    {
        FakeStore store;
        store.put(kOrchid, "orchid");
        store.put(QStringLiteral("Birds/robin.jpg"), "robin");
        Machine bob(QStringLiteral("Bob"));

        LibrarySyncOptions options;
        options.wanted = [](const QString &rel) { return rel.startsWith(QLatin1String("Orchids/")); };
        const auto r = bob.sync(store, options);
        QCOMPARE(r.downloaded, 1);
        QCOMPARE(r.plan.notWanted, 1);
        QVERIFY(!bob.has(QStringLiteral("Birds/robin.jpg")));

        // An update to a photo already here comes down whatever `wanted` says.
        store.put(kOrchid, "orchid v2");
        options.wanted = [](const QString &) { return false; };
        QCOMPARE(bob.sync(store, options).downloaded, 1);
    }

    void uploadOnlyLeavesDownloadsAndConflicts()
    {
        FakeStore store;
        store.put(kOrchid, "theirs");
        Machine alice(QStringLiteral("Alice"));
        alice.write(kRaw, "mine");

        LibrarySyncOptions options;
        options.download = false;
        const auto r = alice.sync(store, options);
        QCOMPARE(r.uploaded, 1);
        QCOMPARE(r.downloaded, 0);
        QVERIFY(!alice.has(kOrchid));
    }

    void cancelStopsBetweenFiles()
    {
        FakeStore store;
        for (int i = 0; i < 5; ++i)
            store.put(QStringLiteral("p%1.jpg").arg(i), QByteArray::number(i));
        Machine bob(QStringLiteral("Bob"));

        LibrarySync engine(store, bob.state, bob.dir.path());
        int downloads = 0;
        engine.setProgress([&](const QString &phase, int, int) {
            if (phase == QLatin1String("Downloading photos"))
                ++downloads;
        });
        engine.setCancel([&] { return downloads >= 2; });
        const auto r = engine.run();
        QVERIFY(r);
        QVERIFY(r->cancelled);
        QVERIFY(r->downloaded < 5);

        // The next run carries on with the rest.
        LibrarySync again(store, bob.state, bob.dir.path());
        const auto rest = again.run();
        QCOMPARE(r->downloaded + rest->downloaded, 5);
    }
    // Stopping an upload part-way, even in the middle of a file, isn't a
    // failure, and the next sync carries on with only what's left.
    void stoppedUploadCarriesOnLater()
    {
        FakeStore store;
        Machine alice(QStringLiteral("Alice"));
        for (int i = 0; i < 6; ++i)
            alice.write(QStringLiteral("p%1.jpg").arg(i), QByteArray::number(i));

        bool cancel = false;
        int started = 0;
        store.abortUpload = [&] {
            if (++started == 3)
                cancel = true;   // pressed Stop while the third was going up
            return cancel;
        };
        LibrarySync engine(store, alice.state, alice.dir.path());
        engine.setCancel([&] { return cancel; });
        const auto first = engine.run();
        QVERIFY(first);
        QVERIFY(first->cancelled);
        QVERIFY(first->failures.isEmpty());
        QCOMPARE(first->uploaded, 2);

        store.abortUpload = {};
        const auto rest = alice.sync(store);
        QVERIFY(rest.failures.isEmpty());
        QCOMPARE(rest.uploaded, 4);
        QCOMPARE(store.uploads, 6);   // nothing sent twice
        QCOMPARE(store.objects.size(), 6);
    }

    void freeUpRemovesOnlySafeUnwantedCopies()
    {
        FakeStore store;
        Machine bob(QStringLiteral("Bob"));
        bob.write(kOrchid, "orchid");
        bob.write(QStringLiteral("Birds/robin.jpg"), "robin");
        bob.sync(store);

        LibrarySyncOptions options;
        options.wanted = [](const QString &rel) { return rel.startsWith(QLatin1String("Orchids/")); };
        const auto check = [&] {
            LibrarySync engine(store, bob.state, bob.dir.path());
            return *engine.plan(options);
        }();
        QCOMPARE(check.freeable, QStringList{QStringLiteral("Birds/robin.jpg")});
        QCOMPARE(check.freeableBytes, qint64(5));

        options.freeUp = true;
        const auto r = bob.sync(store, options);
        QCOMPARE(r.freed, 1);
        QVERIFY(!bob.has(QStringLiteral("Birds/robin.jpg")));
        QVERIFY(bob.has(kOrchid));
        // ...by moving it to the trash folder, never deleting it.
        QDirIterator trash(bob.path(QLatin1String(LibrarySync::kTrashDir)), QDir::Files,
                           QDirIterator::Subdirectories);
        QVERIFY(trash.hasNext());
        QFile moved(trash.next());
        QVERIFY(moved.fileName().endsWith(QLatin1String("Birds/robin.jpg")));
        QVERIFY(moved.open(QIODevice::ReadOnly));
        QCOMPARE(moved.readAll(), QByteArray("robin"));
        QVERIFY(store.objects.contains(QStringLiteral("Birds/robin.jpg")));

        // Not "deleted here": wanted again, it comes back.
        options.freeUp = false;
        options.wanted = {};
        QCOMPARE(bob.sync(store, options).downloaded, 1);
        QVERIFY(bob.has(QStringLiteral("Birds/robin.jpg")));
    }

    void freeUpKeepsWhatIsNotSafelyOnline()
    {
        FakeStore store;
        Machine bob(QStringLiteral("Bob"));
        bob.write(QStringLiteral("Birds/robin.jpg"), "robin");
        bob.write(QStringLiteral("Birds/wren.jpg"), "wren");
        bob.sync(store);
        bob.write(QStringLiteral("Birds/robin.jpg"), "robin edited");   // changed here
        store.put(QStringLiteral("Birds/wren.jpg"), "wren v2");         // changed online
        bob.write(QStringLiteral("Birds/new.jpg"), "new");             // never uploaded

        LibrarySyncOptions options;
        options.upload = false;
        options.download = false;
        options.freeUp = true;
        options.wanted = [](const QString &) { return false; };
        const auto r = bob.sync(store, options);
        QCOMPARE(r.freed, 0);
        QVERIFY(bob.has(QStringLiteral("Birds/robin.jpg")));
        QVERIFY(bob.has(QStringLiteral("Birds/wren.jpg")));
        QVERIFY(bob.has(QStringLiteral("Birds/new.jpg")));
    }

    void fetchDownloadsAndPins()
    {
        FakeStore store;
        store.put(kOrchid, "orchid");
        store.put(QStringLiteral("Birds/robin.jpg"), "robin");
        Machine bob(QStringLiteral("Bob"));

        LibrarySyncOptions none;
        none.wanted = [](const QString &) { return false; };
        QCOMPARE(bob.sync(store, none).plan.notWanted, 2);   // also records the listing

        LibrarySync engine(store, bob.state, bob.dir.path());
        const auto r = engine.fetch({kOrchid, QStringLiteral("Nope/missing.jpg")});
        QCOMPARE(r.downloaded, 1);
        QCOMPARE(r.failures.size(), 1);
        QCOMPARE(bob.read(kOrchid), QByteArray("orchid"));
        QVERIFY(bob.state.pinned().contains(kOrchid));
        QCOMPARE(bob.state.remote().size(), 2);

        // Already here: nothing to do.
        QCOMPARE(engine.fetch({kOrchid}).downloaded, 0);
    }

    void thumbnailsGoUpAndComeDownForOnlineOnlyPhotos()
    {
        FakeStore store;
        QTemporaryDir aliceCache;
        QTemporaryDir bobCache;
        Machine alice(QStringLiteral("Alice"));
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        QVERIFY(image.save(alice.path(QStringLiteral("robin.jpg"))));
        const QByteArray data = alice.read(QStringLiteral("robin.jpg"));
        const QString sha = shaOf(data);

        // Recorded as bigger than a thumbnail: it gets one online.
        const qint64 big = ThumbnailSync::kOwnThumbnailBytes + 1;
        const QString robin = alice.path(QStringLiteral("robin.jpg"));
        ThumbnailSync up(store, aliceCache.path());
        const auto sent = up.run({{robin, sha, big}}, {});
        QVERIFY(sent);
        QCOMPARE(sent->uploaded, 1);
        QVERIFY(store.thumbs.contains(ThumbnailSync::nameFor(sha)));
        // Kept in Alice's cache too, and not uploaded twice.
        QVERIFY(QFileInfo::exists(pl::thumb::ThumbnailCache::diskPathFor(aliceCache.path(), sha, ThumbnailSync::kSize)));
        QCOMPARE(up.run({{robin, sha, big}}, {})->uploaded, 0);

        ThumbnailSync down(store, bobCache.path());
        const auto got = down.run({}, {{sha, {}}, {QStringLiteral("no-such-hash"), {}}});
        QVERIFY(got);
        QCOMPARE(got->downloaded, 1);
        QCOMPARE(store.downloads, 0);   // the thumbnail, not the photo
        QImage thumb(pl::thumb::ThumbnailCache::diskPathFor(bobCache.path(), sha, ThumbnailSync::kSize));
        QCOMPARE(thumb.size(), QSize(64, 48));
    }

    // A photo about the size of its thumbnail gets none online: the
    // computers without it make one from the photo itself.
    void smallPhotosAreTheirOwnThumbnails()
    {
        FakeStore store;
        QTemporaryDir aliceCache;
        QTemporaryDir bobCache;
        Machine alice(QStringLiteral("Alice"));
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(Qt::darkRed);
        QVERIFY(image.save(alice.path(QStringLiteral("wren.jpg"))));
        const QByteArray data = alice.read(QStringLiteral("wren.jpg"));
        const QString sha = shaOf(data);
        QVERIFY(data.size() <= ThumbnailSync::kOwnThumbnailBytes);

        ThumbnailSync up(store, aliceCache.path());
        const auto sent = up.run({{alice.path(QStringLiteral("wren.jpg")), sha, data.size()}}, {});
        QVERIFY(sent);
        QCOMPARE(sent->uploaded, 0);
        QVERIFY(store.thumbs.isEmpty());

        store.put(QStringLiteral("Birds/wren.jpg"), data, QStringLiteral("Alice"));
        RemoteEntry photo = store.list()->first();
        ThumbnailSync down(store, bobCache.path());
        const auto got = down.run({}, {{sha, photo}});
        QVERIFY(got);
        QVERIFY2(got->failures.isEmpty(), qPrintable(got->failures.join(QLatin1Char('\n'))));
        QCOMPARE(got->downloaded, 1);
        QCOMPARE(store.downloads, 1);
        QImage thumb(pl::thumb::ThumbnailCache::diskPathFor(bobCache.path(), sha, ThumbnailSync::kSize));
        QCOMPARE(thumb.size(), QSize(64, 48));
        // Nothing left behind but the cached thumbnail.
        QCOMPARE(QDir(bobCache.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestLibrarySync)
#include "tst_librarysync.moc"
