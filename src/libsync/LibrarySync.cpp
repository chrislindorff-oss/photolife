#include "libsync/LibrarySync.h"

#include "scan/FileScanner.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace pl::libsync {

namespace {

using scan::FileScanner;

qint64 mtimeOf(const QFileInfo &info)
{
    return info.lastModified().toMSecsSinceEpoch();
}

} // namespace

LibrarySync::LibrarySync(LibraryStore &store, LibraryState &state, QString libraryRoot)
    : m_store(store), m_state(state), m_root(QDir::cleanPath(QDir::fromNativeSeparators(libraryRoot)))
{
}

void LibrarySync::setCancel(CancelFn cancel)
{
    m_cancel = cancel;
    m_store.setCancel(std::move(cancel));
}

void LibrarySync::report(const QString &phase, int done, int total)
{
    if (m_progress)
        m_progress(phase, done, total);
}

QString LibrarySync::absPath(const QString &relPath) const
{
    return m_root + QLatin1Char('/') + relPath;
}

bool LibrarySync::isSyncable(const QString &relPath)
{
    if (relPath.isEmpty() || relPath.startsWith(QLatin1Char('/')) || relPath.contains(QLatin1Char('\\'))
        || relPath.contains(QLatin1Char(':')))
        return false;
    const QStringList parts = relPath.split(QLatin1Char('/'));
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QString &dir = parts.at(i);
        if (dir.isEmpty() || dir == QLatin1String("..") || FileScanner::isIgnoredDir(dir))
            return false;
    }
    const QString &name = parts.constLast();
    if (FileScanner::isIgnoredFile(name))
        return false;
    const QString ext = QFileInfo(name).suffix().toLower();
    return FileScanner::imageExtensions().contains(ext) || FileScanner::rawExtensions().contains(ext);
}

QList<LibrarySync::LocalFile> LibrarySync::walk()
{
    QList<LocalFile> out;
    if (m_root.isEmpty() || !QDir(m_root).exists())
        return out;
    QStringList pending{m_root};
    QSet<QString> visited;
    while (!pending.isEmpty()) {
        const QString dirPath = pending.takeLast();
        const QString canonical = QFileInfo(dirPath).canonicalFilePath();
        if (visited.contains(canonical))
            continue;   // symlink cycle
        visited.insert(canonical);

        const QDir dir(dirPath);
        const QFileInfoList entries =
            dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo &info : entries) {
            if (info.isDir()) {
                if (!FileScanner::isIgnoredDir(info.fileName()))
                    pending.append(info.filePath());
                continue;
            }
            if (info.fileName().endsWith(QLatin1String(kPartialSuffix))) {
                QFile::remove(info.filePath());   // left by an interrupted download
                continue;
            }
            const QString rel = QDir(m_root).relativeFilePath(info.filePath());
            if (!isSyncable(rel))
                continue;
            LocalFile f;
            f.relPath = rel;
            f.size = info.size();
            f.mtime = mtimeOf(info);
            out.append(f);
        }
    }
    return out;
}

bool LibrarySync::hashFile(const QString &path, QString *md5, QString *sha256)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash m(QCryptographicHash::Md5);
    QCryptographicHash s(QCryptographicHash::Sha256);
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    for (;;) {
        const qint64 n = file.read(buffer.data(), buffer.size());
        if (n < 0)
            return false;
        if (n == 0)
            break;
        m.addData(QByteArrayView(buffer.constData(), n));
        s.addData(QByteArrayView(buffer.constData(), n));
    }
    *md5 = QString::fromLatin1(m.result().toHex());
    *sha256 = QString::fromLatin1(s.result().toHex());
    return true;
}

std::optional<LibrarySync::LocalFile> LibrarySync::describeLocal(const QString &relPath)
{
    const QFileInfo info(absPath(relPath));
    if (!info.isFile())
        return std::nullopt;
    LocalFile f;
    f.relPath = relPath;
    f.size = info.size();
    f.mtime = mtimeOf(info);
    if (!hashFile(info.filePath(), &f.md5, &f.sha256))
        return std::nullopt;
    return f;
}

