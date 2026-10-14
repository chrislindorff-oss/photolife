#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace pl::collection {

// One kind of building block in an exported file's name.
enum class NameToken {
    ScientificName,
    CommonName,
    DateTaken,      // in FileNamePattern::dateFormat
    TimeTaken,      // HHmmss
    Year,
    Town,
    State,
    Country,
    Author,
    OriginalName,   // the library file's own name, without its extension
    Sequence,       // 001, 002, ... among photos that would otherwise share a name
    Text,           // fixed text typed by the user (NamePart::text)
};

struct NamePart
{
    NameToken token = NameToken::Text;
    QString text;   // NameToken::Text only

    bool operator==(const NamePart &other) const = default;
};

// What a photo's name can be built from, gathered from the catalogue.
struct NameFields
{
    QString scientificName;
    QString commonName;
    QDateTime captured;   // invalid when the photo has no date
    QString town;
    QString state;
    QString country;
    QString author;
    QString originalStem;
    int sequence = 0;     // 0 = leave the Sequence part out
};

// A user-defined naming protocol for the files ExportEngine writes: an
// ordered list of parts joined by a separator, e.g.
// "Caladenia_carnea_2020-01-01_Anglesea_001". An empty pattern means "keep
// each library file's own name" -- the export's original behaviour.
struct FileNamePattern
{
    QList<NamePart> parts;
    QString separator = QStringLiteral("_");
    QString dateFormat = QStringLiteral("yyyy-MM-dd");
    bool spacesToSeparator = false;   // "Caladenia carnea" -> "Caladenia_carnea"

    // Longest stem render() produces, leaving room for " (2)" and an
    // extension within common 255-character filename limits.
    static constexpr int kMaxStemLength = 200;

    bool isEmpty() const { return parts.isEmpty(); }
    bool uses(NameToken token) const;

    // The file name stem (no extension) for one photo. A part with no value
    // for this photo (no common name, no GPS locality, ...) is left out
    // along with its separator, and each part is made safe for a filename.
    // Falls back to fields.originalStem if nothing at all is left.
    QString render(const NameFields &fields) const;
    // render() without the fallback: empty when no part has a value.
    QString renderParts(const NameFields &fields) const;

    // Short human-readable form, e.g. "Scientific name _ Date taken _ Sequence".
    QString describe() const;

    QJsonObject toJson() const;
    static FileNamePattern fromJson(const QJsonObject &json);

    bool operator==(const FileNamePattern &other) const = default;

    // Every token a user can pick, in the order the naming dialog lists them.
    static QList<NameToken> availableTokens();
    static QString tokenLabel(NameToken token);
};

// geocode_cache.locality is "Town, State, Country" with any piece Nominatim
// didn't return left out (see net::geocode::formatLocality), so the pieces
// are told apart by count: three are town/state/country, two are
// state/country, one is the country. A heuristic -- a locality with a town
// but no state would read its town as the state.
struct LocalityParts
{
    QString town;
    QString state;
    QString country;
};
LocalityParts splitLocality(const QString &locality);

// captured_on as CatalogueWriter stores it (ISO 8601, with or without a time).
QDateTime parseCapturedOn(const QString &text);

} // namespace pl::collection
