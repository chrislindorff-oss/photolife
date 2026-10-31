#include "inat/ExifWriter.h"

#include <exiv2/exiv2.hpp>

#include <QFile>

#include <cmath>
#include <functional>

namespace pl::inat {
namespace {

// Image::AutoPtr (std::auto_ptr-based) was renamed to the std::unique_ptr-
// based Image::UniquePtr in exiv2 0.28, with no overlap -- 0.27 (this dev
// machine's apt package) only has AutoPtr, 0.28 (vcpkg's Windows build) only
// has UniquePtr.
#if EXIV2_TEST_VERSION(0, 28, 0)
using ImagePtr = Exiv2::Image::UniquePtr;
#else
using ImagePtr = Exiv2::Image::AutoPtr;
#endif

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
        ImagePtr image = Exiv2::ImageFactory::open(path.toStdString());
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

        if (fields.rating) {
            // Both where photo programs look: XMP (Lightroom, darktable,
            // digiKam) and the EXIF tag (Windows).
            Exiv2::XmpData &xmp = image->xmpData();
            xmp["Xmp.xmp.Rating"] = std::to_string(*fields.rating);
            exif["Exif.Image.Rating"] = uint16_t(*fields.rating);
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

namespace {

// Opens the sidecar at `path` -- creating a minimal empty one first if there
// isn't one -- lets `set` change its XMP, and writes it back.
bool updateSidecar(const QString &path, const std::function<void(Exiv2::XmpData &)> &set, QString *error)
{
    if (!QFile::exists(path)) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            if (error)
                *error = QStringLiteral("could not create %1").arg(path);
            return false;
        }
        // The packet's begin attribute is a UTF-8 byte-order mark.
        const QByteArray packet("<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                                "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
                                " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                                "  <rdf:Description rdf:about=\"\"/>\n"
                                " </rdf:RDF>\n"
                                "</x:xmpmeta>\n"
                                "<?xpacket end=\"w\"?>\n");
        if (file.write(packet) != packet.size()) {
            if (error)
                *error = QStringLiteral("could not write %1").arg(path);
            return false;
        }
    }
    try {
        ImagePtr sidecar = Exiv2::ImageFactory::open(path.toStdString());
        if (!sidecar.get())
            throw Exiv2::Error(Exiv2::ErrorCode::kerErrorMessage, "open failed");
        sidecar->readMetadata();
        Exiv2::XmpData xmp = sidecar->xmpData();
        set(xmp);
        sidecar->setXmpData(xmp);
        // Reading a sidecar also converts its XMP into EXIF, which writing
        // converts back over the XMP -- undoing a changed position. The XMP is
        // the whole of it, so the converted copy goes.
        sidecar->clearExifData();
        sidecar->writeMetadata();
        return true;
    } catch (const Exiv2::Error &e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return false;
    }
}

} // namespace

bool writeSidecarRating(const QString &path, int rating, QString *error)
{
    return updateSidecar(path, [rating](Exiv2::XmpData &xmp) {
        xmp["Xmp.xmp.Rating"] = std::to_string(rating);
    }, error);
}

QString xmpCoordinate(double value, bool latitude)
{
    const double a = std::abs(value);
    const int degrees = int(a);
    const QChar ref = latitude ? (value < 0 ? QLatin1Char('S') : QLatin1Char('N'))
                               : (value < 0 ? QLatin1Char('W') : QLatin1Char('E'));
    return QStringLiteral("%1,%2%3").arg(degrees).arg((a - degrees) * 60.0, 0, 'f', 5).arg(ref);
}

bool writeSidecarGps(const QString &path, double latitude, double longitude, QString *error)
{
    return updateSidecar(path, [latitude, longitude](Exiv2::XmpData &xmp) {
        xmp["Xmp.exif.GPSVersionID"] = "2.2.0.0";
        xmp["Xmp.exif.GPSLatitude"] = xmpCoordinate(latitude, true).toStdString();
        xmp["Xmp.exif.GPSLongitude"] = xmpCoordinate(longitude, false).toStdString();
    }, error);
}

bool writeExifSafely(const QString &path, const ExifFields &fields, QString *error)
{
    const QString tempPath = path + QStringLiteral(".photolife-tmp");
    QFile::remove(tempPath);   // stale leftover from an earlier failed attempt

    if (!QFile::copy(path, tempPath)) {
        if (error)
            *error = QStringLiteral("could not create a working copy of %1").arg(path);
        return false;
    }

    QString writeError;
    if (!writeExif(tempPath, fields, &writeError)) {
        QFile::remove(tempPath);
        if (error)
            *error = writeError;
        return false;
    }

    // A "successful" write that actually produced something broken must not
    // be allowed to replace the only copy of the original -- confirm the
    // result still opens as a valid image before trusting it.
    try {
        ImagePtr check = Exiv2::ImageFactory::open(tempPath.toStdString());
        if (!check.get())
            throw Exiv2::Error(Exiv2::ErrorCode::kerErrorMessage, "reopen failed");
        check->readMetadata();
    } catch (const Exiv2::Error &e) {
        QFile::remove(tempPath);
        if (error)
            *error = QStringLiteral("write appeared to succeed but the result failed "
                                    "verification: %1").arg(QString::fromStdString(e.what()));
        return false;
    }

    // No cross-platform atomic file-replace primitive is available here
    // (Qt's QFile::rename() refuses to overwrite an existing destination),
    // so this is remove-then-rename rather than a single atomic step -- but
    // verification above has already run before the original is touched, so
    // the only failure mode left is losing the temp file's name, never a
    // corrupted result silently taking the original's place.
    if (!QFile::remove(path)) {
        QFile::remove(tempPath);
        if (error)
            *error = QStringLiteral("could not remove the original file at %1").arg(path);
        return false;
    }
    if (!QFile::rename(tempPath, path)) {
        if (error)
            *error = QStringLiteral("wrote successfully but could not move the result into "
                                    "place (original was removed; recovered copy left at %1)")
                          .arg(tempPath);
        return false;
    }
    return true;
}

} // namespace pl::inat