bool LibrarySync::sameContent(const LocalFile &local, const RemoteEntry &remote) const
{
    if (!remote.md5.isEmpty())
        return local.md5 == remote.md5;
    if (!remote.sha256.isEmpty())
        return local.sha256 == remote.sha256;
    return false;
}

QString LibrarySync::conflictPath(const QString &relPath, const QString &who) const
{
    const int slash = relPath.lastIndexOf(QLatin1Char('/'));
    const QString dir = slash < 0 ? QString() : relPath.left(slash + 1);
    const QString name = relPath.mid(slash + 1);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot <= 0 ? name : name.left(dot);
    const QString ext = dot <= 0 ? QString() : name.mid(dot);
    const QString tag = who.trimmed().isEmpty() ? QStringLiteral("conflict")
                                                : QStringLiteral("from %1").arg(who.trimmed());
    for (int n = 1;; ++n) {
        const QString suffix = n == 1 ? QString() : QStringLiteral(" %1").arg(n);
        const QString candidate = dir + stem + QStringLiteral(" (") + tag + suffix
                                  + QLatin1Char(')') + ext;
        if (!QFileInfo::exists(absPath(candidate)) && !m_base.contains(candidate))
            return candidate;
    }
}

std::optional<LibraryPlan> LibrarySync::plan(const LibrarySyncOptions &options)
{
    m_error.clear();
    m_local.clear();
    m_base = m_state.load();

    // This computer: hash only what's new or changed since last time.
    report(QStringLiteral("Checking photos on this computer"), 0, 0);
    QList<LocalFile> local = walk();
    for (int i = 0; i < local.size(); ++i) {
        if (isCancelled()) {
            m_error = QStringLiteral("Cancelled");
            return std::nullopt;
        }
        LocalFile &f = local[i];
        const auto known = m_base.constFind(f.relPath);
        if (known != m_base.constEnd() && known->local && known->size == f.size
            && known->mtime == f.mtime) {
            f.md5 = known->md5;
            f.sha256 = known->sha256;
        } else if (!hashFile(absPath(f.relPath), &f.md5, &f.sha256)) {
            continue;   // unreadable just now; next time
        }
        m_local.insert(f.relPath, f);
        report(QStringLiteral("Checking photos on this computer"), i + 1, int(local.size()));
    }

    // The store.
    report(QStringLiteral("Checking photos online"), 0, 0);
    const auto listed = m_store.list();
    if (!listed) {
        m_error = m_store.error();
        return std::nullopt;
    }
    QHash<QString, RemoteEntry> remote;
    for (RemoteEntry entry : *listed) {
        if (!isSyncable(entry.relPath))
            continue;
        if (entry.md5.isEmpty()) {
            const auto known = m_base.constFind(entry.relPath);
            if (known != m_base.constEnd() && known->remoteVersion == entry.version) {
                entry.md5 = known->md5;
                entry.sha256 = known->sha256;
            } else if (!m_store.describe(entry)) {
                m_error = m_store.error();
                return std::nullopt;
            }
        }
        remote.insert(entry.relPath, entry);
    }
    m_state.replaceRemote(remote.values());

    LibraryPlan plan;
    QSet<QString> paths;
    for (auto it = m_local.cbegin(); it != m_local.cend(); ++it)
        paths.insert(it.key());
    for (auto it = remote.cbegin(); it != remote.cend(); ++it)
        paths.insert(it.key());
    for (auto it = m_base.cbegin(); it != m_base.cend(); ++it)
        paths.insert(it.key());

    QStringList sorted(paths.cbegin(), paths.cend());
    sorted.sort();

    m_state.begin();
    for (const QString &rel : std::as_const(sorted)) {
        const auto l = m_local.constFind(rel);
        const auto r = remote.constFind(rel);
        const auto b = m_base.constFind(rel);
        const bool hasL = l != m_local.constEnd();
        const bool hasR = r != remote.constEnd();
        const bool hasB = b != m_base.constEnd();

        if (hasL && hasR) {
            if (sameContent(*l, *r)) {
                ++plan.inSync;
                if (!hasB || !b->local || b->md5 != l->md5 || b->mtime != l->mtime
                    || b->size != l->size || b->remoteVersion != r->version) {
                    SyncedFile f{rel, l->md5, l->sha256, l->size, l->mtime, r->version, true};
                    m_state.put(f);
                    m_base.insert(rel, f);
                }
                if (options.wanted && !options.wanted(rel)) {
                    plan.freeable << rel;
                    plan.freeableBytes += l->size;
                }
            } else if (hasB && b->remoteVersion == r->version) {
                plan.toUpload << rel;               // only changed here
                plan.uploadBytes += l->size;
            } else if (hasB && b->local && b->md5 == l->md5) {
                plan.toDownload << *r;              // only changed online
                plan.downloadBytes += r->size;
            } else {
                plan.conflicts << *r;               // changed in both places
                plan.downloadBytes += r->size;
                plan.uploadBytes += l->size;
            }
        } else if (hasL) {
            if (hasB && b->local && b->md5 == l->md5) {
                plan.toTrash << rel;                // deleted online
            } else {
                plan.toUpload << rel;
                plan.uploadBytes += l->size;
            }
        } else if (hasR) {
            if (hasB && b->remoteVersion == r->version) {
                if (b->local)
                    plan.removedHere << rel;        // deleted here: keep it online only
                else
                    ++plan.onlineOnly;
            } else if (options.wanted && !options.wanted(rel)) {
                ++plan.notWanted;
            } else {
                plan.toDownload << *r;
                plan.downloadBytes += r->size;
            }
        } else if (hasB) {
            m_state.remove(rel);                    // gone from both
            m_base.remove(rel);
        }
    }
    m_state.commit();

    if (!options.upload) {
        plan.toUpload.clear();
        plan.uploadBytes = 0;
    }
    if (!options.download) {
        for (const RemoteEntry &r : std::as_const(plan.toDownload))
            plan.downloadBytes -= r.size;
        plan.toDownload.clear();
    }
    if (!options.upload || !options.download) {
        for (const RemoteEntry &r : std::as_const(plan.conflicts)) {
            if (options.download)
                plan.downloadBytes -= r.size;
            if (options.upload)
                plan.uploadBytes -= m_local.value(r.relPath).size;
        }
        plan.conflicts.clear();   // needs both directions
    }
    return plan;
}

