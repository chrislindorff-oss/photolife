#pragma once

#include <QSet>
#include <QString>
#include <QtGlobal>

namespace pl::catalogue {

// The taxa the user has ticked as seen but not (yet) photographed -- the hand
// ticks in the Reference Tree's Seen column. A photographed taxon counts as
// seen anyway; that comes from its photos, so it isn't stored here. Keyed by
// iNaturalist id: a tick holds in every tree that has the taxon. Global to
// the catalogue, like best_shot.
//
// Give it an open connection name; all calls run on the caller's thread.
class SeenStore
{
public:
    explicit SeenStore(QString connectionName);

    // Every taxon ticked as seen (iNat ids). Empty on error (see error()).
    QSet<qint64> seenTaxa() const;

    // Ticks (seen = true) or unticks a taxon. Idempotent. False on error.
    bool setSeen(qint64 inatId, bool seen);

    QString error() const { return m_error; }

private:
    QString m_connectionName;
    mutable QString m_error;
};

} // namespace pl::catalogue
