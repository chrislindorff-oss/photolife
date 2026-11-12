#pragma once

#include <QDateTime>
#include <QHash>
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
    std::optional<double> focalLength;   // tag 0x920A, millimetres
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
// Notes PhotoLife wrote into a JPEG's XMP or an XMP sidecar (Organise's run
// notes): "sex", "lifeStage", "alive", "flowers", "notes" -> value; empty
// when there are none.
QHash<QString, QString> readXmpNotes(const QString &path);
// The GPS position in an XMP sidecar (exif:GPSLatitude/GPSLongitude, in XMP's
// "DDD,MM.mmmmk" or "DDD,MM,SSk" form), as Lightroom, darktable and
// PhotoLife's Set Location write it; empty when there's none.
ExifData readXmpGps(const QString &path);
// One XMP GPS coordinate ("25,57.2544S") as signed decimal degrees.
std::optional<double> parseXmpCoordinate(const QString &text);
ExifData readJpegExif(QIODevice &device);
// Reads EXIF from a TIFF-based camera RAW (NEF, CR2, ARW, DNG, PEF...) from
// the start of the file alone -- one small read, which matters on a slow
// disk. Empty for other formats, or when the date or GPS lies further in.
ExifData readTiffExif(const QString &path);
ExifData parseExifSegment(const QByteArray &tiff);   // payload after "Exif\0\0"

// A photo's EXIF whatever its kind: a JPEG's own, else a camera RAW's (from
// its embedded preview, as the scanner reads it, or from its TIFF header).
ExifData readPhotoExif(const QString &path);

// A camera's name as people say it, from EXIF's Make and Model:
// "NIKON CORPORATION" + "NIKON D7000" -> "Nikon D7000", "NIKON Z6_3" ->
// "Nikon Z6III", "Google" + "Pixel 9a" -> "Google Pixel 9a". Empty when
// neither says anything.
QString prettyCameraName(const QString &make, const QString &model);

// "300 mm"; "4.5 mm" for the short focal lengths of phones and compacts.
QString focalText(double millimetres);

} // namespace pl::scan
