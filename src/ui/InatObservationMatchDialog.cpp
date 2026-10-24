#include "ui/InatObservationMatchDialog.h"

#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "scan/CaptureRenamer.h"
#include "thumb/ThumbnailCache.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTimeZone>
#include <QToolButton>
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

// Seconds between two clock times held as UTC.
qint64 gap(const QDateTime &a, const QDateTime &b)
{
    return std::abs(a.secsTo(b));
}

QString whenText(const taxonomy::Observation &obs)
{
    if (obs.timeObserved.isValid())
        return QLocale().toString(obs.timeObserved.date(), QLocale::ShortFormat) + QLatin1Char(' ')
               + obs.timeObserved.time().toString(QStringLiteral("HH:mm"));
    const QDate d = QDate::fromString(obs.observedOn, Qt::ISODate);
    return d.isValid() ? QLocale().toString(d, QLocale::ShortFormat) : QObject::tr("(no date)");
}

} // namespace

InatObservationMatchDialog::InatObservationMatchDialog(net::INatClient &inat, net::PhotoCache &photos,
                                                       const QString &userLogin,
                                                       const Selection &selection, QWidget *parent,
                                                       thumb::ThumbnailCache *thumbs)
    : QDialog(parent), m_inat(inat), m_photos(photos), m_userLogin(userLogin), m_selection(selection),
      m_thumbs(thumbs)
{
    setWindowTitle(tr("Match with an iNaturalist Observation"));
    setModal(true);
    const bool showOwn = m_thumbs && !m_selection.shown.isEmpty();
    resize(showOwn ? 1240 : 860, 600);

    // The photo being matched, to compare with the observations -- each in
    // turn when there are several.
    QWidget *own = nullptr;
    if (showOwn) {
        own = new QWidget(this);
        auto *title = new QLabel(m_selection.shown.size() > 1 ? tr("<b>Your photos</b>") : tr("<b>Your photo</b>"), own);
        m_ownPhoto = new QLabel(own);
        m_ownPhoto->setAlignment(Qt::AlignCenter);
        m_ownPhoto->setMinimumSize(340, 340);
        m_ownPhoto->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_ownPhoto->setFrameShape(QFrame::StyledPanel);
        m_ownCaption = new QLabel(own);
        m_ownCaption->setWordWrap(true);
        m_ownCaption->setAlignment(Qt::AlignHCenter);
        m_ownPrevious = new QToolButton(own);
        m_ownPrevious->setArrowType(Qt::LeftArrow);
        m_ownPrevious->setToolTip(tr("Previous photo"));
        m_ownNext = new QToolButton(own);
        m_ownNext->setArrowType(Qt::RightArrow);
        m_ownNext->setToolTip(tr("Next photo"));
        connect(m_ownPrevious, &QToolButton::clicked, this, [this] {
            m_ownIndex = (m_ownIndex + int(m_selection.shown.size()) - 1) % int(m_selection.shown.size());
            showOwnPhoto();
        });
        connect(m_ownNext, &QToolButton::clicked, this, [this] {
            m_ownIndex = (m_ownIndex + 1) % int(m_selection.shown.size());
            showOwnPhoto();
        });
        const bool several = m_selection.shown.size() > 1;
        m_ownPrevious->setVisible(several);
        m_ownNext->setVisible(several);
        auto *nav = new QHBoxLayout;
        nav->addWidget(m_ownPrevious);
        nav->addWidget(m_ownCaption, 1);
        nav->addWidget(m_ownNext);
        auto *ownLayout = new QVBoxLayout(own);
        ownLayout->setContentsMargins(0, 0, 0, 0);
        ownLayout->addWidget(title);
        ownLayout->addWidget(m_ownPhoto, 1);
        ownLayout->addLayout(nav);
        own->setMinimumWidth(340);
        connect(m_thumbs, &thumb::ThumbnailCache::ready, this, [this](const QString &key, int) {
            const Selection::Photo &p = m_selection.shown.at(m_ownIndex);
            if (key == thumb::ThumbnailCache::keyFor(p.contentHash, p.path))
                showOwnPhoto();
        });
    }

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    // Neither times nor dates given per photo: the range's days.
    if (m_selection.times.isEmpty() && m_selection.untimedDates.isEmpty())
        for (QDate d = m_selection.from; d.isValid() && d <= m_selection.to; d = d.addDays(1))
            m_selection.untimedDates << d;

    // How near in time: narrow first; the counts say what widening would add.
    m_window = Window(std::clamp(m_selection.window, 0, 2));
    if (m_selection.times.isEmpty())
        m_window = Days;
    auto *windowRow = new QHBoxLayout;
    windowRow->addWidget(new QLabel(tr("<b>Taken within:</b>"), this));
    auto *windowGroup = new QButtonGroup(this);   // apart from the name choice's buttons
    for (int w = Minutes; w <= Days; ++w) {
        auto *b = new QRadioButton(this);
        windowGroup->addButton(b, w);
        m_windowButtons[w] = b;
        b->setChecked(w == m_window);
        if (w != Days && m_selection.times.isEmpty()) {
            b->setEnabled(false);
            b->setToolTip(tr("The photos' times aren't known (no EXIF date and time), so only "
                             "their dates can be matched."));
        }
        connect(b, &QRadioButton::toggled, this, [this, w](bool on) {
            if (!on)
                return;
            m_window = Window(w);
            populateCandidates();
        });
        windowRow->addWidget(b);
    }
    windowRow->addStretch(1);

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
    connect(m_applySpecies, &QCheckBox::toggled, this, [this] {
        updateRename();
        updateApplyButton();
    });

    // Renaming the files with the species' name: common or scientific.
    m_rename = new QCheckBox(tr("Also rename the photo files, with its"), this);
    m_rename->setToolTip(tr("Puts the name in place of the name part of each file name, keeping "
                            "the rest (place, date, number) and the RAW, JPEG and sidecar files "
                            "together. Nothing is overwritten."));
    m_renameCommon = new QRadioButton(tr("Common name"), this);
    m_renameScientific = new QRadioButton(tr("Scientific name"), this);
    auto *nameGroup = new QButtonGroup(this);
    nameGroup->addButton(m_renameCommon);
    nameGroup->addButton(m_renameScientific);
    (m_selection.preferCommonName ? m_renameCommon : m_renameScientific)->setChecked(true);
    m_renameExample = new QLabel(this);
    m_renameExample->setWordWrap(true);
    m_renameExample->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette muted = m_renameExample->palette();
    muted.setColor(QPalette::WindowText, muted.color(QPalette::PlaceholderText));
    m_renameExample->setPalette(muted);
    connect(m_rename, &QCheckBox::toggled, this, [this] { updateRename(); });
    connect(m_renameCommon, &QRadioButton::toggled, this, [this] { updateRename(); });

    auto *details = new QVBoxLayout;
    details->addWidget(m_photo, 0, Qt::AlignHCenter);
    details->addWidget(m_species);
    details->addWidget(m_coordinates);
    details->addWidget(m_link);
    details->addSpacing(8);
    details->addWidget(m_applyHeading);
    details->addWidget(m_applySpecies);
    auto *renameBox = new QVBoxLayout;
    renameBox->setContentsMargins(20, 0, 0, 0);
    renameBox->addWidget(m_rename);
    auto *names = new QVBoxLayout;
    names->setContentsMargins(20, 0, 0, 0);
    names->addWidget(m_renameCommon);
    names->addWidget(m_renameScientific);
    renameBox->addLayout(names);
    renameBox->addWidget(m_renameExample);
    details->addLayout(renameBox);
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
    if (own)
        middle->addWidget(own, 1);
    middle->addWidget(m_candidates, 1);
    middle->addWidget(detailsWidget);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(windowRow);
    layout->addWidget(m_status);
    layout->addLayout(middle, 1);
    layout->addWidget(m_buttons);

    connect(&m_photos, &net::PhotoCache::ready, this,
            [this](const QString &url) { onPhotoReady(url); });

    showDetails();
    if (showOwn)
        showOwnPhoto();
    m_status->setText(tr("Searching your iNaturalist observations…"));
    populateCandidates();
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

bool InatObservationMatchDialog::renameFiles() const
{
    return applySpecies() && m_rename->isEnabled() && m_rename->isChecked() && !renameName().isEmpty();
}

bool InatObservationMatchDialog::renameWithCommonName() const
{
    return m_renameCommon->isChecked();
}

QString InatObservationMatchDialog::renameName() const
{
    const std::optional<taxonomy::Observation> chosen = chosenObservation();
    if (!chosen)
        return {};
    return m_renameCommon->isChecked() && !chosen->taxonCommonName.isEmpty() ? chosen->taxonCommonName
                                                                             : chosen->taxonName;
}

void InatObservationMatchDialog::updateRename()
{
    const std::optional<taxonomy::Observation> chosen = chosenObservation();
    const bool can = chosen && applySpecies() && !chosen->taxonName.isEmpty();
    const bool hasCommon = chosen && !chosen->taxonCommonName.isEmpty();
    m_rename->setEnabled(can);
    m_renameCommon->setEnabled(can && m_rename->isChecked() && hasCommon);
    m_renameScientific->setEnabled(can && m_rename->isChecked());
    m_renameCommon->setText(hasCommon ? tr("Common name (%1)").arg(chosen->taxonCommonName) : tr("Common name"));
    m_renameScientific->setText(chosen && !chosen->taxonName.isEmpty()
                                    ? tr("Scientific name (%1)").arg(chosen->taxonName)
                                    : tr("Scientific name"));
    if (!can || !m_rename->isChecked() || m_selection.sampleBaseName.isEmpty()) {
        m_renameExample->clear();
        return;
    }
    const QString stem = scan::CaptureRenamer::renamedStem(m_selection.sampleBaseName,
                                                           m_selection.sampleNameText, renameName());
    const QString ext = m_selection.sampleExtension.isEmpty() ? QString()
                                                              : QLatin1Char('.') + m_selection.sampleExtension;
    m_renameExample->setText(m_selection.photos > 1
                                 ? tr("e.g. %1%2 → %3%2").arg(m_selection.sampleBaseName, ext, stem)
                                 : tr("%1%2 → %3%2").arg(m_selection.sampleBaseName, ext, stem));
}

QList<taxonomy::Observation> InatObservationMatchDialog::rankCandidates(QList<taxonomy::Observation> candidates,
                                                                       qint64 preferredTaxon,
                                                                       const QDate &centre,
                                                                       const QList<QDateTime> &times)
{
    // In seconds: to the nearest photo's time when both are known; else by
    // date, after the timed ones of the same day.
    const auto distance = [&centre, &times](const taxonomy::Observation &o) -> qint64 {
        if (o.timeObserved.isValid() && !times.isEmpty()) {
            qint64 best = std::numeric_limits<qint64>::max();
            for (const QDateTime &t : times)
                best = std::min(best, gap(o.timeObserved, t));
            return best;
        }
        const QDate d = QDate::fromString(o.observedOn, Qt::ISODate);
        return d.isValid() && centre.isValid() ? std::abs(d.daysTo(centre)) * 86400 + 43200 : 1LL << 40;
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

QList<taxonomy::Observation> InatObservationMatchDialog::withinWindow(const QList<taxonomy::Observation> &all,
                                                                     const QList<QDateTime> &times,
                                                                     const QList<QDate> &untimedDates,
                                                                     Window window, int *noTime)
{
    QList<QDate> dates = untimedDates;
    for (const QDateTime &t : times)
        dates << t.date();
    const auto dayOf = [](const taxonomy::Observation &o) {
        return o.timeObserved.isValid() ? o.timeObserved.date() : QDate::fromString(o.observedOn, Qt::ISODate);
    };
    const auto nearDay = [&dates](const QDate &d, int days) {
        return d.isValid() && std::any_of(dates.cbegin(), dates.cend(), [&](const QDate &p) {
                   return std::abs(p.daysTo(d)) <= days;
               });
    };
    const qint64 limit = window == Minutes ? kMinutes * 60LL : kHours * 3600LL;
    QList<taxonomy::Observation> out;
    int untimed = 0;
    for (const taxonomy::Observation &o : all) {
        const QDate day = dayOf(o);
        if (window == Days) {
            if (nearDay(day, 1))
                out << o;
            continue;
        }
        // A photo whose time isn't known matches on its date.
        if (day.isValid() && untimedDates.contains(day)) {
            out << o;
            continue;
        }
        if (!o.timeObserved.isValid()) {
            if (nearDay(day, 0))
                ++untimed;
            continue;
        }
        if (std::any_of(times.cbegin(), times.cend(),
                        [&](const QDateTime &t) { return gap(o.timeObserved, t) <= limit; }))
            out << o;
    }
    if (noTime)
        *noTime = untimed;
    return out;
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
            self->m_searched = true;
            self->populateCandidates();
        });
}

void InatObservationMatchDialog::populateCandidates()
{
    // The counts for each window, on its button.
    const auto windowLabel = [](int w) {
        return w == Minutes ? tr("%1 minutes").arg(kMinutes)
               : w == Hours ? tr("%1 hours").arg(kHours)
                            : tr("A day either side");
    };
    int counts[3] = {};
    int untimed = 0;
    for (int w = Minutes; w <= Days; ++w) {
        int n = 0;
        counts[w] = int(withinWindow(m_found, m_selection.times, m_selection.untimedDates, Window(w), &n).size());
        if (w == m_window)
            untimed = n;
        QString label = windowLabel(w);
        if (m_searched)
            label += QStringLiteral(" (%1)").arg(counts[w]);
        m_windowButtons[w]->setText(label);
    }

    const QDate centre = m_selection.from.addDays(m_selection.from.daysTo(m_selection.to) / 2);
    m_observations = rankCandidates(withinWindow(m_found, m_selection.times, m_selection.untimedDates, m_window),
                                    m_selection.currentTaxonInatId, centre, m_selection.times);

    if (m_searched) {
        // The name it was filed under, to set against iNat's (which may have
        // changed since).
        QString text = m_selection.photoName.isEmpty()
                           ? QString()
                           : tr("This photo is filed as \u201c%1\u201d. ").arg(m_selection.photoName);
        const QString within = m_window == Minutes ? tr("within %1 minutes of these photos").arg(kMinutes)
                               : m_window == Hours ? tr("within %1 hours of these photos").arg(kHours)
                                                   : tr("from a day either side of these photos");
        if (m_observations.isEmpty()) {
            text += tr("None of your iNaturalist observations are %1.").arg(within);
            // Where widening would find some.
            for (int w = m_window + 1; w <= Days; ++w) {
                if (counts[w] > 0) {
                    text += QStringLiteral(" ") + tr("Choose \u201c%1\u201d to see %n.", nullptr, counts[w])
                                                      .arg(windowLabel(w));
                    break;
                }
            }
        } else {
            text += tr("%n observation(s) of yours %1 — pick the one these photos match, if any, "
                       "then tick what to take from it.",
                       nullptr, int(m_observations.size()))
                        .arg(within);
        }
        if (untimed > 0)
            text += QStringLiteral(" ")
                    + tr("%n more that day have no time on iNaturalist — choose \u201cA day either "
                         "side\u201d to see them.", nullptr, untimed);
        m_status->setText(text);
    }

    m_candidates->clear();
    for (int i = 0; i < m_observations.size(); ++i) {
        const taxonomy::Observation &obs = m_observations.at(i);
        const QString date = whenText(obs);
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
        updateRename();
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
    updateRename();
    updateApplyButton();
}

void InatObservationMatchDialog::showOwnPhoto()
{
    if (!m_ownPhoto || m_selection.shown.isEmpty())
        return;
    const Selection::Photo &p = m_selection.shown.at(m_ownIndex);
    QString caption = m_selection.shown.size() > 1
                          ? tr("%1 of %2: %3").arg(m_ownIndex + 1).arg(m_selection.shown.size()).arg(p.label)
                          : p.label;
    if (p.taken.isValid())
        caption += QStringLiteral("\n") + QLocale().toString(p.taken.date(), QLocale::ShortFormat)
                   + QLatin1Char(' ') + p.taken.time().toString(QStringLiteral("HH:mm:ss"));
    m_ownCaption->setText(caption);
    m_ownCaption->setToolTip(p.path);
    // The large one when it's made; the grid's meanwhile.
    QPixmap pm = m_thumbs->thumbnail(p.contentHash, p.path, thumb::ThumbnailCache::kPreviewPx);
    if (pm.isNull())
        pm = m_thumbs->thumbnail(p.contentHash, p.path, thumb::ThumbnailCache::kGridPx);
    if (pm.isNull()) {
        m_ownPhoto->setText(tr("loading…"));
        return;
    }
    const qreal dpr = devicePixelRatioF();
    QPixmap scaled = pm.scaled(m_ownPhoto->contentsRect().size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    m_ownPhoto->setPixmap(scaled);
}

void InatObservationMatchDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    showOwnPhoto();
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