bool LibrarySync::moveToTrash(const QString &relPath, QString *why)
{
    if (m_trashStamp.isEmpty())
        m_trashStamp = QDate::currentDate().toString(Qt::ISODate);
    const QString trashRoot = m_root + QLatin1Char('/') + QLatin1String(kTrashDir)
                              + QLatin1Char('/') + m_trashStamp;
    QString target = trashRoot + QLatin1Char('/') + relPath;
    for (int n = 2; QFileInfo::exists(target); ++n)
        target = trashRoot + QLatin1Char('/') + relPath + QStringLiteral(".%1").arg(n);
    if (!QDir().mkpath(QFileInfo(target).path()) || !QFile::rename(absPath(relPath), target)) {
        *why = QStringLiteral("couldn't move it to %1").arg(QLatin1String(kTrashDir));
        return false;
    }
    return true;
}

// Downloads `remote` to `relPath` (which may differ, for a conflict copy):
// into a partial file, checked against the store's hash, then into place --
// moving aside whatever was there.
bool LibrarySync::fetch(const RemoteEntry &remote, const QString &relPath, LocalFile *saved,
                        QString *why)
{
    const QString target = absPath(relPath);
    const QString partial = target + QLatin1String(kPartialSuffix);
    if (!QDir().mkpath(QFileInfo(target).path())) {
        *why = QStringLiteral("couldn't create its folder");
        return false;
    }
    if (!m_store.download(remote, partial)) {
        QFile::remove(partial);
        *why = m_store.error();
        return false;
    }
    QString md5;
    QString sha256;
    if (!hashFile(partial, &md5, &sha256)) {
        QFile::remove(partial);
        *why = QStringLiteral("couldn't read the downloaded file");
        return false;
    }
    if ((!remote.md5.isEmpty() && md5 != remote.md5)
        || (remote.md5.isEmpty() && !remote.sha256.isEmpty() && sha256 != remote.sha256)) {
        QFile::remove(partial);
        *why = QStringLiteral("the download was damaged (its checksum doesn't match)");
        return false;
    }
    if (QFileInfo::exists(target) && !moveToTrash(relPath, why)) {
        QFile::remove(partial);
        return false;
    }
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        *why = QStringLiteral("couldn't put the downloaded file in place");
        return false;
    }
    const QFileInfo info(target);
    *saved = LocalFile{relPath, info.size(), mtimeOf(info), md5, sha256};
    return true;
}

