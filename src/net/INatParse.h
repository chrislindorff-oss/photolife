#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QList>

class QJsonObject;

namespace pl::net::inat {

// Splits an iNaturalist "ancestry" string ("48460/47126/47217") into ids,
// root first.
QList<qint64> ancestryIds(const QString &ancestry);

// Parses one taxon object from any iNat endpoint. Vernacular and synonym names
// are pulled from an embedded "names" array when present.
pl::taxonomy::Taxon parseTaxon(const QJsonObject &obj);

// Parses one place object from places/autocomplete, deriving the bounding box
// from "bounding_box_geojson" when present.
pl::taxonomy::Place parsePlace(const QJsonObject &obj);

} // namespace pl::net::inat
