#include "ui/InatObservationMatchDialog.h"

#include "net/INatClient.h"
#include "net/PhotoCache.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace pl {
namespace {

QIcon makePlaceholder()
{
    QPixmap pm(128, 128);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 28));
    p.drawRoundedRect(pm.rect().adjusted(12, 12, -12, -12), 8, 8);
    p.end();
    return QIcon(pm);
}

QString previewUrl(const taxonomy::Observation &obs)
{
    return obs.photos.isEmpty() ? QString() : obs.photos.first().previewUrl;
}

constexpr int kLargePx = 300;

} // namespace

InatObservationMatchDialog::InatObservationMatchDialog(net::INatClient &inat, net::PhotoCache &photos,
                                                       const QString &userLogin,
                                                       const Selection &selection, QWidget *parent)
    : QDialog(parent), m_inat(inat), m_photos(photos), m_userLogin(userLogin), m_selection(selection)
{
    setWindowTitle(tr("Match with an iNaturalist Observation"));
    setModal(true);
    resize(860, 560);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_candidates = new QListWidget(this);
    m_candidates->setViewMode(QListView::IconMode);
    m_candidates->setIconSize(QSize(128, 128));
    m_candidates->setGridSize(QSize(150, 175));
    m_candidates->setWordWrap(true);
    m_candidates->setResizeMode(QListView::Adjust);
    m_candidates->setMovement(QListView::Static);
    m_candidates->setSpacing(6);
    connect(m_candidates, &QListWidget::currentRowChanged, this, [this] { showDetails(); });

    // The chosen observation.
    m_photo = new QLabel(this);
    m_photo->setAlignment(Qt::AlignCenter);
    m_photo->setFixedSize(kLargePx, kLargePx);
    m_species = new QLabel(this);
    m_species->setWordWrap(true);
    m_species->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_coordinates = new QLabel(this);
    m_coordinates->setWordWrap(true);
    m_coordinates->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_link = new QLabel(this);
    m_link->setOpenExternalLinks(true);

    m_applyHeading = new QLabel(this);
    m_applyCoordinates = new QCheckBox(tr("Coordinates"), this);
    m_applySpecies = new QCheckBox(tr("Species"), this);
    m_replace = new QCheckBox(
        tr("Also replace those already on %n photo(s)", nullptr, m_selection.withGps), this);
    m_replace->setVisible(m_selection.withGps > 0);
    m_replace->setToolTip(tr("Left unticked, photos that already have coordinates keep them."));
    connect(m_applyCoordinates, &QCheckBox::toggled, this, [this] { updateApplyButton(); });
    connect(m_applySpecies, &QCheckBox::toggled, this, [this] { updateApplyButton(); });

    auto *details = new QVBoxLayout;
    details->addWidget(m_photo, 0, Qt::AlignHCenter);
    details->addWidget(m_species);
    details->addWidget(m_coordinates);
    details->addWidget(m_link);
    details->addSpacing(8);
    details->addWidget(m_applyHeading);
    details->addWidget(m_applySpecies);
    details->addWidget(m_applyCoordinates);
    auto *indent = new QHBoxLayout;
    indent->addSpacing(20);
    indent->addWidget(m_replace);
    details->addLayout(indent);
    details->addStretch(1);
    auto *detailsWidget = new QWidget(this);
    detailsWidget->setLayout(details);
    detailsWidget->setFixedWidth(kLargePx + 60);

    auto *middle = new QHBoxLayout;
    middle->addWidget(m_candidates, 1);
    middle->addWidget(detailsWidget);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addLayout(middle, 1);
    layout->addWidget(m_buttons);

    connect(&m_photos, &net::PhotoCache::ready, this,
            [this](const QString &url) { onPhotoReady(url); });

    showDetails();
    m_status->setText(tr("Searching your iNaturalist observations…"));
    runSearch(1);
}

std::optional<taxonomy::Observation> InatObservationMatchDialog::chosenObservation() const
{
    const int row = m_candidates->currentRow();
    if (row < 0 || row >= m_observations.size())
        return std::nullopt;
    return m_observations.at(row);
}

bool InatObservationMatchDialog::applyCoordinates() const
{
    return m_applyCoordinates->isEnabled() && m_applyCoordinates->isChecked();
}

bool InatObservationMatchDialog::applySpecies() const
{
    return m_applySpecies->isEnabled() && m_applySpecies->isChecked();
}

