#include "ui/IdentifyRunsDialog.h"

#include "app/Application.h"
#include "organise/OrganiseSession.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"
#include "ui/IdentifyPanel.h"
#include "ui/InatTokenGetter.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pl {
namespace {

enum Column { UseColumn, PhotoColumn, NameColumn, NoteColumn };
constexpr int kThumbPx = 120;

} // namespace

IdentifyRunsDialog::IdentifyRunsDialog(Application &app, const organise::OrganiseSession &session,
                                       QWidget *parent)
    : QDialog(parent), m_app(app), m_session(session)
{
    setWindowTitle(tr("Identify Runs with iNaturalist"));

    auto *intro = new QLabel(tr("A new run starts wherever more than"), this);
    m_gap = new QSpinBox(this);
    m_gap->setRange(2, 3600);
    m_gap->setValue(30);
    m_gap->setSuffix(tr(" seconds"));
    auto *intro2 = new QLabel(tr("pass between photos. Photos already named are left alone."), this);
    connect(m_gap, &QSpinBox::valueChanged, this, [this] {
        if (!m_running)
            propose();
    });
    auto *gapRow = new QHBoxLayout;
    gapRow->addWidget(intro);
    gapRow->addWidget(m_gap);
    gapRow->addWidget(intro2, 1);
    m_commonNames = new QCheckBox(tr("Name with common names"), this);
    m_commonNames->setToolTip(tr("Name the runs with the suggestions' common names (Flame Robin) rather "
                                 "than their scientific names (Petroica phoenicea). A taxon without a "
                                 "common name gets its scientific name either way."));
    m_commonNames->setChecked(m_app.settings().inatIdentifyCommonName());
    connect(m_commonNames, &QCheckBox::toggled, this, [this](bool on) {
        m_app.settings().setInatIdentifyCommonName(on);
        for (auto it = m_suggested.cbegin(); it != m_suggested.cend(); ++it)
            fillNames(it.key());
    });
    gapRow->addWidget(m_commonNames);

    m_summary = new QLabel(this);
    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({tr("Use"), tr("First photo"), tr("Name"), tr("iNaturalist")});
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(kThumbPx * 3 / 4 + 8);
    m_table->horizontalHeader()->setSectionResizeMode(UseColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(PhotoColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(NoteColumn, QHeaderView::ResizeToContents);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setIconSize(QSize(kThumbPx, kThumbPx * 3 / 4));
    connect(m_table, &QTableWidget::itemChanged, this, [this] { updateButtons(); });

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_identify = new QPushButton(tr("Identify"), this);
    m_identify->setToolTip(tr("Send each run's first photo to iNaturalist's computer vision, one at "
                              "a time. They're scored, not posted."));
    connect(m_identify, &QPushButton::clicked, this, [this] { m_running ? stop() : start(); });
    m_getToken = new QPushButton(tr("Get Token…"), this);
    m_getToken->hide();
    connect(m_getToken, &QPushButton::clicked, this, [this] { InatTokenGetter::start(m_app); });
    connect(&m_app, &Application::inatApiTokenChanged, this, [this] {
        if (m_getToken->isVisible()) {
            m_getToken->hide();
            start();
        }
    });
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_getToken);
    statusRow->addWidget(m_identify);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setDefault(true);
    m_identify->setAutoDefault(false);
    m_getToken->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(gapRow);
    layout->addWidget(m_summary);
    layout->addWidget(m_table, 1);
    layout->addLayout(statusRow);
    layout->addWidget(buttons);
    resize(820, 620);

    connect(&m_app.photoIdentifier(), &inat::PhotoIdentifier::finished, this, &IdentifyRunsDialog::finished);
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, &IdentifyRunsDialog::thumbnailReady);
    propose();
}

