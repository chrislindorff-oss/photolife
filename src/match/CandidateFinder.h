#pragma once

#include "match/TaxonResolver.h"

#include <QList>
#include <QString>

namespace pl::match {

// Supplies the review UI with the ranked taxon options for a capture and a
// free-text taxon search. Read-only; give it an open connection name.
class CandidateFinder
{
public:
    explicit CandidateFinder(QString connectionName);

    // Best-first candidates for a capture, from its file-name and folder names
    // resolved against the taxonomy cache (plus learned aliases).
    QList<TaxonCandidate> forCapture(qint64 captureId) const;

    // Free-text taxon search for the "search for a taxon" box.
    QList<TaxonCandidate> search(const QString &text, int limit = 25) const;

    // The genus the capture's folder implies, as an iNaturalist id (0 if none) —
    // backs the "Genus only" action.
    qint64 folderGenusInatId(qint64 captureId) const;

private:
    QString m_connectionName;
};

} // namespace pl::match
