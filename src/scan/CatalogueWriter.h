#pragma once

#include "scan/ScanTypes.h"

#include <QHash>
#include <QSet>
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
    // Unsorted folders (scan::Unsorted): what this sync catalogues, and what
    // it only remembers.
    struct UnsortedPlan
    {
        bool active = false;          // there are Unsorted folders
        QString error;
        QStringList folders;
        QStringList roots;
        QList<DiscoveredCapture> work;   // to catalogue (everything else, and runs' representatives)
        QSet<QString> seen;              // every file found
        int skippedFiles = 0;            // remembered already, unchanged
        struct Member
        {
            QString path, dirPath, baseName, taken;
            qint64 size = 0, mtime = 0;
            int repCaptureId = 0;            // the run's representative, when already catalogued...
            QString repDirPath, repBaseName; // ...or being catalogued now
        };
        QList<Member> members;
        struct Promotion   // a run whose representative's files are gone
        {
            int oldCaptureId = 0;
            QString dirPath, baseName;
            QStringList files;
        };
        QList<Promotion> promotions;
    };
    UnsortedPlan planUnsorted(const QList<DiscoveredCapture> &captures, const QStringList &roots);
    QString rememberUnsorted(const UnsortedPlan &plan);

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
