#pragma once

#include "inat/InatObservationFetcher.h"

#include <QList>
#include <QSet>
#include <QString>

namespace pl::inat {

// Answers, for an iNaturalist observation's taxon, "is it in any reference
// tree?" and "do I already have photos of it in my library?" -- for the
// Download from iNaturalist page's NEW / NO TREE badges.
//
// In the library means either:
//  - some capture's top match (auto or confirmed) is this taxon or one below
//    it (a subspecies/variety photo counts for its species; a species photo
//    counts for its genus), or
//  - a capture with no such match has a filename naming it exactly, by
//    scientific or common name -- photos Match Library hasn't resolved yet.
// In a tree means the taxon itself is a member of at least one project.
//
// Snapshot semantics: load() reads the catalogue once; call it again (or make
// a new instance) to pick up later changes. Read-only; runs on the caller's
// thread.
class LibraryPresence
{
public:
    explicit LibraryPresence(QString connectionName);

    void load();

    bool inAnyTree(qint64 taxonInatId) const;
    bool inLibrary(qint64 taxonInatId, const QString &name, const QString &commonName) const;

    // load() plus filling each candidate's presence fields.
    static void annotate(const QString &connectionName, QList<Candidate> &candidates);

private:
    QString m_connectionName;
    QSet<qint64> m_treeTaxa;
    QSet<qint64> m_coveredTaxa;        // matched taxa and all their ancestors
    QSet<QString> m_unmatchedNames;    // folded name_text of captures with no match
};

} // namespace pl::inat
