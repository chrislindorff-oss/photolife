#pragma once

#include <QDateTime>
#include <QString>

#include <optional>

namespace pl::inat {

// Where iNaturalist hands out a signed-in user's API token: a 24-hour JWT,
// shown as {"api_token":"..."}.
inline constexpr char kApiTokenPage[] = "https://www.inaturalist.org/users/api_token";

// The API token in text copied from that page -- its JSON, or the bare token
// -- or empty when the text isn't one.
QString extractApiToken(const QString &text);

// When an API token stops working, from its "exp" claim; nullopt when it
// doesn't say (or isn't a JWT).
std::optional<QDateTime> apiTokenExpiry(const QString &token);

} // namespace pl::inat
