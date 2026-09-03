#pragma once

#include <QString>
#include <QStringList>

namespace pl::match {

// How sure/precise a name is about which taxon it points at.
enum class Qualifier
{
    None,          // a plain binomial, or a bare genus
    Sp,            // "Genus sp" / "Genus sp." — genus-level, species unstated
    SpAff,         // "Genus sp. aff. epithet" / "Genus cf. epithet" — near a known species
    SensuLato,     // "s.l." — the broad circumscription
    SensuStricto,  // "s.s." — the narrow circumscription
    Aggregate,     // "spp. agg" / "agg." — a species aggregate
    Hybrid,        // "Genus a x b" — a named hybrid
    Undescribed,   // "Genus sp. 1" / "Genus sp. (Upright form)" — tagged but undescribed
    Unidentified,  // "Unidentified Genus" / "Genus indet."
};

// A scientific (or common-name-ish) name string broken into parts. Whatever the
// parser cannot place is left out; `raw` always holds the trimmed input.
struct ParsedName
{
    QString genus;
    QString specificEpithet;
    QString infraRank;      // normalised, no dot: "subsp" | "var" | "f" | ""
    QString infraEpithet;
    QStringList altGenera;  // "Corybas_Corysanthes" -> ["Corybas", "Corysanthes"]
    Qualifier qualifier = Qualifier::None;
    bool isHybrid = false;
    QString hybridEpithet2; // "Acacia paradoxa x stictophylla" -> "stictophylla"
    QString informalTag;    // "1" or "Upright form" for Undescribed
    QString raw;

    bool hasGenus() const { return !genus.isEmpty(); }
    bool hasSpecies() const { return !specificEpithet.isEmpty(); }
    bool isGenusOnly() const { return hasGenus() && !hasSpecies(); }

    // "genus epithet" lower-cased, for a taxon_name lookup. Empty when there is
    // no species epithet.
    QString foldedBinomial() const;

    // Best full-name rendering of what was parsed.
    QString canonical() const;

    bool operator==(const ParsedName &) const = default;
};

// Parses one name string (from a folder name or a file name's name field).
ParsedName parseName(const QString &raw);

// A quick "this is not a taxon" check for folder names: localities, staging
// folders, pure punctuation. Conservative — a false negative just means the
// resolver gets a shot and probably fails anyway.
bool looksNonTaxonomic(const QString &raw);

} // namespace pl::match
