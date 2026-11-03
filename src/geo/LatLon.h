#pragma once

#include <QPointF>
#include <QString>

#include <optional>

namespace pl::geo {

// A location as text for the clipboard, "-37.831200, 144.982100" (latitude
// first, as Google Maps copies it).
QString formatLatLon(double latitude, double longitude);

// A location in text: two numbers, latitude then longitude, separated by a
// comma, semicolon or spaces -- what formatLatLon() writes, or what Google
// Maps copies. x = longitude, y = latitude; nothing when it isn't one or is
// out of range.
std::optional<QPointF> parseLatLon(const QString &text);

} // namespace pl::geo
