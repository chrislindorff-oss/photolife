#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace pl::scan {

// Housekeeping for the local catalogue that isn't part of a scan: forgetting
// captures the user no longer wants tracked, and finding the ones whose files
// have vanished from disk (typically left behind when a file was renamed or
// moved and the rescan's rename-detection didn't catch it).
//
// Nothing here touches files on disk. Give it an open connection name; all
// calls run on the caller's thread.
class CatalogueMaintenance
{
public:
    explicit CatalogueMaintenance(QString connectionName);

    // Removes the given captures from the catalogue. Their renditions and any
    // match rows go too (ON DELETE CASCADE); a project's chosen representative
    // is cleared where it pointed at one of them. Returns the number of capture
    // rows actually removed, or 0 on error (see error()).
    int forgetCaptures(const QList<int> &captureIds);

    // A catalogued capture with no backing file left on disk.
    struct MissingCapture
    {
        int captureId = 0;
        QString baseName;
        QString folderPath;
        QStringList missingPaths;   // the rendition file paths that are gone
    };

    // Every capture for which no backing file is present on disk any more,
    // ordered by folder then name. Each carries enough to show the user exactly
    // what would be removed.
    QList<MissingCapture> capturesWithMissingFiles() const;

    // Recomputes latitude/longitude for every capture with a RAW rendition,
    // using raw::extractGps() (LibRaw's own metadata parser) instead of
    // whatever was stored by the embedded-JPEG-EXIF path, which could
    // corrupt the hemisphere sign for some files. Also clears a (0, 0) "Null
    // Island" value a prior, buggier version of this same backfill could have
    // written for a RAW file with no real GPS fix. Idempotent and safe to run
    // repeatedly; captures with no RAW rendition, or whose RAW file genuinely
    // has no usable GPS, are left untouched (beyond that (0, 0) cleanup).
    // Returns the number of captures changed, or 0 on error (see error()).
    int backfillRawGeolocation();

    QString error() const { return m_error; }

private:
    QString m_connectionName;
    mutable QString m_error;
};

} // namespace pl::scan