void IdentifyRunsDialog::propose()
{
    m_starts = m_session.proposedRunStarts(m_gap->value());
    m_suggested.clear();
    const QSignalBlocker quiet(m_table);   // no itemChanged() for half-made rows
    m_table->setRowCount(0);
    m_table->setRowCount(int(m_starts.size()));
    for (int row = 0; row < m_starts.size(); ++row) {
        const organise::Item &item = m_session.items().at(m_starts.at(row));
        auto *use = new QTableWidgetItem;
        use->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        use->setCheckState(Qt::Unchecked);
        m_table->setItem(row, UseColumn, use);
        auto *photo = new QTableWidgetItem(QStringLiteral("%1\n%2").arg(
            item.baseName, QLocale().toString(item.taken, QLocale::ShortFormat)));
        photo->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(row, PhotoColumn, photo);
        auto *name = new QComboBox(m_table);
        name->setEditable(true);
        name->lineEdit()->setPlaceholderText(tr("(not identified yet)"));
        connect(name, &QComboBox::currentTextChanged, this, [this, row](const QString &text) {
            if (QTableWidgetItem *use = m_table->item(row, UseColumn))
                use->setCheckState(text.trimmed().isEmpty() ? Qt::Unchecked : Qt::Checked);
        });
        // Its own height, in the middle of the (thumbnail-high) row.
        auto *cell = new QWidget(m_table);
        auto *centred = new QVBoxLayout(cell);
        centred->setContentsMargins(6, 0, 6, 0);
        centred->addWidget(name, 0, Qt::AlignVCenter);
        m_table->setCellWidget(row, NameColumn, cell);
        auto *note = new QTableWidgetItem;
        note->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(row, NoteColumn, note);
    }
    thumbnailReady();
    m_summary->setText(m_starts.isEmpty()
                           ? tr("Every photo is already in a named run.")
                           : m_starts.size() == 1 ? tr("1 run to identify.")
                                                  : tr("%1 runs to identify.").arg(m_starts.size()));
    m_status->clear();
    updateButtons();
}

void IdentifyRunsDialog::thumbnailReady()
{
    for (int row = 0; row < m_starts.size(); ++row) {
        QTableWidgetItem *photo = m_table->item(row, PhotoColumn);
        if (!photo || !photo->icon().isNull())
            continue;
        const QPixmap pm = m_app.thumbnails().thumbnail({}, m_session.items().at(m_starts.at(row)).previewPath,
                                                        kThumbPx);
        if (!pm.isNull())
            photo->setIcon(QIcon(pm));
    }
}

void IdentifyRunsDialog::start()
{
    const QString token = m_app.settings().inatApiToken();
    if (!InatTokenGetter::isUsable(token)) {
        m_status->setText(token.isEmpty() ? tr("Identifying needs your iNaturalist token. Get one — it "
                                               "lasts a day.")
                                          : tr("Your iNaturalist token has expired. Get a fresh one."));
        m_getToken->show();
        return;
    }
    m_app.inat().setAccessToken(token);
    m_running = true;
    m_asking = -1;
    next();
    updateButtons();
}

void IdentifyRunsDialog::stop()
{
    m_running = false;
    m_asking = -1;
    m_status->setText(tr("Stopped."));
    updateButtons();
}

void IdentifyRunsDialog::next()
{
    // The next row without suggestions yet.
    int row = m_asking + 1;
    while (row < m_starts.size()) {
        auto *name = m_table->cellWidget(row, NameColumn)->findChild<QComboBox *>();
        if (name->count() == 0 && m_table->item(row, NoteColumn)->text().isEmpty())
            break;
        ++row;
    }
    if (row >= m_starts.size()) {
        m_running = false;
        m_asking = -1;
        m_status->setText(tr("Done. Check the names, change any that are wrong, then OK."));
        updateButtons();
        return;
    }
    m_asking = row;
    const organise::Item &item = m_session.items().at(m_starts.at(row));
    m_status->setText(tr("Asking about %1 (%2 of %3)…").arg(item.baseName).arg(row + 1).arg(m_starts.size()));
    m_table->scrollToItem(m_table->item(row, PhotoColumn));
    inat::IdentifyRequest request;
    request.key = item.previewPath;
    request.path = item.previewPath;
    if (item.hasGps())
        request.lonLat = QPointF(*item.longitude, *item.latitude);
    if (item.takenFromExif && item.taken.isValid())
        request.date = item.taken.date();
    m_app.photoIdentifier().identify(request);
}