bool LibrarySync::send(const QString &relPath, const QString &who, QString *why)
{
    // Re-check: the file may have changed since it was hashed.
    LocalFile f = m_local.value(relPath);
    const QFileInfo info(absPath(relPath));
    if (!info.isFile()) {
        *why = QStringLiteral("it's gone from this computer");
        return false;
    }
    if (f.md5.isEmpty() || info.size() != f.size || mtimeOf(info) != f.mtime) {
        const auto fresh = describeLocal(relPath);
        if (!fresh) {
            *why = QStringLiteral("couldn't read it");
            return false;
        }
        f = *fresh;
    }
    const auto uploaded = m_store.upload(absPath(relPath), relPath, f.md5, f.sha256, who);
    if (!uploaded) {
        *why = m_store.error();
        return false;
    }
    const SyncedFile synced{relPath, f.md5, f.sha256, f.size, f.mtime, uploaded->version, true};
    m_state.put(synced);
    m_base.insert(relPath, synced);
    m_local.insert(relPath, f);
    return true;
}

std::optional<LibrarySyncResult> LibrarySync::run(const LibrarySyncOptions &options)
{
    const auto planned = plan(options);
    if (!planned)
        return std::nullopt;

    LibrarySyncResult result;
    result.plan = *planned;
    const LibraryPlan &p = result.plan;
    auto failed = [&](const QString &rel, const QString &why) {
        // A transfer cut short by cancelling isn't a failure: it's simply
        // done again by the next sync.
        if (isCancelled()) {
            result.cancelled = true;
            return;
        }
        result.failures << QStringLiteral("%1: %2").arg(rel, why);
    };
    auto stop = [&] {
        if (isCancelled())
            result.cancelled = true;
        return result.cancelled;
    };

    // Uploads first: collaborators can only fetch what's online.
    for (int i = 0; i < p.toUpload.size() && !stop(); ++i) {
        report(QStringLiteral("Uploading photos"), i, int(p.toUpload.size()));
        const QString &rel = p.toUpload.at(i);
        QString why;
        if (send(rel, options.who, &why)) {
            ++result.uploaded;
            result.bytesUp += m_local.value(rel).size;
        } else {
            failed(rel, why);
        }
    }

    for (int i = 0; i < p.toDownload.size() && !stop(); ++i) {
        report(QStringLiteral("Downloading photos"), i, int(p.toDownload.size()));
        const RemoteEntry &r = p.toDownload.at(i);
        LocalFile saved;
        QString why;
        if (fetch(r, r.relPath, &saved, &why)) {
            const SyncedFile synced{r.relPath, saved.md5, saved.sha256, saved.size, saved.mtime,
                                    r.version, true};
            m_state.put(synced);
            m_base.insert(r.relPath, synced);
            ++result.downloaded;
            result.bytesDown += saved.size;
        } else {
            failed(r.relPath, why);
        }
    }

    // Conflicts: theirs is saved beside ours, then both go online.
    for (int i = 0; i < p.conflicts.size() && !stop(); ++i) {
        report(QStringLiteral("Keeping both copies of changed photos"), i, int(p.conflicts.size()));
        RemoteEntry r = p.conflicts.at(i);
        if (r.uploadedBy.isEmpty())
            m_store.describe(r);   // best effort, for the copy's name
        const QString copy = conflictPath(r.relPath, r.uploadedBy);
        LocalFile saved;
        QString why;
        if (!fetch(r, copy, &saved, &why)) {
            failed(r.relPath, why);
            continue;
        }
        m_local.insert(copy, saved);
        if (!send(r.relPath, options.who, &why) || !send(copy, r.uploadedBy, &why)) {
            failed(r.relPath, why);
            continue;
        }
        ++result.conflicts;
        result.bytesDown += saved.size;
    }

    for (int i = 0; i < p.toTrash.size() && !stop(); ++i) {
        const QString &rel = p.toTrash.at(i);
        QString why;
        if (moveToTrash(rel, &why)) {
            m_state.remove(rel);
            m_base.remove(rel);
            ++result.trashed;
        } else {
            failed(rel, why);
        }
    }

    if (options.freeUp) {
        for (int i = 0; i < p.freeable.size() && !stop(); ++i) {
            report(QStringLiteral("Freeing up space"), i, int(p.freeable.size()));
            const QString &rel = p.freeable.at(i);
            // Only if it's still exactly what was checked.
            const LocalFile f = m_local.value(rel);
            const QFileInfo info(absPath(rel));
            if (!info.isFile() || info.size() != f.size || mtimeOf(info) != f.mtime) {
                failed(rel, QStringLiteral("it changed on this computer, so it was kept"));
                continue;
            }
            if (!QFile::remove(info.filePath())) {
                failed(rel, QStringLiteral("couldn't delete it"));
                continue;
            }
            // Forgotten, not "deleted here": it comes back if it's wanted again.
            m_state.remove(rel);
            m_base.remove(rel);
            ++result.freed;
            result.bytesFreed += f.size;
        }
    }

    for (const QString &rel : p.removedHere) {
        SyncedFile f = m_base.value(rel);
        f.local = false;
        m_state.put(f);
        m_base.insert(rel, f);
    }

    report(QStringLiteral("Done"), 1, 1);
    return result;
}

