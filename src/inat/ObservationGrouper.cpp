#include "inat/ObservationGrouper.h"

#include "geo/Distance.h"

#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace pl::inat {

std::optional<QPointF> ProposedObservation::where() const
{
    for (const UploadPhoto &p : photos)
        if (p.latitude && p.longitude)
            return QPointF(*p.longitude, *p.latitude);
    return std::nullopt;
}

namespace {

// The earliest dated photo, whatever order they're in.
const UploadPhoto *earliest(const QList<UploadPhoto> &photos)
{
    const UploadPhoto *first = nullptr;
    for (const UploadPhoto &p : photos)
        if (p.taken.isValid() && (!first || p.taken < first->taken))
            first = &p;
    return first;
}

} // namespace

QDateTime ProposedObservation::observedAt() const
{
    if (const UploadPhoto *p = earliest(photos))
        return p->taken;
    return when.value_or(QDateTime());
}

bool ProposedObservation::hasTime() const
{
    if (const UploadPhoto *p = earliest(photos))
        return p->hasTime;
    return when.has_value();
}

int ProposedObservation::problems() const
{
    int flags = 0;
    if (taxon.inatId <= 0)
        flags |= NoSpecies;
    if (!where())
        flags |= NoLocation;
    if (!observedAt().isValid())
        flags |= NoDate;
    return flags;
}

int ProposedObservation::remaining() const
{
    return int(std::count_if(photos.cbegin(), photos.cend(), [](const UploadPhoto &p) { return !p.done(); }));
}

namespace ObservationGrouper {
namespace {

void sortByTime(QList<UploadPhoto> &photos)
{
    std::stable_sort(photos.begin(), photos.end(), [](const UploadPhoto &a, const UploadPhoto &b) {
        if (a.taken.isValid() != b.taken.isValid())
            return a.taken.isValid();   // undated last
        return a.taken < b.taken;
    });
}

// Those already sent first, each part in the order it was.
void sentFirst(QList<UploadPhoto> &photos)
{
    std::stable_partition(photos.begin(), photos.end(), [](const UploadPhoto &p) { return p.done(); });
}

int sentCount(const ProposedObservation &g)
{
    int n = 0;
    while (n < g.photos.size() && g.photos.at(n).done())
        ++n;
    return n;
}

// A photo's notes for its observation: annotations, and the free text as the
// note, unless the observation has its own already.
void absorbNotes(ProposedObservation &g, const UploadPhoto &photo)
{
    if (g.annotations.isEmpty()) {
        for (const char *key : {"sex", "lifeStage", "alive", "flowers"}) {
            const QString value = photo.notes.value(QLatin1String(key));
            if (!value.isEmpty())
                g.annotations.insert(QLatin1String(key), value);
        }
    }
    if (g.note.isEmpty())
        g.note = photo.notes.value(QStringLiteral("notes"));
}

ProposedObservation startWith(const UploadPhoto &photo)
{
    ProposedObservation g;
    g.photos << photo;
    g.taxon = photo.taxon;
    g.observationId = photo.observationId;
    absorbNotes(g, photo);
    return g;
}

// Whether `p` is within `maxKm` of the last photo of `g` that has a
// location (or either has none).
bool closeEnough(const ProposedObservation &g, const UploadPhoto &p, double maxKm)
{
    if (maxKm <= 0 || !p.latitude || !p.longitude)
        return true;
    for (auto it = g.photos.crbegin(); it != g.photos.crend(); ++it)
        if (it->latitude && it->longitude)
            return geo::haversineKm(*it->latitude, *it->longitude, *p.latitude, *p.longitude) <= maxKm;
    return true;
}

} // namespace

QList<ProposedObservation> group(QList<UploadPhoto> photos, int gapSeconds, double maxKm)
{
    sortByTime(photos);
    QList<ProposedObservation> groups;
    QHash<qint64, int> byObservation;   // iNat observation id -> its group
    QHash<qint64, int> openFor;         // species -> its latest group of new photos
    for (const UploadPhoto &p : std::as_const(photos)) {
        if (p.observationId > 0) {
            const auto it = byObservation.constFind(p.observationId);
            if (it != byObservation.constEnd()) {
                groups[*it].photos << p;
                absorbNotes(groups[*it], p);
            } else {
                byObservation.insert(p.observationId, int(groups.size()));
                groups << startWith(p);
            }
            continue;
        }
        if (p.taken.isValid()) {
            const auto it = openFor.constFind(p.taxon.inatId);
            if (it != openFor.constEnd()) {
                ProposedObservation &g = groups[*it];
                const int gap = p.taxon.inatId > 0 ? gapSeconds : kNoSpeciesGapSeconds;
                if (g.photos.constLast().taken.secsTo(p.taken) <= gap && closeEnough(g, p, maxKm)) {
                    g.photos << p;
                    absorbNotes(g, p);
                    continue;
                }
            }
            openFor.insert(p.taxon.inatId, int(groups.size()));
        }
        groups << startWith(p);
    }
    for (ProposedObservation &g : groups)
        sentFirst(g.photos);
    // In the order taken.
    std::stable_sort(groups.begin(), groups.end(), [](const ProposedObservation &a, const ProposedObservation &b) {
        const QDateTime ta = a.observedAt(), tb = b.observedAt();
        if (ta.isValid() != tb.isValid())
            return ta.isValid();
        return ta < tb;
    });
    return groups;
}

QList<taxonomy::Taxon> speciesOf(const QList<ProposedObservation> &groups)
{
    QList<taxonomy::Taxon> taxa;
    QSet<qint64> seen;
    for (const ProposedObservation &g : groups)
        if (!seen.contains(g.taxon.inatId)) {
            seen.insert(g.taxon.inatId);
            taxa << g.taxon;
        }
    return taxa;
}

bool canMerge(const QList<ProposedObservation> &groups)
{
    if (groups.size() < 2)
        return false;
    QSet<qint64> made;
    for (const ProposedObservation &g : groups)
        if (g.observationId > 0)
            made.insert(g.observationId);
    return made.size() <= 1;
}

ProposedObservation merge(const QList<ProposedObservation> &groups, const taxonomy::Taxon &taxon)
{
    ProposedObservation merged;
    QStringList notes;
    for (const ProposedObservation &g : groups) {
        merged.photos << g.photos;
        if (g.observationId > 0) {
            merged.observationId = g.observationId;
            merged.uuid = g.uuid;
        }
        if (!merged.when && g.when)
            merged.when = g.when;
        if (!g.note.isEmpty() && !notes.contains(g.note))
            notes << g.note;
        if (merged.annotations.isEmpty())
            merged.annotations = g.annotations;
        if (g.taxon.inatId == taxon.inatId && g.taxonFromVision)
            merged.taxonFromVision = true;
    }
    sortByTime(merged.photos);
    sentFirst(merged.photos);
    merged.taxon = taxon;
    merged.note = notes.join(QLatin1Char('\n'));
    return merged;
}

bool canSplit(const ProposedObservation &group, int at)
{
    return group.observationId <= 0 && !group.uuidSent && at > 0 && at < group.photos.size();
}

QList<ProposedObservation> splitAt(const ProposedObservation &group, int at)
{
    if (!canSplit(group, at))
        return {group};
    ProposedObservation first = group, second = group;
    first.photos = group.photos.mid(0, at);
    second.photos = group.photos.mid(at);
    second.note.clear();
    second.uuid.clear();
    first.checked = second.checked = false;
    return {first, second};
}

bool canMove(const QList<ProposedObservation> &groups, int fromGroup, int from, int toGroup)
{
    if (fromGroup < 0 || fromGroup >= groups.size() || toGroup < 0 || toGroup >= groups.size())
        return false;
    const ProposedObservation &source = groups.at(fromGroup);
    if (from < 0 || from >= source.photos.size())
        return false;
    const UploadPhoto &photo = source.photos.at(from);
    // On iNaturalist (or recorded as part of it): it stays where it is.
    return !photo.done() && photo.observationId <= 0;
}

bool movePhoto(QList<ProposedObservation> &groups, int fromGroup, int from, int toGroup, int to)
{
    if (!canMove(groups, fromGroup, from, toGroup))
        return false;
    UploadPhoto photo = groups[fromGroup].photos.takeAt(from);
    ProposedObservation &target = groups[toGroup];
    if (fromGroup != toGroup)
        photo.taxon = target.taxon;
    to = std::clamp(to, sentCount(target), int(target.photos.size()));
    target.photos.insert(to, photo);
    if (fromGroup != toGroup) {   // reordering doesn't change what's looked for
        target.checked = false;
        groups[fromGroup].checked = false;
    }
    if (groups.at(fromGroup).photos.isEmpty())
        groups.removeAt(fromGroup);
    return true;
}

} // namespace ObservationGrouper

