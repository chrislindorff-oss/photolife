#pragma once

#include <QString>
#include <QStringList>

namespace pl::geo {

// Writes a photo's location (Set Location on Map) into its files: a JPEG's
// EXIF, keeping the file's date; for a RAW, an XMP sidecar -- its existing one
// ("DSC_1234.xmp", or darktable's "DSC_1234.NEF.xmp"), else a new
// "DSC_1234.xmp" -- and, only when asked, the RAW itself, where that can be
// done safely. No catalogue: just files.
class GeotagWriter
{
public:
    struct Result
    {
        QString createdSidecar;   // a new sidecar, to move with the shot
        QStringList failures;     // "DSC_1234.NEF: why", one per file not written
        bool ok() const { return failures.isEmpty(); }
    };

    // `renditions`: the shot's photo files (not its sidecars); `baseName` names
    // a new sidecar.
    static Result writeShot(const QStringList &renditions, const QString &baseName, double latitude,
                            double longitude, bool intoRaw);

    // Whether a RAW of this kind can have its location written into it: the
    // TIFF-based formats exiv2 writes (not Canon CR3 or Fuji RAF).
    static bool canWriteIntoRaw(const QString &path);

    // Writes the location into the RAW at `path` through a working copy,
    // which replaces it only once it reads back with the same date and camera
    // and the new position; the file keeps its date. `path` is untouched on
    // any failure.
    static bool writeIntoRaw(const QString &path, double latitude, double longitude,
                             QString *error = nullptr);
};

} // namespace pl::geo
