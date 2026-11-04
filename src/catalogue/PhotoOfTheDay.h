#pragma once

#include <QDate>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace pl::catalogue {

// Nature Photo of the Day: one identified library photo a day, the same one
// all day. Best shots come first; once every one has had its day, any
// identified photo; once every one of those has too, it starts again. Only
// reads the catalogue -- what's been shown is kept by the caller (Settings),
// passed in and handed back.
class PhotoOfTheDay
{
public:
    struct State
    {
        QDate day;          // the day `captureId` was chosen for
        int captureId = 0;
        QList<int> shown;   // chosen on earlier days, this time round
    };

    struct Pick
    {
        int captureId = 0;
        QString path;            // its preview file
        QString contentHash;     // its thumbnail's key
        bool bestShot = false;
        qint64 taxonInatId = 0;
        QString name;            // scientific
        QString commonName;
        QString rank;
        QString capturedOn;      // as catalogued: a date, or an ISO date-time
        std::optional<double> latitude;
        std::optional<double> longitude;
        QString folder;          // its folder's path
        State state;             // to keep for next time
    };

    // Whether the photo's file can be shown on this computer (it exists,
    // and isn't kept online only). Default: it exists.
    using Available = std::function<bool(const QString &path)>;

    // Today's photo: the one in `state` when it was chosen today and still
    // qualifies, else a new one at random. Nothing when no photo qualifies:
    // identified (a confirmed or automatic match, not an unreviewed guess or
    // a rejected one), with a file that's available.
    static std::optional<Pick> choose(const QString &connectionName, const QDate &today, const State &state,
                                      const Available &available = {});

    struct LineageStep
    {
        qint64 inatId = 0;
        QString rank;         // empty when the catalogue doesn't know the taxon
        QString name;
        QString commonName;
        bool known() const { return !name.isEmpty(); }
    };
    // Life down to the taxon itself (its ancestry, then it), named from the
    // catalogue's taxa; a step it doesn't have comes back with only its id.
    static QList<LineageStep> lineage(const QString &connectionName, qint64 taxonInatId);
};

} // namespace pl::catalogue
