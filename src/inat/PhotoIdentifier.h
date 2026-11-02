#pragma once

#include "net/INatClient.h"

#include <QDate>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>

#include <optional>

namespace pl::inat {

// One photo (or part of it) to identify.
struct IdentifyRequest
{
    QString key;                     // what the caller calls it (a path, a capture id)
    QString path;                    // the file: a JPEG, or a RAW (its embedded preview is used)
    std::optional<QRectF> crop;      // just this part, in 0..1 of the picture
    std::optional<QPointF> lonLat;   // where it was taken (x = longitude, y = latitude)
    std::optional<QDate> date;       // when
};

// What iNaturalist made of it.
struct Identification
{
    net::VisionResult result;
    QSet<qint64> inTrees;   // the suggestions' iNat ids that are in a reference tree
    QString error;          // empty when it worked
    int status = 0;         // the HTTP status: 401 (token expired) and 403 (refused) matter

    bool ok() const { return error.isEmpty(); }
};

// Asks iNaturalist's computer vision what a photo shows: renders it to a
// 1280 px JPEG on a worker thread (cropped first, at more detail, when only
// part of it is wanted), sends it with where and when it was taken, and
// notes which suggestions are in the user's trees. One answer per photo and
// crop is kept for the session, so asking again is instant. Read-only: the
// catalogue isn't changed.
class PhotoIdentifier : public QObject
{
    Q_OBJECT

public:
    PhotoIdentifier(net::INatClient &inat, QString connectionName, QObject *parent = nullptr);

    // Answers through finished(), straight away when it's already known.
    void identify(const IdentifyRequest &request);
    std::optional<Identification> known(const QString &key, const std::optional<QRectF> &crop) const;

    // The picture sent for `request` -- exposed for tests.
    static QByteArray renderJpeg(const IdentifyRequest &request);

    static constexpr int kLongestEdge = 1280;

signals:
    void finished(const QString &key, const std::optional<QRectF> &crop, const pl::inat::Identification &result);

private:
    static QString cacheKey(const QString &key, const std::optional<QRectF> &crop);
    void score(const IdentifyRequest &request, const QByteArray &jpeg);

    net::INatClient &m_inat;
    QString m_connectionName;
    QHash<QString, Identification> m_known;
};

} // namespace pl::inat
