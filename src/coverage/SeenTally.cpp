#include "coverage/SeenTally.h"

namespace pl::coverage {

namespace {

bool isSpecies(const QString &rank)
{
    return rank.compare(QLatin1String("species"), Qt::CaseInsensitive) == 0;
}

bool isBelowSpecies(const taxonomy::TreeNode &n)
{
    return n.isLeafRank && !isSpecies(n.rank);
}

} // namespace

QHash<qint64, SeenState> rollUpSeen(const QList<taxonomy::TreeNode> &flat, const ProjectCoverage &coverage,
                                    const QSet<qint64> &marked)
{
    QHash<qint64, SeenState> states;
    states.reserve(flat.size());
    // The first seen lower taxon beneath each taxon, from its children.
    QHash<qint64, qint64> lowerSeen;
    for (const taxonomy::TreeNode &n : flat) {
        SeenState s;
        s.photographed = coverage.byTaxon.value(n.inatId).subtreeHasPhotos;
        s.marked = marked.contains(n.inatId);
        states.insert(n.inatId, s);
    }
    // Children come after their parents: fold them in from the end.
    for (auto it = flat.crbegin(); it != flat.crend(); ++it) {
        const taxonomy::TreeNode &n = *it;
        SeenState &s = states[n.inatId];
        if (n.isLeafRank) {
            const qint64 via = lowerSeen.value(n.inatId);
            s.seen = s.photographed || s.marked || via != 0;
            if (via != 0 && !s.photographed && !s.marked)
                s.seenVia = via;
        }
        if (isSpecies(n.rank) && s.seen)
            s.speciesSeen += 1;
        if (!n.parentInatId || !states.contains(*n.parentInatId))
            continue;
        const qint64 parent = *n.parentInatId;
        states[parent].speciesSeen += s.speciesSeen;
        if (isBelowSpecies(n) && s.seen && !lowerSeen.contains(parent))
            lowerSeen.insert(parent, n.inatId);
    }
    return states;
}

SeenTotals tallySeen(const QList<taxonomy::TreeNode> &shown, const QHash<qint64, SeenState> &states)
{
    SeenTotals t;
    for (const taxonomy::TreeNode &n : shown) {
        const taxonomy::TreeNode *node = &n;
        if (!node->isLeafRank)
            continue;
        const SeenState s = states.value(n.inatId);
        if (isSpecies(node->rank)) {
            ++t.speciesShown;
            if (s.seen) {
                ++t.speciesSeen;
                if (s.photographed)
                    ++t.speciesPhotographed;
                else
                    ++t.speciesSeenOnly;
            }
        } else if (s.seen) {
            ++t.belowSeen;
        }
    }
    return t;
}

} // namespace pl::coverage
