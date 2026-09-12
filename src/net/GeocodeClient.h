#pragma once

#include "net/INatClient.h"   // for Outcome<T>, reused the same way ReferencePhotoFetcher does

#include <QObject>

#include <functional>

namespace pl::net {

class HttpClient;

// Typed wrapper around OpenStreetMap Nominatim's reverse-geocoding endpoint.
// Goes through the given HttpClient, so it inherits whatever rate limiting
// that instance is configured with -- Application gives GeocodeClient its own
// HttpClient, throttled to Nominatim's usage policy (max 1 request/second),
// independent of the iNaturalist traffic on the app's other HttpClient.
class GeocodeClient : public QObject
{
    Q_OBJECT

public:
    explicit GeocodeClient(HttpClient &http, QObject *parent = nullptr);

    // Reverse-geocodes one coordinate to a short "Town, State, Country" style
    // locality string. An empty value (not an error) means Nominatim had no
    // address for that point, e.g. open ocean.
    void reverseGeocode(double lat, double lon, std::function<void(Outcome<QString>)> done);

private:
    HttpClient &m_http;
};

} // namespace pl::net
