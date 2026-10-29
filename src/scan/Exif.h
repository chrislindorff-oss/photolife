#pragma once

#include <QDateTime>
#include <QString>

#include <optional>

class QByteArray;
class QIODevice;

namespace pl::scan {

// The handful of EXIF fields PhotoLife uses. All optional: a field is only set
// when the file actually carried it.
struct ExifData
{
    QDateTime dateTimeOriginal;   // tag 0x9003, the moment the shutter fired
    QString cameraMake;           // tag 0x010F
    QString cameraModel;          // tag 0x0110
    std::optional<double> latitude;    // decimal degrees, +N/-S (GPS IFD)
    std::optional<double> longitude;   // decimal degrees, +E/-W (GPS IFD)

    bool hasDate() const { return dateTimeOriginal.isValid(); }
    bool hasGps() const { return latitude.has_value() && longitude.has_value(); }
};

// Reads EXIF from a JPEG (APP1/Exif segment). Returns a default-constructed
// ExifData for non-JPEGs, JPEGs without EXIF, or malformed EXIF — never throws.
// Only the first ~256 KiB is inspected, which comfortably covers the metadata.
ExifData readJpegExif(const QString &path);

// The star rating (0-5) in a JPEG's XMP or in an XMP sidecar file, as photo
// programs write it (xmp:Rating); nullopt when there's none.
std::optional<int> readXmpRating(const QString &path);
ExifData readJpegExif(QIODevice &device);
// Reads EXIF from a TIFF-based camera RAW (NEF, CR2, ARW, DNG, PEF...) from
// the start of the file alone -- one small read, which matters on a slow
// disk. Empty for other formats, or when the date or GPS lies further in.
ExifData readTiffExif(const QString &path);
ExifData parseExifSegment(const QByteArray &tiff);   // payload after "Exif\0\0"

} // namespace pl::scan
