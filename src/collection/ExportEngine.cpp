#include "collection/ExportEngine.h"

#include "collection/CollectionManifest.h"
#include "collection/ThumbnailWriter.h"

#include "scan/FileScanner.h"

#include "taxonomy/TaxonomyStore.h"
#include "taxonomy/TaxonomyTypes.h"
#include "util/PathSanitize.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>

namespace pl::collection {
namespace {

using pl::util::sanitizeFilenameComponent;

QString projectName(const QString &connectionName, int projectId)
{
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.prepare(QStringLiteral("SELECT name FROM project WHERE id = ?"));
    q.addBindValue(projectId);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}

// A taxon's folder-name segment: its scientific name, or (when requested,
// available, and different from the scientific name) "Scientific  ·  Common",
// matching the app's own established convention for pairing the two.
QString folderLabel(const taxonomy::TreeNode &node, const ExportOptions &options)
{
    QString label = node.name;
    if (options.includeCommonName && !node.commonName.isEmpty() && node.commonName != node.name)
        label = QStringLiteral("%1  ·  %2").arg(node.name, node.commonName);
    return sanitizeFilenameComponent(label);
}

// The tree's folder layout: `ownFolder` has an entry only for taxa that get a
// real, distinct folder of their own; `resolvedPath` has an entry for every
// taxon in the tree, pointing at whichever folder its own content (child
// folders, or a photo confirmed directly at it) actually belongs in.
//
// When `options.restrictToTaxonIds` is empty, every taxon gets its own folder
// and the two maps are identical. When it's non-empty, a taxon outside the
// set gets no folder of its own -- its `resolvedPath` entry instead points at
// its nearest *included* ancestor's folder (falling back to `rootPath` if
// none is included), so a run of several consecutive excluded ancestors
// collapses cleanly. This is the same reparenting technique
// TaxonomyTreeModel::rebuild() uses for its "hidden ranks" filter
// (TaxonomyTreeModel.cpp's resolvedParent hash), generalized to any subset.
struct FolderResolution
{
    QHash<qint64, QString> ownFolder;
    QHash<qint64, QString> resolvedPath;
};

FolderResolution buildFolderPaths(const QList<taxonomy::TreeNode> &tree, const QString &rootPath,
                                  const ExportOptions &options)
{
    FolderResolution result;
    for (const taxonomy::TreeNode &node : tree) {
        const bool included = options.restrictToTaxonIds.isEmpty()
                                  || options.restrictToTaxonIds.contains(node.inatId);
        const QString base = (node.parentInatId && result.resolvedPath.contains(*node.parentInatId))
                                  ? result.resolvedPath.value(*node.parentInatId)
                                  : rootPath;

        if (included) {
            const QString path = QDir(base).filePath(folderLabel(node, options));
            result.ownFolder.insert(node.inatId, path);
            result.resolvedPath.insert(node.inatId, path);
        } else {
            result.resolvedPath.insert(node.inatId, base);
        }
    }
    return result;
}

// Tracks folders and copies that couldn't be written, so a destination that
// has stopped accepting anything (an unmounted or offline network/cloud
// mount) ends the run instead of failing its way through every photo.
class FailureTracker
{
public:
    explicit FailureTracker(ExportSummary &summary) : m_summary(summary) {}

    void succeeded() { m_consecutive = 0; }

    // Records the failure; true once too many have come in a row.
    bool failed(const QString &what, const QString &reason)
    {
        m_summary.failures << QStringLiteral("%1 — %2").arg(what, reason);
        m_lastReason = reason;
        return ++m_consecutive >= ExportEngine::kMaxConsecutiveFailures;
    }

