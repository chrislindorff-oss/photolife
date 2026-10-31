#include "ui/GeotagDialog.h"

#include "app/Application.h"
#include "db/Database.h"
#include "geo/LocalityStore.h"
#include "net/GeocodeClient.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"
#include "ui/LocationPickerMap.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace pl {

GeotagDialog::GeotagDialog(Application &app, const QList<Photo> &photos, const QList<Clue> &clues,
                           QWidget *parent)
    : QDialog(parent), m_app(app), m_photos(photos)
{
    setWindowTitle(tr("Set Location on Map"));

    // Left: the photos being placed.
    m_ownPhoto = new QLabel(this);
    m_ownPhoto->setAlignment(Qt::AlignCenter);
    m_ownPhoto->setMinimumSize(260, 200);
    m_ownPhoto->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_ownCaption = new QLabel(this);
    m_ownCaption->setAlignment(Qt::AlignCenter);
    m_ownCaption->setWordWrap(true);
    m_ownPrevious = new QToolButton(this);
    m_ownPrevious->setText(QStringLiteral("←"));
    m_ownNext = new QToolButton(this);
    m_ownNext->setText(QStringLiteral("→"));
    connect(m_ownPrevious, &QToolButton::clicked, this, [this] {
        m_ownIndex = std::max(0, m_ownIndex - 1);
        showOwnPhoto();
    });
    connect(m_ownNext, &QToolButton::clicked, this, [this] {
        m_ownIndex = std::min(int(m_photos.size()) - 1, m_ownIndex + 1);
        showOwnPhoto();
    });
    m_ownPrevious->setVisible(m_photos.size() > 1);
    m_ownNext->setVisible(m_photos.size() > 1);
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, [this] { showOwnPhoto(); });
    auto *steps = new QHBoxLayout;
    steps->addStretch(1);
    steps->addWidget(m_ownPrevious);
    steps->addWidget(m_ownNext);
    steps->addStretch(1);
    auto *left = new QVBoxLayout;
    left->addWidget(m_ownPhoto, 1);
    left->addWidget(m_ownCaption);
    left->addLayout(steps);

    // Right: finding the spot.
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search for a place, e.g. Poona Lake, Queensland"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::returnPressed, this, &GeotagDialog::search);
    m_results = new QComboBox(this);
    m_results->hide();
    connect(m_results, &QComboBox::activated, this, [this](int i) {
        const QPointF at = m_results->itemData(i).toPointF();
        m_map->centreOn(at.x(), at.y(), 14);
        m_map->setPin(at.x(), at.y());
        pinMoved(at.x(), at.y());
    });
    m_coordinates = new QLineEdit(this);
    m_coordinates->setPlaceholderText(tr("or paste coordinates: -25.954, 153.105"));
    m_coordinates->setToolTip(tr("Latitude, longitude in decimal degrees, as Google Maps and "
                                 "OpenStreetMap copy them. Press Enter to put the pin there."));
    connect(m_coordinates, &QLineEdit::returnPressed, this, [this] {
        const std::optional<QPointF> at = parseCoordinates(m_coordinates->text());
        if (!at) {
            m_place->setText(tr("That isn't a latitude, longitude pair."));
            return;
        }
        m_map->centreOn(at->x(), at->y(), std::max(m_map->zoom(), 14));
        m_map->setPin(at->x(), at->y());
        pinMoved(at->x(), at->y());
    });
    auto *find = new QHBoxLayout;
    find->addWidget(m_search, 3);
    find->addWidget(m_coordinates, 2);

    m_map = new LocationPickerMap(m_app.tileCache(), this);
    connect(m_map, &LocationPickerMap::pinMoved, this, &GeotagDialog::pinMoved);

    m_place = new QLabel(tr("Click the map where the photos were taken; drag to move around, "
                            "and the wheel zooms."),
                         this);
    m_place->setWordWrap(true);
    m_placeTimer = new QTimer(this);
    m_placeTimer->setSingleShot(true);
    m_placeTimer->setInterval(800);
    connect(m_placeTimer, &QTimer::timeout, this, &GeotagDialog::lookUpPlace);

    int located = 0;
    bool anyRaw = false;
    for (const Photo &p : m_photos) {
        located += p.latitude && p.longitude ? 1 : 0;
        anyRaw = anyRaw || p.isRaw;
    }
    m_replace = new QCheckBox(tr("Replace the location %n photo(s) already have", nullptr, located), this);
    m_replace->setChecked(true);
    m_replace->setVisible(located > 0);
    m_intoRaw = new QCheckBox(tr("Also write it into the RAW files themselves"), this);
    m_intoRaw->setToolTip(tr("A RAW's location always goes into an .xmp sidecar beside it. This "
                             "also writes it into NEF, CR2, ARW, DNG, ORF, PEF and RW2 files, "
                             "through a copy that replaces the original only once it reads back "
                             "intact (not CR3 or RAF)."));
    m_intoRaw->setChecked(m_app.settings().geotagWriteIntoRaw());
    m_intoRaw->setVisible(anyRaw);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)
        ->setText(tr("Set Location for %n Photo(s)", nullptr, int(m_photos.size())));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &GeotagDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *right = new QVBoxLayout;
    right->addLayout(find);
    right->addWidget(m_results);
    right->addWidget(m_map, 1);
    right->addWidget(m_place);
    right->addWidget(m_replace);
    right->addWidget(m_intoRaw);

    auto *columns = new QHBoxLayout;
    columns->addLayout(left, 2);
    columns->addLayout(right, 5);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(columns, 1);
    layout->addWidget(m_buttons);
    resize(1180, 760);

    // The dots: these photos' own locations, and the clues.
    QList<LocationPickerMap::Dot> dots;
    for (const Photo &p : m_photos) {
        if (p.latitude && p.longitude)
            dots << LocationPickerMap::Dot{*p.latitude, *p.longitude, p.label, true};
    }
    for (const Clue &c : clues) {
        const QString when = c.taken.isValid() ? QLocale().toString(c.taken, QLocale::ShortFormat) : QString();
        dots << LocationPickerMap::Dot{c.latitude, c.longitude,
                                       when.isEmpty() ? c.label : c.label + QStringLiteral(" · ") + when, false};
    }
    m_map->setDots(dots);

    // Where to start: the photo's own location; else the clue taken nearest
    // in time to it; else where the map was last left.
    const Photo &first = m_photos.value(0);
    if (first.latitude && first.longitude) {
        m_map->centreOn(*first.latitude, *first.longitude, 15);
        m_map->setPin(*first.latitude, *first.longitude);
        pinMoved(*first.latitude, *first.longitude);
    } else if (!clues.isEmpty()) {
        const Clue *nearest = &clues.first();
        if (first.taken.isValid()) {
            for (const Clue &c : clues) {
                if (c.taken.isValid()
                    && (!nearest->taken.isValid()
                        || std::abs(c.taken.secsTo(first.taken)) < std::abs(nearest->taken.secsTo(first.taken))))
                    nearest = &c;
            }
        }
        m_map->centreOn(nearest->latitude, nearest->longitude, 14);
        if (nearest->taken.isValid() && first.taken.isValid())
            m_place->setText(tr("The grey dots are other photos with a location; the map is on the one "
                                "taken nearest in time (%1). Double-click a dot to use it, or click the map.")
                                 .arg(nearest->label));
    } else if (const QPointF last = m_app.settings().geotagLastLocation(); !last.isNull()) {
        m_map->centreOn(last.y(), last.x(), m_app.settings().geotagLastZoom());
    }

    showOwnPhoto();
    updateOk();
}

