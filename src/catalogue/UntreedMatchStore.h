#pragma once

#include <QList>
#include <QString>
#include <QVariantList>

namespace pl::catalogue {

// One taxon that captures have been matched to (auto or confirmed) but that
// isn't a member of the reference tree(s) being checked.
struct UntreedTaxon
{
    qint64 inatId = 0;
    QString name;
    QString commonName;
    QString rank;
    int rankLevel = 0;
    int photoCount = 0;       // captures whose top match is this taxon
    int confirmedCount = 0;   // of those, how many are confirmed (the rest are auto)
};

// Finds matched taxa worth a second look: ones no reference tree contains
// (a subspecies/variety a tree doesn't list, or a species outside every
// tree's scope), which otherwise show up in no tree view and not in Review
// Unmatched either; and photos identified only to genus or higher.
//
// A capture counts toward the taxon of its *top* match, using the same rule
// as CaptureListModel (a user decision outranks the engine, then highest
// confidence), so the counts agree with the photo grid showing them.
//
// Read-only. Give it an open connection name; all calls run on the caller's
// thread.
class UntreedMatchStore
{
public:
    explicit UntreedMatchStore(QString connectionName);

    // projectId == 0: taxa in no reference tree at all. projectId > 0: taxa
    // not in that tree (they may be in others). Sorted by scientific name.
    // Returns an empty list on error (see error()).
    QList<UntreedTaxon> untreedTaxa(int projectId = 0) const;

    // Taxa above species rank (genus, family, ...) that captures' top matches
    // point at directly -- photos identified only to genus or higher --
    // whether or not any tree contains the taxon. Sorted by scientific name.
    QList<UntreedTaxon> aboveSpeciesTaxa() const;

    QString error() const { return m_error; }

private:
    QList<UntreedTaxon> matchedTaxa(const QString &condition, const QVariantList &binds) const;

    QString m_connectionName;
    mutable QString m_error;
};

} // namespace pl::catalogue
