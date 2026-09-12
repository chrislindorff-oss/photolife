#include "raw/RawPreview.h"

#include "thumb/ThumbnailCache.h"

#include <QTransform>

#include <cctype>

#ifdef PHOTOLIFE_HAVE_LIBRAW
#include <libraw/libraw.h>
#include <cstring>
#endif

namespace pl::raw {

double dmsToDecimalDegrees(double deg, double min, double sec, char ref, char negativeRef)
{
    const double magnitude = deg + min / 60.0 + sec / 3600.0;
    const bool negative =
        std::toupper(static_cast<unsigned char>(ref)) == std::toupper(static_cast<unsigned char>(negativeRef));
    return negative ? -magnitude : magnitude;
}

bool isAvailable()
{
#ifdef PHOTOLIFE_HAVE_LIBRAW
    return true;
#else
    return false;
#endif
}

#ifdef PHOTOLIFE_HAVE_LIBRAW
namespace {

// LibRaw's flip codes map to the same rotations as EXIF orientation.
QImage applyFlip(QImage image, int flip)
{
    switch (flip) {
    case 3:
        return image.transformed(QTransform().rotate(180));
    case 5:
    case 6:
        return image.transformed(QTransform().rotate(90));
    case 7:
    case 8:
        return image.transformed(QTransform().rotate(-90));
    default:
        return image;
    }
}

QImage fromThumb(const libraw_processed_image_t *thumb, int flip)
{
    if (!thumb)
        return {};

    QImage image;
    if (thumb->type == LIBRAW_IMAGE_JPEG) {
        image.loadFromData(reinterpret_cast<const uchar *>(thumb->data), int(thumb->data_size),
                           "JPEG");
    } else if (thumb->type == LIBRAW_IMAGE_BITMAP && thumb->colors == 3) {
        const int w = int(thumb->width);
        const int h = int(thumb->height);
        if (w <= 0 || h <= 0 || qint64(thumb->data_size) < qint64(w) * h * 3)
            return {};
        // Wrap the packed RGB buffer, then copy() to get an owned, aligned image.
        image = QImage(reinterpret_cast<const uchar *>(thumb->data), w, h, w * 3,
                       QImage::Format_RGB888)
                    .copy();
    }
    if (image.isNull())
        return {};
    return applyFlip(std::move(image), flip);
}

} // namespace

QImage extractPreview(const QString &path, int minLongestEdge)
{
    LibRaw raw;
    if (raw.open_file(path.toLocal8Bit().constData()) != LIBRAW_SUCCESS)
        return {};
    if (raw.unpack_thumb() != LIBRAW_SUCCESS)
        return {};

    int err = 0;
    libraw_processed_image_t *thumb = raw.dcraw_make_mem_thumb(&err);
    QImage image = fromThumb(thumb, raw.imgdata.sizes.flip);
    if (thumb)
        LibRaw::dcraw_clear_mem(thumb);
    raw.recycle();

    if (image.isNull())
        return {};

    const int longest = qMax(image.width(), image.height());
    if (minLongestEdge > 0 && longest > minLongestEdge * 2) {
        // Full-size preview but we only need a thumbnail — shrink to save memory.
        image = image.scaled(minLongestEdge * 2, minLongestEdge * 2, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return image;
}

QByteArray extractEmbeddedJpegBytes(const QString &path)
{
    LibRaw raw;
    if (raw.open_file(path.toLocal8Bit().constData()) != LIBRAW_SUCCESS)
        return {};
    if (raw.unpack_thumb() != LIBRAW_SUCCESS)
        return {};

    int err = 0;
    libraw_processed_image_t *thumb = raw.dcraw_make_mem_thumb(&err);
    QByteArray bytes;
    if (thumb && thumb->type == LIBRAW_IMAGE_JPEG)
        bytes = QByteArray(reinterpret_cast<const char *>(thumb->data), int(thumb->data_size));
    if (thumb)
        LibRaw::dcraw_clear_mem(thumb);
    raw.recycle();
    return bytes;
}

namespace {
bool isRef(char c, char a, char b)
{
    const int u = std::toupper(static_cast<unsigned char>(c));
    return u == std::toupper(static_cast<unsigned char>(a)) || u == std::toupper(static_cast<unsigned char>(b));
}
} // namespace

RawGps extractGps(const QString &path)
{
    LibRaw raw;
    if (raw.open_file(path.toLocal8Bit().constData()) != LIBRAW_SUCCESS)
        return {};

    RawGps out;
    const libraw_gps_info_t &gps = raw.imgdata.other.parsed_gps;
    // gpsparsed can be set even when the file carries an empty/placeholder GPS
    // block (no fix acquired) -- e.g. latref/longref are null bytes and every
    // DMS component is zero, which would otherwise silently become (0, 0):
    // "Null Island", in the Atlantic off West Africa. Require a real N/S and
    // E/W reference before trusting the coordinates at all.
    const bool hasRefs = isRef(gps.latref, 'N', 'S') && isRef(gps.longref, 'E', 'W');
    if (gps.gpsparsed && hasRefs) {
        out.latitude = dmsToDecimalDegrees(gps.latitude[0], gps.latitude[1], gps.latitude[2],
                                           gps.latref, 'S');
        out.longitude = dmsToDecimalDegrees(gps.longitude[0], gps.longitude[1], gps.longitude[2],
                                            gps.longref, 'W');
    }
    raw.recycle();
    return out;
}

#else  // no LibRaw

QImage extractPreview(const QString &, int)
{
    return {};
}

QByteArray extractEmbeddedJpegBytes(const QString &)
{
    return {};
}

RawGps extractGps(const QString &)
{
    return {};
}

#endif

void installRawLoader(pl::thumb::ThumbnailCache &cache)
{
    if (!isAvailable())
        return;
    cache.setRawLoader([](const QString &path, int longestEdge) {
        return extractPreview(path, longestEdge);
    });
}

} // namespace pl::raw