std::optional<QPointF> GeotagDialog::parseCoordinates(const QString &text)
{
    // Two numbers, each optionally followed by its hemisphere letter.
    static const QRegularExpression pair(QStringLiteral(
        "^\\s*(-?\\d+(?:\\.\\d+)?)\\s*°?\\s*([NSns])?\\s*[,;\\s]\\s*(-?\\d+(?:\\.\\d+)?)\\s*°?\\s*([EWew])?\\s*$"));
    const QRegularExpressionMatch m = pair.match(text);
    if (!m.hasMatch())
        return std::nullopt;
    double lat = m.captured(1).toDouble();
    double lon = m.captured(3).toDouble();
    if (m.captured(2).compare(QLatin1String("S"), Qt::CaseInsensitive) == 0)
        lat = -std::abs(lat);
    if (m.captured(4).compare(QLatin1String("W"), Qt::CaseInsensitive) == 0)
        lon = -std::abs(lon);
    if (std::abs(lat) > 90 || std::abs(lon) > 180)
        return std::nullopt;
    return QPointF(lat, lon);
}

double GeotagDialog::latitude() const
{
    return m_map->pinLatitude();
}

double GeotagDialog::longitude() const
{
    return m_map->pinLongitude();
}

bool GeotagDialog::replaceExisting() const
{
    return m_replace->isVisible() ? m_replace->isChecked() : true;
}

bool GeotagDialog::writeIntoRaw() const
{
    return m_intoRaw->isVisible() && m_intoRaw->isChecked();
}

