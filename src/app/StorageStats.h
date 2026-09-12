#pragma once

#include <QString>
#include <QtGlobal>

namespace pl::app {

// The combined size, in bytes, of every regular file under `dir` (recursing
// into subdirectories). 0 if `dir` doesn't exist. Used to report cache disk
// use to the user; not cheap for a large cache, so call it only when the user
// asks (e.g. a "Storage Usage" dialog), not on a hot path.
qint64 directoryBytes(const QString &dir);

} // namespace pl::app
