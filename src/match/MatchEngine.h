#pragma once

#include <QList>
#include <QMetaType>
#include <QSet>
#include <QString>

#include <functional>

namespace pl::match {

// What the engine decided for one capture.
struct MatchOutcome
{
    qint64 captureId = 0;
    qint64 taxonId = 0;        // 0 = no taxon resolved
    QString matchedRank;       // rank the link was made at
    QString method;            // "folder+file" | "file" | "folder" | "alias" | "fuzzy"
    double confidence = 0.0;
    QString status;            // "auto" | "pending"
    QString qualifier;         // "sp" | "aff" | "s.l." | "s.s." | "agg" | "hybrid" | "undescribed" | "unidentified"
    QString note;

    bool hasTaxon() const { return taxonId > 0; }
};

// Ties captures to taxa using the folder classification (match::classifyFolders),
// the file-name grammar, and the taxonomy cache (match::TaxonResolver).
// Read model: evaluateCapture() decides without writing; matchAll() classifies
// the folders, evaluates every capture, and writes capture_match rows,
// preserving any the user has decided. matchGroup() does the same for a
// narrower set of captures, writing only results that land inside a group.
class MatchEngine
{
public:
    explicit MatchEngine(QString connectionName);

    struct Stats
    {
        int captures = 0;
        int autoApplied = 0;
        int pending = 0;
        int unmatched = 0;   // pending with no taxon
        int outsideGroup = 0;   // matchGroup() only: evaluated, but left unchanged
        bool cancelled = false;
        QString error;

        bool ok() const { return error.isEmpty(); }
    };

    using CancelFn = std::function<bool()>;
    using ProgressFn = std::function<void(int done, int total)>;

    void setAutoThreshold(double t) { m_autoThreshold = t; }
    void setPendingThreshold(double t) { m_pendingThreshold = t; }

    // `preferTaxonIds`: see ResolveHints::preferTaxonIds.
    MatchOutcome evaluateCapture(qint64 captureId, const QSet<qint64> &preferTaxonIds = {}) const;

    Stats matchAll(const CancelFn &cancel = {}, const ProgressFn &progress = {});

    // Re-checks only the captures an earlier pass left unresolved -- no match
    // row yet, pending (in Review Unmatched), or auto-matched above species
    // rank -- preferring taxa in `groupTaxonIds` (local taxon ids; see
    // ResolveHints::preferTaxonIds). A result is written only when its taxon
    // is in the group; everything else is left exactly as it was. User
    // decisions and species-level auto matches are never touched.
    Stats matchGroup(const QSet<qint64> &groupTaxonIds, const CancelFn &cancel = {},
                     const ProgressFn &progress = {});

    // Matches exactly these captures with the full library rules (e.g. the
    // photos an iNaturalist download just added), leaving every other capture
    // alone. User decisions among them are still preserved.
    Stats matchCaptures(const QList<qint64> &captureIds, const CancelFn &cancel = {},
                        const ProgressFn &progress = {});

private:
    // Shared body of matchAll()/matchGroup(): evaluates and writes each
    // capture (skipping user-decided ones). A non-empty `group` both steers
    // resolution and filters which results are written.
    Stats run(const QList<qint64> &captureIds, const QSet<qint64> &group, const CancelFn &cancel,
              const ProgressFn &progress);

    QString m_connectionName;
    double m_autoThreshold = 0.90;
    double m_pendingThreshold = 0.68;
};

} // namespace pl::match

Q_DECLARE_METATYPE(pl::match::MatchEngine::Stats)
