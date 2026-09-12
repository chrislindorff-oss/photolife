#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

#include <optional>

namespace pl::thumb {
class ThumbnailCache;
}

namespace pl::raw {

// True when this build was compiled with LibRaw. When false, extractPreview()
// always returns a null image and RAW-only captures keep a placeholder tile.
bool isAvailable();

// Reads the largest embedded preview (usually a full-size JPEG) from a camera
// RAW file, applies its orientation, and returns it scaled so its longest edge
// is at least `minLongestEdge` (never upscaled). Null QImage on any failure or
// when LibRaw is not compiled in.
QImage extractPreview(const QString &path, int minLongestEdge);

// Raw embedded-preview JPEG bytes (no decode/scale) from a camera RAW file —
// for feeding to the JPEG EXIF parser, since cameras typically stamp the
// embedded preview with the same date/camera EXIF as the original capture.
// Do NOT use this for GPS: LibRaw's reconstructed copy of the embedded
// preview's EXIF has been observed to corrupt small inline fields, including
// GPSLatitudeRef/GPSLongitudeRef — see extractGps() instead. Empty on
// failure, a non-JPEG embedded thumbnail, or when LibRaw isn't compiled in.
QByteArray extractEmbeddedJpegBytes(const QString &path);

struct RawGps
{
    std::optional<double> latitude;    // decimal degrees, +N/-S
    std::optional<double> longitude;   // decimal degrees, +E/-W
};

// Reads GPS from the RAW file's own metadata (LibRaw's parser), not the
// embedded-JPEG-preview EXIF copy extractEmbeddedJpegBytes() returns — that
// copy has been observed to corrupt small inline fields (GPSLatitudeRef in
// particular), silently defaulting the hemisphere sign to positive. Returns
// an empty result for a missing/non-RAW file, a RAW file with no GPS, or
// when LibRaw isn't compiled in.
RawGps extractGps(const QString &path);

// Pure DMS -> decimal-degrees conversion, exposed for testing: deg + min/60
// + sec/3600, negated when `ref` case-insensitively matches `negativeRef`
// (e.g. ('S', 'S') for latitude, ('W', 'W') for longitude).
double dmsToDecimalDegrees(double deg, double min, double sec, char ref, char negativeRef);

// Installs extractPreview() as the ThumbnailCache's RAW source loader. Safe to
// call even when LibRaw is absent (it becomes a no-op path).
void installRawLoader(pl::thumb::ThumbnailCache &cache);

} // namespace pl::raw
