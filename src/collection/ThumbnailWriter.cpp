#include "collection/ThumbnailWriter.h"

#include "raw/RawPreview.h"
#include "thumb/ThumbnailCache.h"

#include <exiv2/exiv2.hpp>

#include <QBuffer>
#include <QFile>
#include <QImage>

#include <exception>

namespace pl::collection {
namespace {

// See ExifWriter.cpp: exiv2 0.28 renamed Image::AutoPtr to Image::UniquePtr.
#if EXIV2_TEST_VERSION(0, 28, 0)
using ImagePtr = Exiv2::Image::UniquePtr;
#else
using ImagePtr = Exiv2::Image::AutoPtr;
#endif

// Groups that describe the source file's own layout or are bulky vendor
// data, rather than the photo: the embedded preview image, a RAW file's
// extra image directories, and camera maker notes (often tens of KB).
bool isDroppedExifGroup(const std::string &group)
{
    return group == "Thumbnail" || group.rfind("SubImage", 0) == 0
           || group.rfind("Image2", 0) == 0 || group.rfind("Image3", 0) == 0
           || Exiv2::ExifTags::isMakerGroup(group);
}

QByteArray thumbnailImageBytes(const QString &sourcePath, const QString &contentHash, int px,
                               const QString &cacheDir)
{
    if (!contentHash.isEmpty() && !cacheDir.isEmpty()) {
        QFile cached(thumb::ThumbnailCache::diskPathFor(cacheDir, contentHash, px));
        if (cached.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = cached.readAll();
            if (!bytes.isEmpty())
                return bytes;
        }
    }

    const QImage image = thumb::ThumbnailCache::render(sourcePath, px, &raw::extractPreview);
    if (image.isNull())
        return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    // Same format and quality as the thumbnail cache, so a thumbnail made here
    // matches one made from the cache byte for byte.
    if (!image.save(&buffer, "JPG", 85))
        return {};
    return bytes;
}

// `jpeg` with the source's metadata written into it, or `jpeg` unchanged if
// the source has none that exiv2 can read.
QByteArray withSourceMetadata(const QByteArray &jpeg, const QString &sourcePath)
{
    try {
        ImagePtr source = Exiv2::ImageFactory::open(QFile::encodeName(sourcePath).toStdString());
        if (!source.get())
            return jpeg;
        source->readMetadata();

        Exiv2::ExifData exif = source->exifData();
        for (auto it = exif.begin(); it != exif.end();) {
            if (isDroppedExifGroup(it->groupName()) || it->key() == "Exif.Photo.MakerNote")
                it = exif.erase(it);
            else
                ++it;
        }
        Exiv2::XmpData xmp = source->xmpData();
        if (exif.empty() && source->iptcData().empty() && xmp.empty())
            return jpeg;

        // The pixels were already turned upright when the thumbnail was made.
        if (!exif.empty())
            exif["Exif.Image.Orientation"] = uint16_t(1);
        if (xmp.findKey(Exiv2::XmpKey("Xmp.tiff.Orientation")) != xmp.end())
            xmp["Xmp.tiff.Orientation"] = "1";

        ImagePtr target = Exiv2::ImageFactory::open(
            reinterpret_cast<const Exiv2::byte *>(jpeg.constData()), jpeg.size());
        if (!target.get())
            return jpeg;
        target->setExifData(exif);
        target->setIptcData(source->iptcData());
        target->setXmpData(xmp);
        target->writeMetadata();

        Exiv2::BasicIo &io = target->io();
        const auto size = io.size();
        const Exiv2::byte *data = io.mmap();
        QByteArray result(reinterpret_cast<const char *>(data), qsizetype(size));
        io.munmap();
        return result.isEmpty() ? jpeg : result;
    } catch (const std::exception &) {
        return jpeg;   // unreadable or unsupported metadata: keep the image without it
    }
}

} // namespace

QByteArray makeThumbnailJpeg(const QString &sourcePath, const QString &contentHash, int px,
                             const QString &cacheDir, QString *error)
{
    const QByteArray jpeg = thumbnailImageBytes(sourcePath, contentHash, px, cacheDir);
    if (jpeg.isEmpty()) {
        if (error)
            *error = QStringLiteral("couldn't make a thumbnail (the file couldn't be read as "
                                    "an image)");
        return {};
    }
    return withSourceMetadata(jpeg, sourcePath);
}

} // namespace pl::collection
