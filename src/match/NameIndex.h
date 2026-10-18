#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include <memory>
#include <optional>

namespace pl::match {

// Every common (vernacular) name in the taxonomy cache, held in memory so a
// whole library can be matched against it quickly -- by its folded spelling,
// and by a "loose" key that ignores hyphens, spaces and apostrophes, so
// "Superb Fairy-wren" finds "Superb Fairywren" and "Lathams Snipe" finds
// "Latham's Snipe". Read-only once built; build one per matching pass.
class NameIndex
{
public:
    struct Entry
    {
        qint64 taxonId = 0;
        qint64 inatId = 0;
        qint64 parentInatId = 0;
        QString name;          // accepted scientific name
        QString rank;
        QString commonName;    // the taxon's preferred common name
        QString vernacular;    // the common name that matched
    };

    // All common names on the open connection.
    static std::shared_ptr<const NameIndex> build(const QString &connectionName);

    // Folded (TaxonomyStore::foldName) lower-case letters and digits only.
    static QString looseKey(const QString &text);

    // The taxa with a common name spelled `text` (folded); failing that, the
    // ones whose name matches loosely. `loose` says which.
    QList<Entry> lookup(const QString &text, bool *loose = nullptr) const;

    // The one taxon a set of hits means, if they mean one: a single taxon, or
    // a species together with only its own subspecies/varieties (which often
    // share its common name).
    static std::optional<Entry> unique(const QList<Entry> &hits);

    // Species and below: where a common name reliably means one taxon.
    static bool isSpeciesOrBelow(const QString &rank);

    int size() const { return int(m_byFolded.size()); }

private:
    QHash<QString, QList<Entry>> m_byFolded;
    QHash<QString, QList<Entry>> m_byLoose;
};

} // namespace pl::match
