#pragma once

#include <QMetaType>
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
// preserving any the user has decided.
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
        bool cancelled = false;
        QString error;

        bool ok() const { return error.isEmpty(); }
    };

    using CancelFn = std::function<bool()>;
    using ProgressFn = std::function<void(int done, int total)>;

    void setAutoThreshold(double t) { m_autoThreshold = t; }
    void setPendingThreshold(double t) { m_pendingThreshold = t; }

    MatchOutcome evaluateCapture(qint64 captureId) const;

    Stats matchAll(const CancelFn &cancel = {}, const ProgressFn &progress = {});

private:
    QString m_connectionName;
    double m_autoThreshold = 0.90;
    double m_pendingThreshold = 0.68;
};

} // namespace pl::match

Q_DECLARE_METATYPE(pl::match::MatchEngine::Stats)
