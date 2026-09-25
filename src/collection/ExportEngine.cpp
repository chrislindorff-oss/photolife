#include "collection/ExportEngine.h"

#include "collection/CollectionManifest.h"

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

// Ensures `path` exists, counting it in `summary.foldersCreated` the first
// time this run creates it (mkpath() itself can't distinguish "already
// there" from "just created").
void ensureFolder(const QString &path, QSet<QString> &made, ExportSummary &summary)
{
    if (made.contains(path))
        return;
    made.insert(path);
    if (!QDir(path).exists()) {
        QDir().mkpath(path);
        ++summary.foldersCreated;
    }
}

// Where a source file lands in destFolder: path is destFolder/baseName, or a
// "(2)", "(3)", ... variant if a different file already sits there.
// needsCopy is false (and the matching summary counter bumped) when a file
// of the same size is already at `path` -- a copy from an earlier run.
struct Destination
{
    QString path;
    bool needsCopy = true;
};

Destination destinationFor(const QString &destFolder, const QString &sourcePath,
                           qint64 sourceFileSize, ExportSummary &summary)
{
    const QFileInfo src(sourcePath);
    const QString stem = src.completeBaseName();
    const QString suffix = src.suffix();

    QString candidate = QDir(destFolder).filePath(src.fileName());
    if (!QFileInfo::exists(candidate)) {
        return {candidate, true};
    }
    if (QFileInfo(candidate).size() == sourceFileSize) {
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
        if (QFileInfo(candidate).size() == sourceFileSize) {
            ++summary.photosSkippedExisting;
            return {candidate, false};
        }
    }
}

struct RenditionRow
{
    QString path;
    qint64 fileSize = 0;
    qint64 taxonInatId = 0;
};

QList<RenditionRow> fetchRenditions(const QString &connectionName, int projectId,
                                    const ExportOptions &options)
{
    QStringList statuses = {QStringLiteral("confirmed")};
    if (options.includeAutoApplied)
        statuses << QStringLiteral("auto");
    QStringList placeholders(statuses.size(), QStringLiteral("?"));

    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT r.path, r.file_size, t.inat_id "
        "FROM capture c "
        "JOIN capture_match m ON m.id = ("
        "   SELECT id FROM capture_match WHERE capture_id = c.id "
        "   ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
        "JOIN taxon t ON t.id = m.taxon_id "
        "JOIN rendition r ON r.capture_id = c.id "
        "WHERE m.status IN (%1) "
        "  AND m.taxon_id IN (SELECT taxon_id FROM project_taxon WHERE project_id = ?)")
                  .arg(placeholders.join(QLatin1Char(','))));
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
        rows.append(row);
    }
    return rows;
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
    const FolderResolution resolution = buildFolderPaths(tree, rootPath, options);

    QSet<QString> madeFolders;
    if (options.scaffoldEmptyFolders) {
        for (const QString &path : resolution.ownFolder)
            ensureFolder(path, madeFolders, summary);
    }

    QSet<QString> expected;   // every file this run copied or found already in place
    const QList<RenditionRow> renditions = fetchRenditions(m_connectionName, projectId, options);
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
        ensureFolder(folder, madeFolders, summary);

        const Destination dest = destinationFor(folder, row.path, row.fileSize, summary);
        if (!dest.needsCopy) {
            expected.insert(QDir::cleanPath(dest.path));
        } else if (QFile::copy(row.path, dest.path)) {
            expected.insert(QDir::cleanPath(dest.path));
            ++summary.photosCopied;
        }

        ++done;
        if (progress)
            progress(done, total);
    }

    if (options.reportUnexpectedFiles) {
        const QDir root(rootPath);
        QDirIterator it(rootPath, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = QDir::cleanPath(it.next());
            if (it.fileName() == QLatin1String(kManifestFileName) || expected.contains(path))
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
    manifest.createdAt = previous && previous->createdAt.isValid() ? previous->createdAt : now;
    manifest.updatedAt = now;
    QDir().mkpath(rootPath);
    manifest.write(rootPath);

    return summary;
}

} // namespace pl::collection
