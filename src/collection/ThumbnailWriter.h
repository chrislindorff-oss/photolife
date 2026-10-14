#pragma once

#include <QByteArray>
#include <QString>

namespace pl::collection {

// Builds the JPEG a thumbnails-only export writes for one photo: `sourcePath`
// scaled so its longest edge is `px`, upright, carrying the source's own
// EXIF/IPTC/XMP (date taken, camera, GPS...) minus its embedded thumbnail and
// maker notes. Reuses the thumbnail cache's file for `contentHash` when one
// exists in `cacheDir`, so photos already seen in the grid cost almost nothing.
//
// The result is deterministic -- the same source always gives the same
// bytes -- which is what lets a later export recognise it as already there.
// Metadata that can't be read (some RAW formats) is simply left out; only
// failing to produce an image at all returns empty bytes, with `*error` set.
// Safe to call from any thread.
QByteArray makeThumbnailJpeg(const QString &sourcePath, const QString &contentHash, int px,
                             const QString &cacheDir, QString *error = nullptr);

} // namespace pl::collection