void IdentifyRunsDialog::fillNames(int row)
{
    auto *name = m_table->cellWidget(row, NameColumn)->findChild<QComboBox *>();
    const QList<taxonomy::Taxon> &taxa = m_suggested.value(row);
    // The same suggestion stays chosen; a name typed in stays as it is.
    int chosen = -1;
    for (int i = 0; i < taxa.size(); ++i) {
        if (name->currentText() == taxa.at(i).commonName || name->currentText() == taxa.at(i).name)
            chosen = i;
    }
    const QString typed = name->currentText();
    const QSignalBlocker block(name);
    name->clear();
    for (const taxonomy::Taxon &t : taxa)
        name->addItem(IdentifyPanel::photoName(m_app, t));
    if (chosen >= 0)
        name->setCurrentIndex(chosen);
    else
        name->setEditText(typed);
}

void IdentifyRunsDialog::finished(const QString &key, const std::optional<QRectF> &crop,
                                  const inat::Identification &result)
{
    if (!m_running || m_asking < 0 || crop || key != m_session.items().at(m_starts.at(m_asking)).previewPath)
        return;
    const int row = m_asking;
    QTableWidgetItem *note = m_table->item(row, NoteColumn);
    if (!result.ok()) {
        if (result.status == 401) {
            m_running = false;
            m_status->setText(tr("iNaturalist didn't accept the token — it has probably expired. Get "
                                 "a fresh one to carry on."));
            m_getToken->show();
            updateButtons();
            return;
        }
        if (result.status == 403 || result.status == 429) {
            m_running = false;
            m_status->setText(tr("iNaturalist refused or is busy (it may be limiting requests). Try again "
                                 "later — what's done so far is kept."));
            updateButtons();
            return;
        }
        note->setText(tr("Couldn't identify: %1").arg(result.error));
        next();
        return;
    }
    auto *name = m_table->cellWidget(row, NameColumn)->findChild<QComboBox *>();
    QList<taxonomy::Taxon> taxa;
    for (const net::VisionSuggestion &s : result.result.suggestions) {
        if (taxa.size() < 8)
            taxa << s.taxon;
    }
    if (result.result.commonAncestor)
        taxa << result.result.commonAncestor->taxon;
    m_suggested.insert(row, taxa);
    {
        const QSignalBlocker block(name);
        name->setEditText(QString());
    }
    fillNames(row);
    if (!result.result.suggestions.isEmpty()) {
        const net::VisionSuggestion &top = result.result.suggestions.first();
        QStringList notes{QStringLiteral("%1%").arg(qRound(top.score))};
        if (top.seenNearby)
            notes << tr("seen nearby");
        notes << (result.inTrees.contains(top.taxon.inatId) ? tr("in your trees") : tr("not in your trees"));
        note->setText(notes.join(QStringLiteral(" · ")));
        name->setCurrentIndex(0);
        m_table->item(row, UseColumn)->setCheckState(Qt::Checked);
    } else {
        note->setText(tr("No suggestions"));
    }
    next();
}

void IdentifyRunsDialog::updateButtons()
{
    m_identify->setText(m_running ? tr("Stop") : tr("Identify"));
    m_identify->setEnabled(!m_starts.isEmpty());
    m_gap->setEnabled(!m_running);
    int ticked = 0;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *use = m_table->item(row, UseColumn);
        ticked += use && use->checkState() == Qt::Checked ? 1 : 0;
    }
    m_ok->setText(ticked == 1 ? tr("Name 1 Run") : tr("Name %1 Runs").arg(ticked));
    m_ok->setMinimumWidth(m_ok->fontMetrics().horizontalAdvance(m_ok->text()) + 40);
    m_ok->setEnabled(ticked > 0 && !m_running);
}

QList<QPair<int, QString>> IdentifyRunsDialog::names() const
{
    QList<QPair<int, QString>> out;
    for (int row = 0; row < m_starts.size(); ++row) {
        if (m_table->item(row, UseColumn)->checkState() != Qt::Checked)
            continue;
        const QString name =
            m_table->cellWidget(row, NameColumn)->findChild<QComboBox *>()->currentText().simplified();
        if (!name.isEmpty())
            out.append({m_starts.at(row), name});
    }
    return out;
}

} // namespace pl
