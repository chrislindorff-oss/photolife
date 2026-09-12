#include "inat/ExifWriter.h"

#include <exiv2/exiv2.hpp>

#include <cmath>

namespace pl::inat {
namespace {

// EXIF stores a lat/lon degree as three rationals (degrees, minutes, seconds
// -- seconds carrying two implied decimal digits of precision here). This
// builds the value in place and returns it by value.
Exiv2::URationalValue toGpsRational(double absDecimalDegrees)
{
    const int deg = int(absDecimalDegrees);
    const double minutesFull = (absDecimalDegrees - deg) * 60.0;
    const int min = int(minutesFull);
    const double secondsFull = (minutesFull - min) * 60.0;

    Exiv2::URationalValue value;
    value.value_.push_back({static_cast<uint32_t>(deg), 1});
    value.value_.push_back({static_cast<uint32_t>(min), 1});
    value.value_.push_back({static_cast<uint32_t>(std::llround(secondsFull * 100)), 100});
    return value;
}

} // namespace

bool writeExif(const QString &path, const ExifFields &fields, QString *error)
{
    try {
        // Image::AutoPtr (std::auto_ptr-based) was renamed to the
        // std::unique_ptr-based Image::UniquePtr in exiv2 0.28, with no
        // overlap -- 0.27 (this dev machine's apt package) only has
        // AutoPtr, 0.28 (vcpkg's Windows build) only has UniquePtr.
#if EXIV2_TEST_VERSION(0, 28, 0)
        Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(path.toStdString());
#else
        Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(path.toStdString());
#endif
        if (!image.get()) {
            if (error)
                *error = QStringLiteral("could not open %1").arg(path);
            return false;
        }
        image->readMetadata();
        Exiv2::ExifData &exif = image->exifData();

        if (!fields.author.isEmpty()) {
            exif["Exif.Image.Artist"] = fields.author.toStdString();
            exif["Exif.Image.Copyright"] = fields.author.toStdString();
        }
        if (!fields.dateTimeOriginal.isEmpty()) {
            // EXIF dates are "YYYY:MM:DD HH:MM:SS"; observed_on is date-only,
            // so a neutral midday time is used rather than inventing one.
            QString exifDate = fields.dateTimeOriginal;
            exifDate.replace(QLatin1Char('-'), QLatin1Char(':'));
            exifDate += QStringLiteral(" 12:00:00");
            exif["Exif.Photo.DateTimeOriginal"] = exifDate.toStdString();
            exif["Exif.Image.DateTime"] = exifDate.toStdString();
        }
        if (fields.latitude && fields.longitude) {
            exif["Exif.GPSInfo.GPSLatitudeRef"] = *fields.latitude >= 0 ? "N" : "S";
            const Exiv2::URationalValue lat = toGpsRational(std::abs(*fields.latitude));
            exif.add(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitude"), &lat);
            exif["Exif.GPSInfo.GPSLongitudeRef"] = *fields.longitude >= 0 ? "E" : "W";
            const Exiv2::URationalValue lon = toGpsRational(std::abs(*fields.longitude));
            exif.add(Exiv2::ExifKey("Exif.GPSInfo.GPSLongitude"), &lon);
        }
        if (!fields.taxonName.isEmpty()) {
            // No standard EXIF tag for a species identification.
            exif["Exif.Photo.UserComment"] =
                QStringLiteral("charset=Ascii %1").arg(fields.taxonName).toStdString();
        }

        image->setExifData(exif);
        image->writeMetadata();
        return true;
    } catch (const Exiv2::Error &e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return false;
    }
}

} // namespace pl::inat
