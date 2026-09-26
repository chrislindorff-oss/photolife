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

// A decision the user made about a capture that its current filename now
// contradicts: the file was renamed since (or was decided before names were
// recorded), and the new name confidently resolves to a different taxon.
// The engine never overrides a user decision itself -- it reports these for
// the user to settle (MatchReviewer::confirm() or keepDecision()).
struct StaleDecision
{
    qint64 captureId = 0;
    QString baseName;          // the file's current name, without extension
    QString decidedName;       // the taxon the user chose
    qint64 suggestedInatId = 0;
    QString suggestedName;     // the taxon the current name resolves to
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
        QList<StaleDecision> staleDecisions;
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
    // decisions and species-level auto matches are never touched, though a
    // renamed user-decided capture is still checked for staleDecisions.
    Stats matchGroup(const QSet<qint64> &groupTaxonIds, const CancelFn &cancel = {},
                     const ProgressFn &progress = {});

    // Matches exactly these captures with the full library rules (e.g. the
    // photos an iNaturalist download just added), leaving every other capture
    // alone. User decisions among them are still preserved.
    Stats matchCaptures(const QList<qint64> &captureIds, const CancelFn &cancel = {},
                        const ProgressFn &progress = {});

private:
    // Shared body of matchAll()/matchGroup(): evaluates and writes each
    // capture (skipping user-decided ones, apart from checkUserDecision()).
    // A non-empty `group` both steers resolution and filters which results
    // are written.
    Stats run(const QList<qint64> &captureIds, const QSet<qint64> &group, const CancelFn &cancel,
              const ProgressFn &progress);

    // For a user-decided capture: if its name changed since the decision
    // (or was never recorded), re-evaluates it and either reports a
    // StaleDecision or, when the name still agrees, records the name so it
    // isn't re-checked. Never changes the decision itself.
    void checkUserDecision(qint64 captureId, const QSet<qint64> &group, Stats &stats) const;
    // True when local taxon `taxonId` is `ancestorId` or lies beneath it.
    bool isAtOrBelow(qint64 taxonId, qint64 ancestorId) const;

    QString m_connectionName;
    double m_autoThreshold = 0.90;
    double m_pendingThreshold = 0.68;
};

} // namespace pl::match

Q_DECLARE_METATYPE(pl::match::MatchEngine::Stats)
