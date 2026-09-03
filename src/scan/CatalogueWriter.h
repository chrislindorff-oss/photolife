#pragma once

#include "scan/ScanTypes.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

namespace pl::scan {

// Persists the output of a FileScanner walk into an open catalogue: upserts the
// folder tree, one capture per shot, and a rendition per file. Incremental —
// files whose size and mtime are unchanged are left untouched and not rehashed.
//
// Constructed with the name of an already-open QSqlDatabase connection; all work
// happens on the calling thread, so give it a connection owned by that thread.
class CatalogueWriter
{
public:
    explicit CatalogueWriter(QString connectionName);

    using CancelFn = std::function<bool()>;
    using ProgressFn = std::function<void(const ScanProgress &)>;

    void setCancelPredicate(CancelFn fn) { m_cancel = std::move(fn); }
    void setProgressCallback(ProgressFn fn) { m_progress = std::move(fn); }

    // Writes `captures` (as produced by FileScanner::scan) into the catalogue.
    // `roots` is the set of scan roots, used to root the folder tree's depth and
    // parent links. Runs in a single transaction; on error nothing is committed.
    ScanSummary sync(const QList<DiscoveredCapture> &captures, const QStringList &roots);

private:
    int folderIdFor(const QString &dirPath, const QString &rootPath);
    QString rootForDir(const QString &dirPath, const QStringList &roots) const;
    bool cancelled() const { return m_cancel && m_cancel(); }

    QString m_connectionName;
    CancelFn m_cancel;
    ProgressFn m_progress;

    QHash<QString, int> m_folderIds;   // dirPath -> folder.id, within one sync()
    ScanProgress m_stats;
};

} // namespace pl::scan
