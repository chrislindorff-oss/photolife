#pragma once

#include <QList>
#include <QPointF>

namespace pl::geo {

// One geotagged photo: its coordinates, and `row` — the index into whatever
// model produced it, so a cluster can be turned back into model indices
// without this module knowing anything about that model.
struct Pin
{
    double lon = 0.0;
    double lat = 0.0;
    int row = -1;
};

// A group of pins close enough together on screen, at a given zoom, to be
// shown as a single marker. `centerLonLat` is the centroid of the pins it
// contains.
struct Cluster
{
    QPointF centerLonLat;
    QList<int> rows;
};

// Groups `pins` into on-screen clusters at `zoom`: any pin within
// `pixelRadius` screen pixels (at that zoom's Web Mercator scale) of a
// cluster's running centroid joins it, otherwise it starts a new one. Zooming
// in shrinks the pixel footprint of any given ground distance, so the same
// pins naturally separate into more, smaller clusters at higher zoom.
QList<Cluster> clusterPins(const QList<Pin> &pins, int zoom, double pixelRadius = 40.0);

} // namespace pl::geo