LibrarySyncResult LibrarySync::fetch(const QStringList &relPaths)
{
    LibrarySyncResult result;
    m_error.clear();
    m_base = m_state.load();
    const QHash<QString, RemoteEntry> remote = m_state.remote();
    for (int i = 0; i < relPaths.size(); ++i) {
        if (isCancelled()) {
            result.cancelled = true;
            break;
        }
        report(QStringLiteral("Downloading photos"), i, int(relPaths.size()));
        const QString &rel = relPaths.at(i);
        const auto r = remote.constFind(rel);
        if (r == remote.constEnd()) {
            result.failures << QStringLiteral("%1: %2").arg(rel, QStringLiteral("it isn't online"));
            continue;
        }
        m_state.pin(rel);
        const auto b = m_base.constFind(rel);
        if (QFileInfo::exists(absPath(rel)) && b != m_base.constEnd() && b->local
            && b->remoteVersion == r->version)
            continue;   // already here and current
        if (QFileInfo::exists(absPath(rel))) {
            result.failures << QStringLiteral("%1: %2").arg(
                rel, QStringLiteral("a different copy is already here; Sync Photos sorts it out"));
            continue;
        }
        LocalFile saved;
        QString why;
        if (fetch(*r, rel, &saved, &why)) {
            const SyncedFile synced{rel, saved.md5, saved.sha256, saved.size, saved.mtime,
                                    r->version, true};
            m_state.put(synced);
            m_base.insert(rel, synced);
            ++result.downloaded;
            result.bytesDown += saved.size;
        } else if (isCancelled()) {
            result.cancelled = true;
            break;
        } else {
            result.failures << QStringLiteral("%1: %2").arg(rel, why);
        }
    }
    report(QStringLiteral("Done"), 1, 1);
    return result;
}

} // namespace pl::libsync