QList<QPair<int, int>> annotationIds(const QHash<QString, QString> &annotations)
{
    // iNaturalist's controlled terms (GET /v1/controlled_terms).
    static const QHash<QString, QPair<int, QHash<QString, int>>> terms = {
        {QStringLiteral("lifeStage"),
         {1, {{QStringLiteral("Adult"), 2}, {QStringLiteral("Teneral"), 3}, {QStringLiteral("Pupa"), 4},
              {QStringLiteral("Nymph"), 5}, {QStringLiteral("Larva"), 6}, {QStringLiteral("Egg"), 7},
              {QStringLiteral("Juvenile"), 8}, {QStringLiteral("Subimago"), 16}}}},
        {QStringLiteral("sex"),
         {9, {{QStringLiteral("Female"), 10}, {QStringLiteral("Male"), 11}, {QStringLiteral("Cannot Be Determined"), 20}}}},
        {QStringLiteral("flowers"),
         {12, {{QStringLiteral("Flowers"), 13}, {QStringLiteral("Fruits or Seeds"), 14}, {QStringLiteral("Flower Buds"), 15},
               {QStringLiteral("No Flowers or Fruits"), 21}}}},
        {QStringLiteral("alive"),
         {17, {{QStringLiteral("Alive"), 18}, {QStringLiteral("Dead"), 19}, {QStringLiteral("Cannot Be Determined"), 20}}}},
    };
    QList<QPair<int, int>> out;
    for (const char *key : {"lifeStage", "sex", "alive", "flowers"}) {
        const auto term = terms.constFind(QLatin1String(key));
        const QString value = annotations.value(QLatin1String(key));
        if (term == terms.cend() || value.isEmpty())
            continue;
        if (const auto v = term->second.constFind(value); v != term->second.cend())
            out.append({term->first, *v});
    }
    return out;
}

QString annotationsText(const QHash<QString, QString> &annotations)
{
    QStringList parts;
    for (const char *key : {"sex", "lifeStage", "alive", "flowers"}) {
        const QString value = annotations.value(QLatin1String(key));
        if (!value.isEmpty())
            parts << value;
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace pl::inat
