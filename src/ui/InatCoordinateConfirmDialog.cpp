#include "ui/InatCoordinateConfirmDialog.h"

#include "net/INatClient.h"
#include "net/PhotoCache.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

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

} // namespace

InatCoordinateConfirmDialog::InatCoordinateConfirmDialog(net::INatClient &inat, net::PhotoCache &photos,
                                                          qint64 taxonInatId, const QString &userLogin,
                                                          const QDate &capturedOn, QWidget *parent)
    : QDialog(parent), m_inat(inat), m_photos(photos), m_taxonInatId(taxonInatId),
      m_userLogin(userLogin), m_capturedOn(capturedOn)
{
    setWindowTitle(tr("Confirm iNaturalist Observation"));
    setModal(true);
    resize(480, 440);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_candidates = new QListWidget(this);
    m_candidates->setViewMode(QListView::IconMode);
    m_candidates->setIconSize(QSize(128, 128));
    m_candidates->setResizeMode(QListView::Adjust);
    m_candidates->setMovement(QListView::Static);
    m_candidates->setSpacing(6);
    connect(m_candidates, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(m_candidates, &QListWidget::currentRowChanged, this, [this](int row) {
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(row >= 0);
    });

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_candidates, 1);
    layout->addWidget(m_buttons);

    connect(&m_photos, &net::PhotoCache::ready, this,
            [this](const QString &url) { onPhotoReady(url); });

    runSearch();
}

std::optional<taxonomy::Observation> InatCoordinateConfirmDialog::chosenObservation() const
{
    const QListWidgetItem *item = m_candidates->currentItem();
    if (!item)
        return std::nullopt;
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_observations.size())
        return std::nullopt;
    return m_observations.at(index);
}

void InatCoordinateConfirmDialog::setBusy(bool busy)
{
    m_candidates->setEnabled(!busy);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!busy && m_candidates->currentRow() >= 0);
}

void InatCoordinateConfirmDialog::runSearch()
{
    setBusy(true);
    m_status->setText(tr("Searching iNaturalist…"));

    // A day either side of the local capture's date, the same window
    // InatObservationFetcher::looksLikeDuplicate() uses to decide two records
    // plausibly describe the same sighting.
    const QDate from = m_capturedOn.addDays(-1);
    const QDate to = m_capturedOn.addDays(1);

    m_inat.fetchObservations(
        m_userLogin, {m_taxonInatId}, 0, from, to, 1,
        [this](net::Outcome<net::ObservationPage> out) {
            setBusy(false);
            if (!out.ok()) {
                m_status->setText(tr("Search failed: %1").arg(out.error));
                populateCandidates({});
                return;
            }

            // Only observations with actual coordinates are useful here.
            QList<taxonomy::Observation> withCoords;
            for (const taxonomy::Observation &obs : out.value.results) {
                if (obs.latitude && obs.longitude)
                    withCoords << obs;
            }

            if (withCoords.isEmpty())
                m_status->setText(
                    tr("No matching iNaturalist observations with location data were found."));
            else
                m_status->setText(tr("%n candidate observation(s) found — pick the one that "
                                     "matches, if any, or Cancel.",
                                     nullptr, withCoords.size()));
            populateCandidates(withCoords);
        });
}

void InatCoordinateConfirmDialog::populateCandidates(const QList<taxonomy::Observation> &candidates)
{
    m_observations = candidates;
    m_candidates->clear();

    for (int i = 0; i < m_observations.size(); ++i) {
        const taxonomy::Observation &obs = m_observations.at(i);
        const QString label = obs.observedOn.isEmpty() ? tr("(no date)") : obs.observedOn;
        auto *item = new QListWidgetItem(makePlaceholder(), label, m_candidates);
        item->setData(Qt::UserRole, i);
        if (!obs.photos.isEmpty()) {
            const QPixmap pm = m_photos.photo(obs.photos.first().previewUrl);
            if (!pm.isNull())
                item->setIcon(QIcon(pm));
        }
    }
    if (!m_observations.isEmpty())
        m_candidates->setCurrentRow(0);
}

void InatCoordinateConfirmDialog::onPhotoReady(const QString &url)
{
    for (int i = 0; i < m_candidates->count(); ++i) {
        QListWidgetItem *item = m_candidates->item(i);
        const int idx = item->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_observations.size())
            continue;
        const taxonomy::Observation &obs = m_observations.at(idx);
        if (obs.photos.isEmpty() || obs.photos.first().previewUrl != url)
            continue;
        const QPixmap pm = m_photos.photo(url);
        if (!pm.isNull())
            item->setIcon(QIcon(pm));
    }
}

} // namespace pl
