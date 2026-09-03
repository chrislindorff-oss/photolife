#pragma once

#include <QImage>
#include <QString>

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

// Installs extractPreview() as the ThumbnailCache's RAW source loader. Safe to
// call even when LibRaw is absent (it becomes a no-op path).
void installRawLoader(pl::thumb::ThumbnailCache &cache);

} // namespace pl::raw
