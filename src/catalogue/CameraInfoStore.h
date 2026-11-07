#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace pl::catalogue {

// What a photo's EXIF says about the camera: the cache behind the
// Photographic Life List's Camera and Focal length columns (capture_camera).
struct CameraInfo
{
    int captureId = 0;
    QString camera;                      // as people say it ("Nikon D7000"); empty when not recorded
    std::optional<double> focalLength;   // millimetres
};

// Give it an open connection name; all calls run on the caller's thread.
class CameraInfoStore
{
public:
    explicit CameraInfoStore(QString connectionName);

    // Records (or replaces) what was read, in one transaction. False on error.
    bool save(const QList<CameraInfo> &infos);

    QString error() const { return m_error; }

private:
    QString m_connectionName;
    QString m_error;
};

} // namespace pl::catalogue