void GeotagDialog::accept()
{
    if (!m_map->hasPin())
        return;
    m_app.settings().setGeotagLastView(latitude(), longitude(), m_map->zoom());
    if (m_intoRaw->isVisible())
        m_app.settings().setGeotagWriteIntoRaw(m_intoRaw->isChecked());
    QDialog::accept();
}

void GeotagDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    showOwnPhoto();
}

void GeotagDialog::showOwnPhoto()
{
    if (m_photos.isEmpty())
        return;
    const Photo &p = m_photos.at(m_ownIndex);
    QString caption = m_photos.size() > 1
                          ? tr("%1 of %2: %3").arg(m_ownIndex + 1).arg(m_photos.size()).arg(p.label)
                          : p.label;
    if (p.taken.isValid())
        caption += QStringLiteral("\n") + QLocale().toString(p.taken, QLocale::ShortFormat);
    caption += QStringLiteral("\n")
               + (p.latitude && p.longitude ? tr("Now at %1, %2").arg(*p.latitude, 0, 'f', 5).arg(*p.longitude, 0, 'f', 5)
                                            : tr("No location yet"));
    m_ownCaption->setText(caption);
    m_ownPrevious->setEnabled(m_ownIndex > 0);
    m_ownNext->setEnabled(m_ownIndex + 1 < m_photos.size());
    thumb::ThumbnailCache &thumbs = m_app.thumbnails();
    QPixmap pm = thumbs.thumbnail(p.contentHash, p.path, thumb::ThumbnailCache::kPreviewPx);
    if (pm.isNull())
        pm = thumbs.thumbnail(p.contentHash, p.path, thumb::ThumbnailCache::kGridPx);
    if (pm.isNull()) {
        m_ownPhoto->setText(tr("loading…"));
        return;
    }
    const qreal dpr = devicePixelRatioF();
    QPixmap scaled = pm.scaled(m_ownPhoto->contentsRect().size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    m_ownPhoto->setPixmap(scaled);
}

void GeotagDialog::pinMoved(double latitude, double longitude)
{
    m_coordinates->setText(QStringLiteral("%1, %2").arg(latitude, 0, 'f', 6).arg(longitude, 0, 'f', 6));
    m_place->setText(tr("Finding the place…"));
    m_placeTimer->start();   // once the pin settles: OpenStreetMap allows one lookup a second
    updateOk();
}

void GeotagDialog::lookUpPlace()
{
    if (!m_map->hasPin())
        return;
    const double lat = latitude(), lon = longitude();
    const int request = ++m_placeRequest;
    auto show = [this](const QString &place) {
        m_place->setText(place.isEmpty() ? tr("No place name here — the photos will be named with just the date.")
                                         : tr("Place: %1").arg(place));
    };
    geo::LocalityStore store(m_app.database().connectionName());
    if (const std::optional<QString> cached = store.lookup(lat, lon)) {
        show(*cached);
        return;
    }
    QPointer<GeotagDialog> self(this);
    m_app.geocoder().reverseGeocode(lat, lon, [self, request, lat, lon, show](net::Outcome<QString> out) {
        if (!self || request != self->m_placeRequest)
            return;
        if (!out.ok()) {
            self->m_place->setText(tr("The place couldn't be looked up (%1).").arg(out.error));
            return;
        }
        geo::LocalityStore(self->m_app.database().connectionName()).upsertLocality(lat, lon, out.value);
        show(out.value);
    });
}

void GeotagDialog::search()
{
    const QString query = m_search->text().trimmed();
    if (query.isEmpty())
        return;
    // Coordinates typed into the search box work too.
    if (const std::optional<QPointF> at = parseCoordinates(query)) {
        m_coordinates->setText(query);
        emit m_coordinates->returnPressed();
        return;
    }
    m_place->setText(tr("Searching…"));
    QPointer<GeotagDialog> self(this);
    m_app.geocoder().search(query, [self, query](net::Outcome<QList<net::geocode::Place>> out) {
        if (!self)
            return;
        if (!out.ok()) {
            self->m_place->setText(tr("The search failed (%1).").arg(out.error));
            return;
        }
        if (out.value.isEmpty()) {
            self->m_place->setText(tr("Nothing found for “%1”.").arg(query));
            self->m_results->hide();
            return;
        }
        self->m_results->clear();
        for (const net::geocode::Place &p : out.value)
            self->m_results->addItem(p.name, QPointF(p.latitude, p.longitude));
        self->m_results->setVisible(out.value.size() > 1);
        emit self->m_results->activated(0);
    });
}

void GeotagDialog::updateOk()
{
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(m_map->hasPin() && !m_photos.isEmpty());
}

} // namespace pl
