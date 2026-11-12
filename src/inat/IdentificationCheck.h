#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QList>
#include <QString>

namespace pl::inat {

// Check Identifications: your photos on iNaturalist, set against what the
// community has made of them since.

// An observation of yours that photos in the library are linked to (they
// were uploaded from here, or downloaded from it), with the name the photos
// have here.
struct LinkedObservation
{
    qint64 observationId = 0;
    QList<int> captureIds;
    QString previewPath;        // the first photo's, for a thumbnail
    QString previewHash;
    qint64 mineInatId = 0;      // the photos' taxon here; 0 when unnamed
    QString mineName;
    QString mineCommonName;
    QString mineRank;
    QList<qint64> mineAncestry; // root first, without the taxon itself
};

// Every observation photos in the catalogue are linked to. Reads only.
QList<LinkedObservation> linkedObservations(const QString &connectionName);

// How iNaturalist's name for an observation stands to yours.
enum class IdVerdict {
    Same,         // the same taxon
    Finer,        // iNaturalist's is within yours (a species where you had the genus)
    Coarser,      // iNaturalist's is above yours (the community backed off)
    Different,    // somewhere else in the tree
    YouHadNone,   // the photos have no name here; iNaturalist has one
    NoName,       // iNaturalist has no name for it (yet)
};
IdVerdict compareIds(const LinkedObservation &mine, const taxonomy::Observation &theirs);
QString verdictText(IdVerdict verdict);

} // namespace pl::inat
