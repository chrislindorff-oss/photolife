#pragma once

#include <QString>

namespace pl::util {

// Strips characters illegal in filenames/folder names on the platforms this
// app targets (Windows is the most restrictive -- <>:"/\|?* plus control
// characters), replacing each with a space, then collapses the whitespace
// left behind so a multi-word species or place name stays readable.
QString sanitizeFilenameComponent(const QString &text);

} // namespace pl::util
