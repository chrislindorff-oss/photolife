#pragma once

#include <QDateTime>
#include <QString>

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

    bool hasDate() const { return dateTimeOriginal.isValid(); }
};

// Reads EXIF from a JPEG (APP1/Exif segment). Returns a default-constructed
// ExifData for non-JPEGs, JPEGs without EXIF, or malformed EXIF — never throws.
// Only the first ~256 KiB is inspected, which comfortably covers the metadata.
ExifData readJpegExif(const QString &path);
ExifData readJpegExif(QIODevice &device);
ExifData parseExifSegment(const QByteArray &tiff);   // payload after "Exif\0\0"

} // namespace pl::scan
