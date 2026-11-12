#include "ui/GpxGeotagDialog.h"

#include "app/Application.h"
#include "settings/Settings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pl {

GpxGeotagDialog::GpxGeotagDialog(Application &app, const QList<QDateTime> &taken, QWidget *parent)
    : QDialog(parent), m_app(app), m_taken(taken)
{
    setWindowTitle(tr("Create Geotag from GPX"));
    m_file = new QLabel(tr("<i>No track chosen.</i>"), this);
    m_file->setWordWrap(true);
    auto *chooseButton = new QPushButton(tr("Choose…"), this);
    chooseButton->setAutoDefault(false);
    connect(chooseButton, &QPushButton::clicked, this, &GpxGeotagDialog::choose);
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(chooseButton);

    m_sign = new QComboBox(this);
    m_sign->addItem(tr("Add"), 1);
    m_sign->addItem(tr("Subtract"), -1);
    m_hours = new QSpinBox(this);
    m_hours->setRange(0, 23);
    m_hours->setSuffix(tr(" h"));
    m_minutes = new QSpinBox(this);
    m_minutes->setRange(0, 59);
    m_minutes->setSuffix(tr(" min"));
    for (QWidget *w : {static_cast<QWidget *>(m_hours), static_cast<QWidget *>(m_minutes)})
        connect(static_cast<QSpinBox *>(w), &QSpinBox::valueChanged, this, &GpxGeotagDialog::refresh);
    connect(m_sign, &QComboBox::currentIndexChanged, this, &GpxGeotagDialog::refresh);
    auto *clock = new QHBoxLayout;
    clock->addWidget(m_sign);
    clock->addWidget(m_hours);
    clock->addWidget(m_minutes);
    clock->addWidget(new QLabel(tr("to the photos' times"), this));
    clock->addStretch(1);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *form = new QFormLayout;
    form->addRow(tr("Track:"), fileRow);
    form->addRow(tr("Camera clock:"), clock);
    auto *intro = new QLabel(tr("Each photo gets the position the track recorded at the time it was taken, "
                                "between the points either side. Photos more than 5 minutes from any track point "
                                "are left as they are."),
                             this);
    intro->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(m_summary);
    layout->addStretch(1);
    layout->addWidget(m_buttons);
    setMinimumWidth(560);
    refresh();
}

int GpxGeotagDialog::offsetSeconds() const
{
    return m_sign->currentData().toInt() * (m_hours->value() * 3600 + m_minutes->value() * 60);
}

int GpxGeotagDialog::matches() const
{
    int n = 0;
    const int offset = offsetSeconds();
    for (const QDateTime &t : m_taken)
        n += t.isValid() && m_track.positionAt(t.addSecs(offset).toUTC()) ? 1 : 0;
    return n;
}

void GpxGeotagDialog::choose()
{
    QString start = m_path.isEmpty() ? m_app.settings().gpxTracksFolder() : QFileInfo(m_path).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a GPX Track"), start,
                                                      tr("GPS tracks (*.gpx *.GPX);;All files (*)"));
    if (path.isEmpty())
        return;
    QString error;
    m_track = geo::GpxTrack::load(path, &error);
    m_path = path;
    m_file->setText(m_track.isEmpty() ? tr("%1 can't be used: %2.").arg(QFileInfo(path).fileName().toHtmlEscaped(), error)
                                      : QDir::toNativeSeparators(path).toHtmlEscaped());
    refresh();
}

void GpxGeotagDialog::refresh()
{
    const int timed = int(std::count_if(m_taken.cbegin(), m_taken.cend(), [](const QDateTime &t) { return t.isValid(); }));
    const int untimed = int(m_taken.size()) - timed;
    QString text;
    if (!m_track.hasTimes()) {
        text = m_track.isEmpty() ? QString() : tr("This track has no times in it, so photos can't be matched to it.");
    } else {
        const QLocale locale;
        const QString range = tr("The track runs from %1 to %2 (this computer's time).")
                                  .arg(locale.toString(m_track.start().toLocalTime(), QStringLiteral("d MMM yyyy HH:mm")),
                                       locale.toString(m_track.end().toLocalTime(), QStringLiteral("d MMM yyyy HH:mm")));
        text = tr("<b>%1 of %n photo(s)</b> fall on it.", nullptr, int(m_taken.size())).arg(matches()) + QLatin1Char(' ') + range;
    }
    if (untimed > 0)
        text += QStringLiteral("<br>")
                + tr("%n of them have only a date (from the file name), not a time, so they can't be matched.",
                     nullptr, untimed);
    m_summary->setText(text);
    QPushButton *ok = m_buttons->button(QDialogButtonBox::Ok);
    const int n = m_track.hasTimes() ? matches() : 0;
    ok->setText(n > 0 ? tr("Geotag %n Photo(s)", nullptr, n) : tr("Geotag"));
    ok->setEnabled(n > 0);
}

} // namespace pl
