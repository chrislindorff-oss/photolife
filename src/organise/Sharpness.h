#pragma once

#include <QImage>
#include <QString>

#include <optional>

namespace pl::organise {

// How sharp a photo looks, for picking the best of a burst: the variance of
// the Laplacian (edge strength) over the middle of the frame, where the
// subject usually is, on a grey copy about 1000 px across. Higher is sharper.
// The numbers only mean something next to each other -- the same scene, the
// same size -- so it's a hint for choosing between near-identical shots, not
// a verdict on any one photo.
double sharpness(const QImage &image);

// The same, for a photo file (a JPEG, else a RAW's embedded preview). Empty
// when it can't be read. Safe on a worker thread.
std::optional<double> sharpnessOfFile(const QString &path);

} // namespace pl::organise
