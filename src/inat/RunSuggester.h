#pragma once

#include "inat/PhotoIdentifier.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>

#include <optional>

namespace pl::net {
class INatClient;
}

namespace pl::inat {

// Suggest Names (Review → Needs Identifying): splits photos that have no
// guess into runs of shots -- a gap of more than `gapSeconds` between one
// and the next -- asks iNaturalist's computer vision about the middle photo
// of each run, one at a time, and records the top suggestion as every photo
// in the run's guess: a pending match (decided_by 'vision') that Match
// Library leaves alone. Nothing is confirmed; the photos move to To Confirm,
// grouped by the suggestion. One request per run, never per photo.
class RunSuggester : public QObject
{
    Q_OBJECT

public:
    struct Photo
    {
        qint64 captureId = 0;
        QString path;                     // its JPEG, or RAW (the embedded preview is sent)
        QDateTime taken;                  // invalid when only the day (or nothing) is known
        std::optional<QPointF> lonLat;    // x = longitude, y = latitude
        QDate day;
    };

    struct Result
    {
        int runs = 0;          // asked about
        int photos = 0;        // given a suggestion
        int noSuggestion = 0;  // runs iNaturalist had nothing for
        int failed = 0;        // runs that couldn't be identified
        bool cancelled = false;
        int status = 0;        // 401 (token expired), 403/429 (refused, busy): the run stopped
        QString error;         // the last error, if any
    };

    static constexpr int kDefaultGapSeconds = 30;

    RunSuggester(net::INatClient &inat, PhotoIdentifier &identifier, QString connectionName,
                 QObject *parent = nullptr);

    // The captures' best file, time and place, oldest first.
    static QList<Photo> loadPhotos(const QString &connectionName, const QList<qint64> &captureIds);
    // Consecutive photos (oldest first) no more than `gapSeconds` apart; a
    // photo without a time is a run of its own.
    static QList<QList<Photo>> splitRuns(QList<Photo> photos, int gapSeconds);

    // Records `taxonInatId` (already in the taxonomy) as the capture's
    // suggestion, replacing an earlier one. False if it isn't known.
    static bool recordSuggestion(const QString &connectionName, qint64 captureId, qint64 taxonInatId,
                                 double confidence, const QString &note);

    void start(const QList<qint64> &captureIds, int gapSeconds = kDefaultGapSeconds);
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    void progress(int runsDone, int runsTotal);
    void finished(const pl::inat::RunSuggester::Result &result);

private:
    void next();
    void identified(const QString &key, const std::optional<QRectF> &crop, const Identification &result);
    void record(const QList<Photo> &run, const net::VisionSuggestion &top);
    void finish();

    net::INatClient &m_inat;
    PhotoIdentifier &m_identifier;
    QString m_connectionName;
    QList<QList<Photo>> m_runs;
    int m_at = -1;
    QString m_askingKey;
    bool m_running = false;
    Result m_result;
};

} // namespace pl::inat

Q_DECLARE_METATYPE(pl::inat::RunSuggester::Result)
