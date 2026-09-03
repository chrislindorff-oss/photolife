#include "raw/RawPreview.h"

#include "thumb/ThumbnailCache.h"

#include <QTransform>

#ifdef PHOTOLIFE_HAVE_LIBRAW
#include <libraw/libraw.h>
#include <cstring>
#endif

namespace pl::raw {

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

#else  // no LibRaw

QImage extractPreview(const QString &, int)
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
