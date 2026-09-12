#pragma once

#include <QList>
#include <QString>

namespace pl::catalogue {

// The user's hand-picked "best shots": captures they have starred as a
// favourite photo of the species the capture is identified as. Any number of
// captures may be starred, and the star follows the capture's live
// identification (the species is never stored here — it is read back from
// capture_match at query time). Global to the catalogue, like capture_match.
//
// Nothing here touches files on disk. Give it an open connection name; all
// calls run on the caller's thread.
class BestShotStore
{
public:
    explicit BestShotStore(QString connectionName);

    // Stars (nominate = true) or unstars (nominate = false) each capture.
    // Idempotent: starring an already-starred capture, or unstarring one that
    // isn't starred, is a no-op. Runs in one transaction. Returns the number of
    // best_shot rows actually added or removed, or -1 on error (see error()).
    int setBestShots(const QList<int> &captureIds, bool nominate);

    bool isBestShot(int captureId) const;

    QString error() const { return m_error; }

private:
    QString m_connectionName;
    mutable QString m_error;
};

} // namespace pl::catalogue