bool InatObservationMatchDialog::replaceExistingCoordinates() const
{
    return applyCoordinates() && m_replace->isChecked();
}

QList<taxonomy::Observation> InatObservationMatchDialog::rankCandidates(QList<taxonomy::Observation> candidates,
                                                                       qint64 preferredTaxon,
                                                                       const QDate &centre)
{
    const auto distance = [&centre](const taxonomy::Observation &o) {
        const QDate d = QDate::fromString(o.observedOn, Qt::ISODate);
        return d.isValid() && centre.isValid() ? std::abs(d.daysTo(centre)) : 1000000;
    };
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](const taxonomy::Observation &a, const taxonomy::Observation &b) {
                         const bool pa = preferredTaxon > 0 && a.taxonInatId == preferredTaxon;
                         const bool pb = preferredTaxon > 0 && b.taxonInatId == preferredTaxon;
                         if (pa != pb)
                             return pa;
                         const qint64 da = distance(a), db = distance(b);
                         if (da != db)
                             return da < db;
                         return a.id > b.id;
                     });
    return candidates;
}

QString InatObservationMatchDialog::speciesText(const taxonomy::Observation &obs)
{
    if (obs.taxonName.isEmpty())
        return obs.taxonCommonName;
    QString text = obs.taxonCommonName.isEmpty()
                       ? obs.taxonName
                       : QStringLiteral("%1 (%2)").arg(obs.taxonCommonName, obs.taxonName);
    if (!obs.taxonRank.isEmpty())
        text += QStringLiteral(" · ") + obs.taxonRank;
    return text;
}

void InatObservationMatchDialog::runSearch(int page)
{
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    // A day either side, the same window InatObservationFetcher::looksLikeDuplicate()
    // uses to decide two records plausibly describe the same sighting.
    const QDate from = m_selection.from.addDays(-1);
    const QDate to = m_selection.to.addDays(1);
    QPointer<InatObservationMatchDialog> self(this);
    m_inat.fetchObservations(
        m_userLogin, {}, 0, from, to, page,
        [self, page](net::Outcome<net::ObservationPage> out) {
            if (!self)
                return;
            if (!out.ok()) {
                self->m_status->setText(tr("Search failed: %1").arg(out.error));
                self->populateCandidates();
                return;
            }
            self->m_found += out.value.results;
            if (page < kMaxPages && !out.value.results.isEmpty()
                && self->m_found.size() < out.value.totalResults) {
                self->runSearch(page + 1);
                return;
            }
            const QDate centre = self->m_selection.from.addDays(self->m_selection.from.daysTo(self->m_selection.to) / 2);
            self->m_found = rankCandidates(self->m_found, self->m_selection.currentTaxonInatId, centre);
            if (self->m_found.isEmpty())
                self->m_status->setText(tr("None of your iNaturalist observations are from within a "
                                           "day of these photos."));
            else
                self->m_status->setText(
                    tr("%n observation(s) of yours from around then — pick the one these photos "
                       "match, if any, then tick what to take from it.",
                       nullptr, int(self->m_found.size())));
            self->populateCandidates();
        });
}

void InatObservationMatchDialog::populateCandidates()
{
    m_observations = m_found;
    m_candidates->clear();

    for (int i = 0; i < m_observations.size(); ++i) {
        const taxonomy::Observation &obs = m_observations.at(i);
        const QString date = obs.observedOn.isEmpty() ? tr("(no date)") : obs.observedOn;
        const QString name = !obs.taxonCommonName.isEmpty() ? obs.taxonCommonName
                             : !obs.taxonName.isEmpty()     ? obs.taxonName
                                                            : tr("Unknown");
        auto *item = new QListWidgetItem(makePlaceholder(), date + QLatin1Char('\n') + name, m_candidates);
        item->setToolTip(speciesText(obs));
        const QPixmap pm = m_photos.photo(previewUrl(obs));
        if (!pm.isNull())
            item->setIcon(QIcon(pm));
    }
    if (!m_observations.isEmpty())
        m_candidates->setCurrentRow(0);
    showDetails();
}

QString InatObservationMatchDialog::largeUrl(const taxonomy::Observation &obs) const
{
    // iNat's photo URLs name their size: the medium one is sharper here.
    QString url = previewUrl(obs);
    return url.replace(QStringLiteral("/small."), QStringLiteral("/medium."));
}

