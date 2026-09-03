#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace pl::scan {

// One image file found on disk, before it is tied to a catalogue row.
struct DiscoveredFile
{
    QString path;      // absolute, native separators
    QString dirPath;   // absolute path of the containing folder
    QString stem;      // file name without the final extension
    QString ext;       // lower-cased extension without the dot ("jpg", "nef")
    qint64 size = 0;
    qint64 mtime = 0;  // seconds since epoch
    bool isRaw = false;
};

// The RAW + JPEG renditions of a single shot, grouped by (folder, stem).
struct DiscoveredCapture
{
    QString dirPath;
    QString baseName;               // the shared stem
    QList<DiscoveredFile> files;    // one or more renditions, RAW and/or JPEG
};

// Progress ticks emitted during a walk.
struct ScanProgress
{
    int foldersSeen = 0;
    int filesSeen = 0;       // image files kept (junk excluded)
    int capturesSeen = 0;
    QString currentDir;
};

// Result of persisting a scan into the catalogue.
struct ScanSummary
{
    int foldersUpserted = 0;
    int capturesAdded = 0;
    int capturesUpdated = 0;
    int renditionsAdded = 0;
    int renditionsUpdated = 0;
    int renditionsUnchanged = 0;
    int filesHashed = 0;
    bool cancelled = false;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

} // namespace pl::scan

Q_DECLARE_METATYPE(pl::scan::ScanProgress)
Q_DECLARE_METATYPE(pl::scan::ScanSummary)