    // Ends the run: the destination seems to have gone away.
    void giveUp(const QString &collectionRoot)
    {
        m_summary.error = QStringLiteral(
                              "stopped after %1 folders or files in a row couldn't be written "
                              "to %2 — it may be offline or no longer mounted (last error: %3)")
                              .arg(ExportEngine::kMaxConsecutiveFailures)
                              .arg(collectionRoot, m_lastReason);
    }

private:
    ExportSummary &m_summary;
    int m_consecutive = 0;
    QString m_lastReason;
};

// Ensures `path` exists, counting it in `summary.foldersCreated` the first
// time this run creates it (mkpath() itself can't distinguish "already
// there" from "just created"). Remembers each folder's outcome, so a folder
// that couldn't be created is tried (and reported) only once. Returns false
// if the folder isn't there; `giveUp` is set once failures have piled up.
bool ensureFolder(const QString &path, QHash<QString, bool> &made, ExportSummary &summary,
                  FailureTracker &failures, bool &giveUp)
{
    const auto known = made.constFind(path);
    if (known != made.cend())
        return *known;

    bool ok = QDir(path).exists();
    if (!ok) {
        ok = QDir().mkpath(path);
        if (ok)
            ++summary.foldersCreated;
    }
    made.insert(path, ok);
    if (ok)
        failures.succeeded();
    else
        giveUp = failures.failed(path, QStringLiteral("couldn't create the folder"));
    return ok;
}

// Whether files can be created in `folder` right now -- a quick write-and-
// delete probe, since a FUSE mount whose backend has gone away can still
// report the folder as present and writable. Fills `error` when not.
bool canWriteTo(const QString &folder, QString &error)
{
    QFile probe(QDir(folder).filePath(QStringLiteral(".photolife-write-test")));
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate) || probe.write("ok", 2) != 2
        || !probe.flush()) {
        error = probe.errorString();
        probe.remove();
        return false;
    }
    probe.close();
    probe.remove();
    return true;
}

// Where a file named fileName lands in destFolder: path is
// destFolder/fileName, or a "(2)", "(3)", ... variant if a different file
// already sits there. needsCopy is false (and the matching summary counter
// bumped) when a file of the same size is already at `path` -- a copy from
// an earlier run -- unless sizeMeansSame is false, when the caller has
// already looked for an earlier copy itself (see ExistingCopies).
struct Destination
{
    QString path;
    bool needsCopy = true;
};

Destination destinationFor(const QString &destFolder, const QString &fileName,
                           qint64 sourceFileSize, ExportSummary &summary,
                           bool sizeMeansSame = true)
{
    const QFileInfo src(fileName);
    const QString stem = src.completeBaseName();
    const QString suffix = src.suffix();

    QString candidate = QDir(destFolder).filePath(fileName);
    if (!QFileInfo::exists(candidate)) {
        return {candidate, true};
    }
    if (sizeMeansSame && QFileInfo(candidate).size() == sourceFileSize) {
        ++summary.photosSkippedExisting;
        return {candidate, false};
    }

    for (int n = 2;; ++n) {
        const QString name = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(stem).arg(n)
                                              : QStringLiteral("%1 (%2).%3").arg(stem).arg(n).arg(suffix);
        candidate = QDir(destFolder).filePath(name);
        if (!QFileInfo::exists(candidate)) {
            ++summary.photosRenamedForCollision;
            return {candidate, true};
        }
        if (sizeMeansSame && QFileInfo(candidate).size() == sourceFileSize) {
            ++summary.photosSkippedExisting;
            return {candidate, false};
        }
    }
}

// Whether the file at `path` holds exactly `content`, or exactly what's in
// the file at `otherPath` -- read in chunks, since originals can be large.
bool sameContent(const QString &path, const QByteArray &content)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.size() == content.size() && f.readAll() == content;
}

bool sameContent(const QString &path, const QString &otherPath)
{
    QFile a(path);
    QFile b(otherPath);
    if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly) || a.size() != b.size())
        return false;
    constexpr qint64 kChunk = 1 << 20;
    while (!a.atEnd()) {
        if (a.read(kChunk) != b.read(kChunk))
            return false;
    }
    return true;
}

// With a naming pattern, a photo's name can change between runs -- a photo
// matched since the last export can take an earlier sequence number -- so
// an earlier copy is recognised by its content wherever it sits in the
// folder, not by its name. Each folder's files are listed (by size) the
// first time it's asked about; only same-size files are ever read.
class ExistingCopies
{
public:
    QString find(const QString &folder, qint64 size,
                 const std::function<bool(const QString &)> &matches)
    {
        auto it = m_byFolder.find(folder);
        if (it == m_byFolder.end()) {
            QMultiHash<qint64, QString> bySize;
            const QFileInfoList files = QDir(folder).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
            for (const QFileInfo &file : files)
                bySize.insert(file.size(), file.filePath());
            it = m_byFolder.insert(folder, bySize);
        }
        for (auto f = it->constFind(size); f != it->cend() && f.key() == size; ++f) {
            if (matches(f.value()))
                return f.value();
        }
        return {};
    }

private:
    QHash<QString, QMultiHash<qint64, QString>> m_byFolder;
};

struct RenditionRow
{
    QString path;
    qint64 fileSize = 0;
    qint64 taxonInatId = 0;
    QString contentHash;
    qint64 captureId = 0;
    QString capturedOn;
    QString taxonName;
    QString commonName;
    QString locality;
};

