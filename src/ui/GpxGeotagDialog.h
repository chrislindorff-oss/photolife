#pragma once

#include "geo/GpxTrack.h"

#include <QDateTime>
#include <QDialog>
#include <QList>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QSpinBox;

namespace pl {

class Application;

// Create Geotag from GPX for library photos: choose a track, and the camera
// clock's correction if it was off; it says live how many of the photos fall
// on the track. Nothing is written here: exec it, then read track() and
// offsetSeconds() if it returns Accepted.
class GpxGeotagDialog : public QDialog
{
    Q_OBJECT

public:
    // `taken`: each photo's time (local, as the camera said); invalid when
    // only its date is known.
    GpxGeotagDialog(Application &app, const QList<QDateTime> &taken, QWidget *parent = nullptr);

    const geo::GpxTrack &track() const { return m_track; }
    int offsetSeconds() const;
    // How many photos fall on the track with the current choices.
    int matches() const;

private:
    void choose();
    void refresh();

    Application &m_app;
    QList<QDateTime> m_taken;
    geo::GpxTrack m_track;
    QString m_path;
    QLabel *m_file = nullptr;
    QLabel *m_summary = nullptr;
    QComboBox *m_sign = nullptr;
    QSpinBox *m_hours = nullptr;
    QSpinBox *m_minutes = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace pl
