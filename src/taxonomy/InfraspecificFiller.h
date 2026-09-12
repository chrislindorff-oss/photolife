#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QList>
#include <QObject>

namespace pl::net {
class INatClient;
}

namespace pl::taxonomy {

class TaxonomyStore;

// Walks a reference tree's species one at a time, asking iNaturalist's taxonomy
// for each one's infraspecific children (subspecies / variety / form / hybrid)
// and adding the active ones to the project. When the project is scoped to a
// place, a child is only added if it has at least one verifiable observation
// there — so a cosmopolitan species doesn't drag in every subspecies worldwide,
// but a child that genuinely occurs in the region is never missed (which the
// old observations/species_counts approach was: that endpoint rolls every
// observation up to species rank and never reports infraspecific taxa).
//
// A request per species, plus one per candidate child when a place filter
// applies, so it can take a while on a large branch. Each species is marked as
// checked as soon as it's done, so a cancelled or interrupted run picks up
// where it left off next time; an explicitly scoped run re-checks regardless.
//
// Drive it from the thread that owns the INatClient / store connection.
class InfraspecificFiller : public QObject
{
    Q_OBJECT

public:
    InfraspecificFiller(pl::net::INatClient &inat, TaxonomyStore &store,
                        QObject *parent = nullptr);

    // Walks the project's species that still need an infraspecific check. When
    // `scopeInatId` > 0, only the species at or below that taxon in the tree,
    // and every one of them is re-checked (an explicit request overrides the
    // "already checked" flag).
    void start(int projectId, qint64 scopeInatId = 0);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

signals:
    void progress(int done, int total);
    void finished(bool ok, const QString &error, int added);

private:
    void fetchNextSpecies();
    void checkNextCandidate();
    void finishCurrentSpecies();
    void fail(const QString &error);
    void succeed();
    bool checkCancelled();

    pl::net::INatClient &m_inat;
    TaxonomyStore &m_store;

    bool m_running = false;
    bool m_cancelled = false;
    int m_projectId = -1;
    qint64 m_placeId = 0;   // 0 = no place filter (worldwide tree)

    QList<qint64> m_pending;   // species inat ids still to check
    int m_index = 0;
    int m_added = 0;

    // Per-species state while its candidate children are region-checked.
    qint64 m_currentSpecies = 0;
    QList<pl::taxonomy::Taxon> m_candidates;
    int m_candidateIndex = 0;
};

} // namespace pl::taxonomy