QList<RenditionRow> fetchRenditions(const QString &connectionName, int projectId,
                                    const ExportOptions &options)
{
    QStringList statuses = {QStringLiteral("confirmed")};
    if (options.includeAutoApplied)
        statuses << QStringLiteral("auto");
    QStringList placeholders(statuses.size(), QStringLiteral("?"));

    // Originals: every file of the photo (or every non-RAW one). Thumbnails:
    // just the one file a thumbnail is made from, preferring a non-RAW file
    // as the app's own previews do.
    const QString renditionJoin =
        options.thumbnailPx > 0
            ? QStringLiteral("JOIN rendition r ON r.id = ("
                             "   SELECT id FROM rendition WHERE capture_id = c.id "
                             "   ORDER BY (kind = 'raw'), id LIMIT 1) ")
            : QStringLiteral("JOIN rendition r ON r.capture_id = c.id ");
    const QString rawFilter = options.thumbnailPx == 0 && !options.includeRaw
                                  ? QStringLiteral(" AND r.kind <> 'raw'")
                                  : QString();

    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT r.path, r.file_size, t.inat_id, r.content_hash, "
        "       c.id, c.captured_on, t.name, t.common_name, g.locality "
        "FROM capture c "
        "JOIN capture_match m ON m.id = ("
        "   SELECT id FROM capture_match WHERE capture_id = c.id "
        "   ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
        "JOIN taxon t ON t.id = m.taxon_id "
        "%2"
        // CAST(... AS NUMERIC): as in CaptureListModel, Postgres's
        // two-argument ROUND() has no overload for a bare double column.
        "LEFT JOIN geocode_cache g ON g.lat_round = ROUND(CAST(c.latitude AS NUMERIC), 3) "
        "   AND g.lon_round = ROUND(CAST(c.longitude AS NUMERIC), 3) "
        "WHERE m.status IN (%1) "
        "  AND m.taxon_id IN (SELECT taxon_id FROM project_taxon WHERE project_id = ?)%3 "
        // A stable order, so sequence numbers and " (2)" suffixes come out
        // the same on every run.
        "ORDER BY t.inat_id, (c.captured_on IS NULL), c.captured_on, c.id, r.id")
                  .arg(placeholders.join(QLatin1Char(',')), renditionJoin, rawFilter));
    for (const QString &s : statuses)
        q.addBindValue(s);
    q.addBindValue(projectId);

    QList<RenditionRow> rows;
    if (!q.exec())
        return rows;
    while (q.next()) {
        RenditionRow row;
        row.path = q.value(0).toString();
        row.fileSize = q.value(1).toLongLong();
        row.taxonInatId = q.value(2).toLongLong();
        row.contentHash = q.value(3).toString();
        row.captureId = q.value(4).toLongLong();
        row.capturedOn = q.value(5).toString();
        row.taxonName = q.value(6).toString();
        row.commonName = q.value(7).toString();
        row.locality = q.value(8).toString();
        rows.append(row);
    }
    return rows;
}

// The file-name stem each capture gets under options.fileNamePattern: the
// same for all of a capture's files, so its RAW and JPEG stay paired.
// Sequence numbers count up per folder among photos whose names would
// otherwise match, and two different photos that still land on the same
// name in one folder are told apart with " (2)", " (3)", ...
QHash<qint64, QString> namesForCaptures(const QList<RenditionRow> &rows,
                                        const FolderResolution &resolution,
                                        const ExportOptions &options, ExportSummary &summary)
{
    const FileNamePattern &pattern = options.fileNamePattern;
    QHash<qint64, QString> names;
    QHash<QString, int> sequenceCounters;   // folder + name without its sequence number
    QSet<QString> taken;                    // folder + name, case-insensitively
    for (const RenditionRow &row : rows) {
        if (names.contains(row.captureId))
            continue;
        const QString folder = resolution.resolvedPath.value(row.taxonInatId);

        NameFields fields;
        fields.scientificName = row.taxonName;
        if (row.commonName != row.taxonName)
            fields.commonName = row.commonName;
        fields.captured = parseCapturedOn(row.capturedOn);
        const LocalityParts place = splitLocality(row.locality);
        fields.town = place.town;
        fields.state = place.state;
        fields.country = place.country;
        fields.author = options.authorName;
        fields.originalStem = QFileInfo(row.path).completeBaseName();
        if (pattern.uses(NameToken::Sequence)) {
            // fields.sequence is still 0 here, so the number is left out.
            const QString key = folder + QLatin1Char('\n') + pattern.renderParts(fields).toLower();
            fields.sequence = ++sequenceCounters[key];
        }

        const QString stem = pattern.render(fields);
        QString name = stem;
        for (int n = 2; taken.contains(folder + QLatin1Char('\n') + name.toLower()); ++n)
            name = QStringLiteral("%1 (%2)").arg(stem).arg(n);
        if (name != stem)
            ++summary.photosRenamedForCollision;
        taken.insert(folder + QLatin1Char('\n') + name.toLower());
        names.insert(row.captureId, name);
    }
    return names;
}

} // namespace

