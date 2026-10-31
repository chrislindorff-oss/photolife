#include "geo/GeotagWriter.h"

#include "inat/ExifWriter.h"
#include "scan/Exif.h"
#include "scan/FileScanner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cmath>

namespace pl::geo {
namespace {

bool isRaw(const QString &path)
{
    return scan::FileScanner::rawExtensions().contains(QFileInfo(path).suffix().toLower());
}

bool isJpeg(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") || ext == QLatin1String("jpe");
}

// Puts the file's modification time back: a location isn't an edit, and
// Organise recognises a shot by it.
void keepDate(const QString &path, const QDateTime &modified)
{
    QFile file(path);
    if (file.open(QIODevice::ReadWrite))
        file.setFileTime(modified, QFileDevice::FileModificationTime);
}

bool samePlace(const scan::ExifData &exif, double latitude, double longitude)
{
    // EXIF keeps seconds to 1/100: about 30 cm.
    return exif.hasGps() && std::abs(*exif.latitude - latitude) < 1e-5
           && std::abs(*exif.longitude - longitude) < 1e-5;
}

} // namespace

bool GeotagWriter::canWriteIntoRaw(const QString &path)
{
    static const QStringList kinds = {QStringLiteral("nef"), QStringLiteral("nrw"), QStringLiteral("cr2"),
                                      QStringLiteral("arw"), QStringLiteral("dng"), QStringLiteral("orf"),
                                      QStringLiteral("pef"), QStringLiteral("rw2"), QStringLiteral("srw")};
    return kinds.contains(QFileInfo(path).suffix().toLower());
}

bool GeotagWriter::writeIntoRaw(const QString &path, double latitude, double longitude, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (!canWriteIntoRaw(path))
        return fail(QStringLiteral("this kind of RAW can't be written to"));
    const scan::ExifData before = scan::readTiffExif(path);
    if (!before.hasDate())
        return fail(QStringLiteral("its metadata couldn't be read, so it's left as it is"));
    const QDateTime modified = QFileInfo(path).lastModified();
    const qint64 size = QFileInfo(path).size();

    const QString temp = path + QStringLiteral(".photolife-tmp");
    QFile::remove(temp);   // a leftover from an earlier failed attempt
    if (!QFile::copy(path, temp))
        return fail(QStringLiteral("couldn't make a working copy"));
    inat::ExifFields fields;
    fields.latitude = latitude;
    fields.longitude = longitude;
    QString why;
    if (!inat::writeExif(temp, fields, &why)) {
        QFile::remove(temp);
        return fail(why);
    }
    // The copy must still be the same photo, now with the position -- and
    // not suspiciously shrunk -- before it may replace the original.
    const scan::ExifData after = scan::readTiffExif(temp);
    if (after.dateTimeOriginal != before.dateTimeOriginal || after.cameraModel != before.cameraModel
        || !samePlace(after, latitude, longitude) || QFileInfo(temp).size() < size * 9 / 10) {
        QFile::remove(temp);
        return fail(QStringLiteral("the written copy didn't read back intact, so the original is left as it is"));
    }
    keepDate(temp, modified);
    if (!QFile::remove(path)) {
        QFile::remove(temp);
        return fail(QStringLiteral("couldn't replace the original"));
    }
    if (!QFile::rename(temp, path))
        return fail(QStringLiteral("written, but couldn't be put in place: the copy is at %1").arg(temp));
    return true;
}

GeotagWriter::Result GeotagWriter::writeShot(const QStringList &renditions, const QString &baseName,
                                             double latitude, double longitude, bool intoRaw)
{
    Result out;
    bool needsSidecar = false;
    for (const QString &path : renditions) {
        const QString name = QFileInfo(path).fileName();
        if (isJpeg(path)) {
            const QDateTime modified = QFileInfo(path).lastModified();
            inat::ExifFields fields;
            fields.latitude = latitude;
            fields.longitude = longitude;
            QString error;
            if (inat::writeExifSafely(path, fields, &error))
                keepDate(path, modified);
            else
                out.failures << QStringLiteral("%1: %2").arg(name, error);
        } else if (isRaw(path)) {
            needsSidecar = true;
            QString error;
            if (intoRaw && canWriteIntoRaw(path) && !writeIntoRaw(path, latitude, longitude, &error))
                out.failures << QStringLiteral("%1: %2").arg(name, error);
        }
    }
    if (!needsSidecar || renditions.isEmpty())
        return out;

    // Every sidecar the shot already has, else a new one by its base name.
    const QDir dir = QFileInfo(renditions.first()).absoluteDir();
    QStringList sidecars;
    const QString shared = dir.filePath(baseName + QStringLiteral(".xmp"));
    if (QFileInfo::exists(shared))
        sidecars << shared;
    for (const QString &path : renditions) {
        if (QFileInfo::exists(path + QStringLiteral(".xmp")))
            sidecars << path + QStringLiteral(".xmp");
    }
    if (sidecars.isEmpty()) {
        sidecars << shared;
        out.createdSidecar = shared;
    }
    for (const QString &sidecar : std::as_const(sidecars)) {
        QString error;
        if (!inat::writeSidecarGps(sidecar, latitude, longitude, &error)) {
            out.failures << QStringLiteral("%1: %2").arg(QFileInfo(sidecar).fileName(), error);
            if (sidecar == out.createdSidecar && !QFileInfo::exists(sidecar))
                out.createdSidecar.clear();
        }
    }
    return out;
}

} // namespace pl::geo
