#pragma once

#include "match/NameParser.h"

#include <QList>
#include <QString>

namespace pl::match {

// A taxon the resolver thinks a name might refer to.
struct TaxonCandidate
{
    qint64 taxonId = 0;    // local taxon.id
    qint64 inatId = 0;
    QString name;          // accepted scientific name
    QString rank;
    QString commonName;
    QString matchedVia;    // "accepted" | "synonym" | "vernacular" | "misspelling" | "fuzzy" | "alias"
    double score = 0.0;    // 0..1

    bool operator==(const TaxonCandidate &) const = default;
};

// Folder-derived context that sharpens (or is expected to disagree with) a match.
struct ResolveHints
{
    QString genus;        // folder's genus
    QString family;       // folder's family — an ancestry hint only (may be pre-APG)
    QString folderPath;   // for folder-scoped aliases
};

// Resolves a parsed name against the cached taxonomy (taxon / taxon_name) and
// the learned name_alias table. Read-only; give it an open connection name.
class TaxonResolver
{
public:
    explicit TaxonResolver(QString connectionName);

    // Best-first candidates (possibly empty). Exact accepted/synonym/vernacular
    // hits rank above fuzzy hits; a fuzzy hit's score is capped below 0.75.
    QList<TaxonCandidate> resolve(const ParsedName &parsed, const ResolveHints &hints = {}) const;

    // The user has recorded this raw text as "not a taxon" for this folder or
    // globally (a locality or staging folder).
    bool isMarkedNonTaxon(const QString &rawText, const QString &folderPath = {}) const;

    void setFuzzyThreshold(double t) { m_fuzzyThreshold = t; }

private:
    QList<TaxonCandidate> exactCandidates(const ParsedName &parsed) const;
    QList<TaxonCandidate> fuzzyCandidates(const ParsedName &parsed) const;
    void applyHints(QList<TaxonCandidate> &candidates, const ResolveHints &hints) const;

    QString m_connectionName;
    double m_fuzzyThreshold = 0.80;
};

} // namespace pl::match
