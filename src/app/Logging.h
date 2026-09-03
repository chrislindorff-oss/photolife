#pragma once

#include <QString>

namespace pl::logging {

// Installs a message handler that writes to both stderr and a rolling log file
// under `logDir` (created if needed). Safe to call once at startup.
void install(const QString &logDir);

// Absolute path of the current log file, or empty if logging is not installed.
QString currentLogFile();

} // namespace pl::logging
