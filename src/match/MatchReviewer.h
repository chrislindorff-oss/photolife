#pragma once

#include <QString>

namespace pl::match {

// Applies a reviewer's decisions to the catalogue: writes capture_match rows
// tagged decided_by='user' (which the engine then never overwrites) and learns
// name_alias entries so the same oddity is not queued again.
//
// Give it an open connection name; all calls run on the caller's thread.
class MatchReviewer
{
public:
    explicit MatchReviewer(QString connectionName);

    // Confirm the capture as `taxonInatId` (0 = keep whatever taxon the current
    // match already points at). When `learnAlias`, the capture's parsed name is
    // recorded as an alias for the taxon. Returns false if there is nothing to
    // confirm.
    bool confirm(qint64 captureId, qint64 taxonInatId = 0, bool learnAlias = true);

    // The capture has no acceptable match — drop it from the queue.
    bool reject(qint64 captureId);

    // Match at genus level (for "Genus sp." and friends).
    bool setGenusOnly(qint64 captureId, qint64 genusInatId);

    // Record "this is not a taxon" (a locality / staging folder). Scoped to the
    // capture's folder by default, or globally. Also reclassifies the folder.
    bool markNotATaxon(qint64 captureId, bool folderScope = true);

    // Re-open a decided capture so the engine will reconsider it.
    bool resetToPending(qint64 captureId);

    // Apply one taxon to every capture in a folder (optionally its subtree).
    // `onlyPending` leaves already-decided captures alone. Returns the count.
    int applyTaxonToFolder(int folderId, qint64 taxonInatId, bool recursive,
                           bool onlyPending = true);

    // Mark a folder subtree as "ignored" (staging) so its captures leave the
    // queue. Returns the folder count updated.
    int ignoreFolderTree(int folderId);

    QString error() const { return m_error; }

private:
    struct CaptureInfo
    {
        int folderId = 0;
        QString nameText;
        QString folderName;
        QString folderPath;
        bool found = false;
    };
    CaptureInfo captureInfo(qint64 captureId) const;
    bool writeDecision(qint64 captureId, qint64 taxonInatId, const QString &status,
                       const QString &matchedRank, const QString &qualifier);
    void learnAlias(const QString &rawText, qint64 taxonInatId, const QString &scope);
    bool fail(const QString &what);

    QString m_connectionName;
    mutable QString m_error;
};

} // namespace pl::match