ExportEngine::ExportEngine(QString connectionName) : m_connectionName(std::move(connectionName))
{
}

ExportSummary ExportEngine::run(int projectId, const QString &destRoot,
                                const ExportOptions &options, const CancelFn &cancel,
                                const ProgressFn &progress)
{
    ExportSummary summary;

    const QString name = projectName(m_connectionName, projectId);
    if (name.isEmpty()) {
        summary.error = QStringLiteral("no such reference tree");
        return summary;
    }

    taxonomy::TaxonomyStore store(m_connectionName);
    const QList<taxonomy::TreeNode> tree = store.projectTree(projectId);
    if (tree.isEmpty()) {
        summary.error = QStringLiteral("this reference tree has no taxa yet");
        return summary;
    }

    const QString rootPath = options.updateExisting
                                 ? QDir::cleanPath(destRoot)
                                 : QDir(destRoot).filePath(sanitizeFilenameComponent(name));
    summary.collectionRoot = rootPath;

    // Fail fast when the destination can't take anything at all, rather than
    // discovering it one folder and one photo at a time.
    if (!QDir().mkpath(rootPath)) {
        summary.error = QStringLiteral("couldn't create %1 — check the drive is connected and "
                                       "mounted").arg(rootPath);
        return summary;
    }
    QString writeError;
    if (!canWriteTo(rootPath, writeError)) {
        summary.error = QStringLiteral("can't write to %1 — check the drive is connected and "
                                       "mounted (%2)").arg(rootPath, writeError);
        return summary;
    }

    const FolderResolution resolution = buildFolderPaths(tree, rootPath, options);
    FailureTracker failures(summary);
    bool giveUp = false;

    QHash<QString, bool> madeFolders;
    if (options.scaffoldEmptyFolders) {
        const int folderTotal = int(resolution.ownFolder.size());
        int foldersDone = 0;
        for (const QString &path : resolution.ownFolder) {
            if (cancel && cancel()) {
                summary.cancelled = true;
                return summary;
            }
            ensureFolder(path, madeFolders, summary, failures, giveUp);
            if (giveUp) {
                failures.giveUp(rootPath);
                return summary;
            }
            if (progress)
                progress(Phase::PreparingFolders, ++foldersDone, folderTotal);
        }
    }

    QSet<QString> expected;   // every file this run copied or found already in place
    const QList<RenditionRow> renditions = fetchRenditions(m_connectionName, projectId, options);
    const bool renaming = !options.fileNamePattern.isEmpty();
    const QHash<qint64, QString> names =
        renaming ? namesForCaptures(renditions, resolution, options, summary)
                 : QHash<qint64, QString>();
    ExistingCopies existingCopies;
    const int total = renditions.size();
    int done = 0;
    for (const RenditionRow &row : renditions) {
        if (cancel && cancel()) {
            summary.cancelled = true;
            return summary;
        }

        const QString folder = resolution.resolvedPath.value(row.taxonInatId);
        if (folder.isEmpty()) {
            ++done;
            continue;   // shouldn't happen: the query already scopes to project_taxon members
        }

        if (!ensureFolder(folder, madeFolders, summary, failures, giveUp)) {
            ++summary.photosFailed;
            if (giveUp) {
                failures.giveUp(rootPath);
                return summary;
            }
        } else if (!QFileInfo::exists(row.path)) {
            // The library copy is missing (e.g. its drive isn't mounted) -- not
            // the destination's fault, so it doesn't count towards giving up.
            ++summary.photosFailed;
            summary.failures << QStringLiteral("%1 — the original file wasn't found")
                                    .arg(row.path);
        } else if (options.thumbnailPx > 0) {
            QString thumbError;
            const QByteArray thumbnail =
                makeThumbnailJpeg(row.path, row.contentHash, options.thumbnailPx,
                                  options.thumbnailCacheDir, &thumbError);
            if (thumbnail.isEmpty()) {
                // The original's fault, not the destination's -- like a
                // missing file, it doesn't count towards giving up.
                ++summary.photosFailed;
                summary.failures << QStringLiteral("%1 — %2").arg(row.path, thumbError);
            } else {
                const QString name =
                    (renaming ? names.value(row.captureId) : QFileInfo(row.path).completeBaseName())
                    + QStringLiteral(".jpg");
                Destination dest;
                const QString earlier =
                    renaming ? existingCopies.find(folder, thumbnail.size(),
                                                   [&](const QString &p) {
                                                       return sameContent(p, thumbnail);
                                                   })
                             : QString();
                if (!earlier.isEmpty()) {
                    ++summary.photosSkippedExisting;
                    dest = {earlier, false};
                } else {
                    dest = destinationFor(folder, name, thumbnail.size(), summary, !renaming);
                }
                QFile out(dest.path);
                if (!dest.needsCopy) {
                    expected.insert(QDir::cleanPath(dest.path));
                    failures.succeeded();
                } else if (out.open(QIODevice::WriteOnly) && out.write(thumbnail) == thumbnail.size()
                           && out.flush()) {
                    out.close();
                    expected.insert(QDir::cleanPath(dest.path));
                    ++summary.photosCopied;
                    failures.succeeded();
                } else {
                    const QString reason = out.errorString();
                    out.close();
                    out.remove();   // never leave a truncated thumbnail behind
                    ++summary.photosFailed;
                    if (failures.failed(dest.path, reason)) {
                        failures.giveUp(rootPath);
                        return summary;
                    }
                }
            }
        } else {
            const QFileInfo sourceInfo(row.path);
            QString name = sourceInfo.fileName();
            if (renaming) {
                name = names.value(row.captureId);
                if (!sourceInfo.suffix().isEmpty())
                    name += QLatin1Char('.') + sourceInfo.suffix();
            }
            Destination dest;
            const QString earlier =
                renaming ? existingCopies.find(folder, row.fileSize,
                                               [&](const QString &p) {
                                                   return sameContent(p, row.path);
                                               })
                         : QString();
            if (!earlier.isEmpty()) {
                ++summary.photosSkippedExisting;
                dest = {earlier, false};
            } else {
                dest = destinationFor(folder, name, row.fileSize, summary, !renaming);
            }
            QFile source(row.path);
            if (!dest.needsCopy) {
                expected.insert(QDir::cleanPath(dest.path));
                failures.succeeded();
            } else if (source.copy(dest.path)) {
                expected.insert(QDir::cleanPath(dest.path));
                ++summary.photosCopied;
                failures.succeeded();
            } else {
                ++summary.photosFailed;
                if (failures.failed(dest.path, source.errorString())) {
                    failures.giveUp(rootPath);
                    return summary;
                }
            }
        }

        ++done;
        if (progress)
            progress(Phase::CopyingPhotos, done, total);
    }

    if (options.reportUnexpectedFiles) {
        const QDir root(rootPath);
        QDirIterator it(rootPath, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        int checked = 0;
        while (it.hasNext()) {
            if (cancel && cancel()) {
                summary.cancelled = true;
                return summary;
            }
            const QString path = QDir::cleanPath(it.next());
            if (progress && ++checked % 200 == 0)
                progress(Phase::CheckingCollection, checked, 0);
            if (it.fileName() == QLatin1String(kManifestFileName) || expected.contains(path))
                continue;
            // RAW copies from an earlier export that included them belong to
            // the collection; they're only left out of this run.
            if (!options.includeRaw
                && scan::FileScanner::rawExtensions().contains(it.fileInfo().suffix().toLower()))
                continue;
            summary.unexpectedFiles << root.relativeFilePath(path);
        }
        summary.unexpectedFiles.sort(Qt::CaseInsensitive);
    }

    // Keep the collection's identity and folder-naming choice for a later update.
    CollectionManifest manifest;
    const auto previous = CollectionManifest::read(rootPath);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    manifest.projectId = projectId;
    manifest.projectName = name;
    manifest.includeCommonName = options.includeCommonName;
    manifest.thumbnailPx = options.thumbnailPx;
    manifest.fileNamePattern = options.fileNamePattern;
    manifest.authorName = options.authorName;
    manifest.createdAt = previous && previous->createdAt.isValid() ? previous->createdAt : now;
    manifest.updatedAt = now;
    if (!manifest.write(rootPath)) {
        summary.failures << QStringLiteral("%1 — couldn't save the collection's record, so a "
                                           "later update may not recognise this folder")
                                .arg(QDir(rootPath).filePath(QLatin1String(kManifestFileName)));
    }

    return summary;
}

} // namespace pl::collection