void InatObservationMatchDialog::showDetails()
{
    const std::optional<taxonomy::Observation> chosen = chosenObservation();
    const bool have = chosen.has_value();
    m_applyHeading->setText(
        tr("<b>Apply to the %n selected photo(s):</b>", nullptr, m_selection.photos));
    if (!have) {
        m_photo->clear();
        m_species->setText(QString());
        m_coordinates->setText(QString());
        m_link->clear();
        m_applySpecies->setText(tr("Species"));
        m_applySpecies->setEnabled(false);
        m_applyCoordinates->setText(tr("Coordinates"));
        m_applyCoordinates->setEnabled(false);
        m_replace->setEnabled(false);
        updateApplyButton();
        return;
    }
    const taxonomy::Observation &obs = *chosen;
    showPhoto();

    const bool hasTaxon = obs.taxonInatId > 0 && !obs.taxonName.isEmpty();
    const bool hasCoords = obs.latitude && obs.longitude;
    m_species->setText(hasTaxon ? tr("<b>Species:</b> %1").arg(speciesText(obs).toHtmlEscaped())
                                : tr("<b>Species:</b> not identified"));
    m_coordinates->setText(
        hasCoords ? tr("<b>Coordinates:</b> %1, %2%3")
                        .arg(*obs.latitude, 0, 'f', 5)
                        .arg(*obs.longitude, 0, 'f', 5)
                        .arg(obs.placeGuess.isEmpty() ? QString()
                                                      : QStringLiteral("<br>") + obs.placeGuess.toHtmlEscaped())
                  : tr("<b>Coordinates:</b> none on this observation"));
    m_link->setText(QStringLiteral("<a href=\"https://www.inaturalist.org/observations/%1\">%2</a>")
                        .arg(obs.id)
                        .arg(tr("Open observation %1 on iNaturalist").arg(obs.id)));

    m_applySpecies->setText(hasTaxon ? tr("Species: %1").arg(!obs.taxonCommonName.isEmpty() ? obs.taxonCommonName
                                                                                            : obs.taxonName)
                                     : tr("Species"));
    m_applySpecies->setEnabled(hasTaxon);
    m_applySpecies->setChecked(hasTaxon
                               && (m_selection.anyNeedsSpecies
                                   || obs.taxonInatId != m_selection.currentTaxonInatId));
    m_applyCoordinates->setText(hasCoords ? tr("Coordinates: %1, %2")
                                                .arg(*obs.latitude, 0, 'f', 5)
                                                .arg(*obs.longitude, 0, 'f', 5)
                                          : tr("Coordinates"));
    m_applyCoordinates->setEnabled(hasCoords);
    m_applyCoordinates->setChecked(hasCoords && m_selection.withGps < m_selection.photos);
    m_replace->setEnabled(hasCoords);
    updateApplyButton();
}

void InatObservationMatchDialog::showPhoto()
{
    const std::optional<taxonomy::Observation> chosen = chosenObservation();
    if (!chosen)
        return;
    // The medium size when it's here, the small one meanwhile.
    QPixmap pm = m_photos.photo(largeUrl(*chosen));
    if (pm.isNull())
        pm = m_photos.photo(previewUrl(*chosen));
    if (pm.isNull())
        m_photo->setText(previewUrl(*chosen).isEmpty() ? tr("(no photo)") : tr("loading…"));
    else
        m_photo->setPixmap(pm.scaled(m_photo->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void InatObservationMatchDialog::updateApplyButton()
{
    m_replace->setEnabled(m_applyCoordinates->isEnabled() && m_applyCoordinates->isChecked());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(chosenObservation().has_value()
                                                        && (applyCoordinates() || applySpecies()));
}

void InatObservationMatchDialog::onPhotoReady(const QString &url)
{
    for (int i = 0; i < m_candidates->count() && i < m_observations.size(); ++i) {
        if (previewUrl(m_observations.at(i)) != url)
            continue;
        const QPixmap pm = m_photos.photo(url);
        if (!pm.isNull())
            m_candidates->item(i)->setIcon(QIcon(pm));
    }
    if (const auto chosen = chosenObservation();
        chosen && (url == largeUrl(*chosen) || url == previewUrl(*chosen)))
        showPhoto();
}

} // namespace pl
