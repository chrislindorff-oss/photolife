#pragma once

#include "inat/ObservationGrouper.h"
#include "net/INatClient.h"

#include <QList>
#include <QObject>
#include <QString>

namespace pl::inat {

// Sends proposed observations to the user's iNaturalist account, one at a
// time: makes the observation, then adds each photo as a 2048 px JPEG
// rendered without its metadata. Every id iNaturalist gives back is written
// on the photo's capture straight away (capture.inat_observation_id and
// inat_photo_id), so a stop, a failure or a crash part-way leaves a record
// of exactly what's there, and the next try only sends what's missing.
//
// Never sends the same thing twice: a create that fails in a way the
// server may have acted on is looked up by its uuid before anything else.
class ObservationUploader : public QObject
{
    Q_OBJECT

public:
    struct Job
    {
        int key = 0;                       // what the caller calls it
        ProposedObservation observation;   // ready: no problems(), and a uuid
        QString geoprivacy;                // "open" | "obscured" | "private"
    };

    ObservationUploader(net::INatClient &inat, QString connectionName, QObject *parent = nullptr);

    void start(const QList<Job> &jobs);
    // Stops once the request in flight has its answer.
    void stop() { m_stop = true; }
    bool isRunning() const { return m_running; }

    static constexpr int kLongestEdge = 2048;   // the largest size iNaturalist keeps

    // These captures as photos to upload, from the catalogue: preview file,
    // date, location, best match (confirmed first; not a rejected one) and
    // what's already on iNaturalist. Missing ids are left out.
    // A photo that's an exact copy of one on iNaturalist comes back linked
    // to that observation.
    static QList<UploadPhoto> loadPhotos(const QString &connectionName, const QList<int> &captureIds);

    // Records these captures as of observation `observationId` without
    // sending them (the user said it's the same sighting): a negative
    // inat_photo_id, minus the capture's own id.
    static bool link(const QString &connectionName, const QList<int> &captureIds, qint64 observationId);

signals:
    void jobStarted(int key);
    // The observation exists on iNaturalist, and its photos say so.
    void observationMade(int key, qint64 observationId);
    void photoSent(int key, int captureId, qint64 photoId, int sent, int total);
    // `error` is empty when every photo got there.
    void jobFinished(int key, const QString &error, int status);
    // All done, or stopped -- `tokenExpired` when a 401 stopped it.
    void finished(bool tokenExpired);

private:
    void next();
    void create();
    void lookUp(const QString &failedError, int failedStatus);
    void made(qint64 observationId);
    void sendNextPhoto();
    void fail(const QString &error, int status);
    bool stamp(const QList<int> &captureIds, qint64 observationId, qint64 photoId = 0);

    net::INatClient &m_inat;
    QString m_connectionName;
    QList<Job> m_jobs;
    int m_index = -1;
    Job m_job;   // the one being sent
    bool m_running = false;
    bool m_stop = false;
};

} // namespace pl::inat
