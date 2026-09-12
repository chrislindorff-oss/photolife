#pragma once

#include <QString>

#include <optional>

namespace pl::inat {

// What gets embedded into a downloaded iNaturalist photo. There's no
// standard EXIF tag for a species identification, so taxonName goes into
// UserComment rather than being invented as a custom tag.
struct ExifFields
{
    QString author;                     // -> Artist / Copyright
    QString dateTimeOriginal;           // "YYYY-MM-DD" (from the observation's observed_on)
    std::optional<double> latitude;
    std::optional<double> longitude;
    QString taxonName;                  // -> UserComment
};

// Writes `fields` into the JPEG at `path`, in place, via exiv2. Returns false
// (with `*error` set, if given) if the file can't be read as a JPEG or the
// write fails -- the downloaded file itself is left as-is either way, so a
// failed write costs metadata, not the photo.
bool writeExif(const QString &path, const ExifFields &fields, QString *error = nullptr);

} // namespace pl::inat
