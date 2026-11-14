#pragma once

#include "geo/GeoBox.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace pl::geo {

// Named areas saved from the map (Shift-drag, then Places → Save Area as
// Place…): used as place:Name in the library search, as a trip's place, and
// in Targets. Kept in the settings as "name<TAB>swLat<TAB>swLng<TAB>neLat<TAB>neLng".
struct SavedPlace
{
    QString name;
    GeoBox box;
};

QList<SavedPlace> parseSavedPlaces(const QStringList &stored);
QStringList formatSavedPlaces(const QList<SavedPlace> &places);
// By name, case aside.
std::optional<SavedPlace> findSavedPlace(const QList<SavedPlace> &places, const QString &name);
// Adds `place`, replacing one of the same name (case aside); kept in name order.
QList<SavedPlace> withSavedPlace(QList<SavedPlace> places, const SavedPlace &place);

// "place:Anakie Gorge robin" -> the place "Anakie Gorge" and the rest,
// "robin". The name after place: is the longest saved name the text starts
// with (case aside), so names with spaces need no quotes; "place:\"x y\""
// works too. A name that isn't saved comes back in `unknown`.
struct PlaceQuery
{
    std::optional<SavedPlace> place;
    QString rest;      // the search without the place: part
    QString unknown;   // the name given, when no saved place has it
};
PlaceQuery extractPlace(const QString &search, const QList<SavedPlace> &places);

// The SQL condition for photos inside `box`, over columns `latColumn` and
// `lonColumn` (a box across the 180th meridian too). Numbers only, inline.
QString boxClause(const GeoBox &box, const QString &latColumn, const QString &lonColumn);

} // namespace pl::geo
