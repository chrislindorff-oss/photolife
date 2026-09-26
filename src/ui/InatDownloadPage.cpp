#include "ui/InatDownloadPage.h"

#include "app/Application.h"
#include "inat/InatDownloadModel.h"
#include "inat/InatImportService.h"
#include "inat/InatObservationFetcher.h"
#include "inat/LibraryPresence.h"
#include "db/Database.h"
#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"
#include "ui/InatToolsView.h"
#include "ui/InatScopeWidgets.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace pl {
namespace {

// Draws the same border/caption look as CaptureDelegate, for the iNaturalist
// download grid's InatDownloadModel rows. A separate class rather than reuse:
// CaptureDelegate reads model::CaptureListModel-specific roles (status,
// best-shot, badges) whose numeric values collide with unrelated
// InatDownloadModel roles (e.g. MatchStatusRole == HasGpsRole == UserRole+7),
// so calling it against this model would paint bogus status dots. The
// "faded" look for likely-duplicate candidates is already baked into the
// decoration pixmap itself (InatDownloadModel::dimmed()), so no extra
// painting is needed for that here.
class InatCandidateDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->text.clear();
        option->icon = QIcon();
    }

public:
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QString caption = index.data(Qt::DisplayRole).toString();
        const int lines = std::max(1, int(caption.count(QLatin1Char('\n'))) + 1);
        return QSize(option.decorationSize.width() + 20,
                    option.decorationSize.height() + 20 + lines * 16);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const pl::ThemeColors &theme = pl::themeColors(pl::currentThemeVariant());
        const bool selected = option.state & QStyle::State_Selected;

        painter->fillRect(option.rect, option.palette.color(QPalette::Base));
        if (selected) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.selectedRow);
            painter->drawRoundedRect(option.rect.adjusted(1, 1, -2, -2), theme.radius, theme.radius);
            painter->restore();
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(selected ? theme.primaryAccent : theme.border, selected ? 2 : 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(option.rect.adjusted(1, 1, -2, -2), theme.radius, theme.radius);
        painter->restore();

        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        const QRect decoBox(option.rect.left() + (option.rect.width() - option.decorationSize.width()) / 2,
                            option.rect.top(), option.decorationSize.width(),
                            option.decorationSize.height());
        QRect imageRect = decoBox;
        if (!icon.isNull()) {
            const QSize native = icon.availableSizes().value(0, option.decorationSize);
            const QSize fitted = native.scaled(option.decorationSize, Qt::KeepAspectRatio);
            imageRect = QRect(QPoint(0, 0), fitted);
            imageRect.moveCenter(decoBox.center());
            painter->drawPixmap(imageRect, icon.pixmap(native));
        }
        const int imageBottom = imageRect.bottom();

        // NEW (no photos of this taxon in the library) / NO TREE (taxon in no
        // reference tree): pills across the top-left of the thumbnail.
        if (index.data(inat::InatDownloadModel::PresenceCheckedRole).toBool()) {
            QList<std::pair<QString, QColor>> badges;
            if (!index.data(inat::InatDownloadModel::InLibraryRole).toBool())
                badges.append({QStringLiteral("NEW"), theme.primaryAccent});
            if (!index.data(inat::InatDownloadModel::InAnyTreeRole).toBool())
                badges.append({QStringLiteral("NO TREE"), theme.warning});
            if (!badges.isEmpty()) {
                QFont font = painter->font();
                font.setPointSize(7);
                font.setBold(true);
                const QFontMetrics fm(font);
                const int padH = 6, pillH = 16, gap = 4;
                int x = imageRect.left() + 4;
                const int y = imageRect.top() + 4;
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setFont(font);
                for (const auto &[text, color] : badges) {
                    const QRect pill(x, y, fm.horizontalAdvance(text) + padH * 2, pillH);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(color);
                    painter->drawRoundedRect(pill, pillH / 2.0, pillH / 2.0);
                    painter->setPen(Qt::white);
                    painter->drawText(pill, Qt::AlignCenter, text);
                    x += pill.width() + gap;
                }
                painter->restore();
            }
        }

        const QString caption = index.data(Qt::DisplayRole).toString();
        if (!caption.isEmpty()) {
            QFont font = painter->font();
            if (font.pointSize() > 0)
                font.setPointSize(qMax(1, font.pointSize() - 1));
            else
                font.setPixelSize(qMax(1, font.pixelSize() - 1));
            QFontMetrics fm(font);
            const int lineH = 16;
            const int top = imageBottom + 4;
            const QRect textArea(option.rect.left() + 4, top, option.rect.width() - 8,
                                 option.rect.bottom() - top);

            painter->save();
            painter->setFont(font);
            painter->setPen(option.palette.color(QPalette::Text));
            int y = textArea.top();
            for (const QString &line : caption.split(QLatin1Char('\n'))) {
                const QString elided = fm.elidedText(line, Qt::ElideRight, textArea.width());
                painter->drawText(QRect(textArea.left(), y, textArea.width(), lineH),
                                  Qt::AlignHCenter | Qt::AlignVCenter, elided);
                y += lineH;
            }
            painter->restore();
        }
    }
};

} // namespace

// A QSortFilterProxyModel that additionally hides InatDownloadModel rows
// flagged LikelyDuplicateRole when hideFaded is on, layered on top of the
// base class's own name-substring filtering.
class InatFilterProxyModel : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setHideFaded(bool hide)
    {
        if (m_hideFaded == hide)
            return;
        m_hideFaded = hide;
        invalidateFilter();
    }

    // Rows of the page's Show combo.
    enum Presence { AllPresence, NewToLibrary, NotInAnyTree, NewOrNotInTree };
    void setPresenceFilter(int presence)
    {
        if (m_presence == presence)
            return;
        m_presence = presence;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        if (!QSortFilterProxyModel::filterAcceptsRow(row, parent))
            return false;
        const QModelIndex idx = sourceModel()->index(row, 0, parent);
        if (m_hideFaded && idx.data(inat::InatDownloadModel::LikelyDuplicateRole).toBool())
            return false;
        if (m_presence != AllPresence
            && idx.data(inat::InatDownloadModel::PresenceCheckedRole).toBool()) {
            const bool isNew = !idx.data(inat::InatDownloadModel::InLibraryRole).toBool();
            const bool noTree = !idx.data(inat::InatDownloadModel::InAnyTreeRole).toBool();
            if ((m_presence == NewToLibrary && !isNew) || (m_presence == NotInAnyTree && !noTree)
                || (m_presence == NewOrNotInTree && !isNew && !noTree))
                return false;
        }
        return true;
    }

private:
    bool m_hideFaded = false;
    int m_presence = AllPresence;
};

InatDownloadPage::InatDownloadPage(Application &app, InatAccountBar &account, QWidget *parent)
    : QWidget(parent), m_app(app), m_account(account)
{
    m_fetcher = new inat::InatObservationFetcher(m_app.inat(),
                                                 m_app.database().connectionName(), this);
    m_model = new inat::InatDownloadModel(m_app.photoCache(), m_app.taxonomyStore(), this);
    m_proxy = new InatFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterRole(inat::InatDownloadModel::NameRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setDynamicSortFilter(true);
    m_proxy->setSortRole(inat::InatDownloadModel::NameRole);

    m_treeCombo = new ReferenceTreeCombo(QString(), this);
    m_treeCombo->setToolTip(
        tr("The reference tree whose species and locality the search can be limited to. "
           "Follows the tree selected in the Reference Trees panel until you pick one here."));
    connect(m_treeCombo, &QComboBox::currentIndexChanged, this,
            [this] { updateLocalityEnabled(); });

    m_scopeCombo = new QComboBox(this);
    m_scopeCombo->addItem(tr("Species in this tree"));
    m_scopeCombo->addItem(tr("All my observations"));
    m_scopeCombo->setToolTip(
        tr("\"All my observations\" isn't limited to the tree's species — use it with Show: "
           "\"New to library\" / \"Not in any tree\" to find taxa you don't have yet. "
           "\"Restrict to this tree's locality\" still applies to both."));

    m_restrictToLocality = new QCheckBox(tr("Restrict to this tree's locality"), this);
    m_restrictToLocality->setChecked(true);
    m_restrictToLocality->setToolTip(
        tr("When checked, only observations within the chosen reference tree's own locality "
           "count — a species that also occurs elsewhere won't pull in observations from "
           "outside the tree's region. Trees with no locality set search worldwide either way."));

    m_searchButton = new QPushButton(tr("Search iNaturalist"), this);
    connect(m_searchButton, &QPushButton::clicked, this, &InatDownloadPage::search);

    m_cancelSearchButton = new QPushButton(tr("Stop"), this);
    m_cancelSearchButton->setEnabled(false);
    m_cancelSearchButton->setToolTip(
        tr("A search covers every species in the tree one at a time; a prolific "
           "observer or a broad tree can mean a lot of iNaturalist pages to page "
           "through. Stop if it's taking too long."));
    connect(m_cancelSearchButton, &QPushButton::clicked, this, [this] { m_fetcher->cancel(); });

    auto *header = new QHBoxLayout;
    header->addWidget(new QLabel(tr("Tree:"), this));
    header->addWidget(m_treeCombo, 1);
    header->addWidget(new QLabel(tr("Search:"), this));
    header->addWidget(m_scopeCombo);
    header->addWidget(m_restrictToLocality);
    header->addWidget(m_searchButton);
    header->addWidget(m_cancelSearchButton);

    m_taxonLimit = new TaxonLimitRow(m_app.inat(), this);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(tr("Filter by species name…"));
    m_filterEdit->setClearButtonEnabled(true);
    connect(m_filterEdit, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_proxy->setFilterFixedString(text); });

    m_sortCombo = new QComboBox(this);
    m_sortCombo->addItem(tr("Name (A → Z)"), int(Qt::AscendingOrder));
    m_sortCombo->addItem(tr("Name (Z → A)"), int(Qt::DescendingOrder));
    connect(m_sortCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_proxy->sort(0, Qt::SortOrder(m_sortCombo->itemData(i).toInt()));
    });

    m_hideFaded = new QCheckBox(tr("Hide faded photos"), this);
    m_hideFaded->setToolTip(
        tr("Hides candidates that look like something already in your library."));
    connect(m_hideFaded, &QCheckBox::toggled, this,
            [this](bool on) { m_proxy->setHideFaded(on); });

    m_presenceCombo = new QComboBox(this);
    m_presenceCombo->addItem(tr("Show: all"), int(InatFilterProxyModel::AllPresence));
    m_presenceCombo->addItem(tr("Show: new to library"), int(InatFilterProxyModel::NewToLibrary));
    m_presenceCombo->addItem(tr("Show: not in any tree"), int(InatFilterProxyModel::NotInAnyTree));
    m_presenceCombo->addItem(tr("Show: new or not in any tree"),
                             int(InatFilterProxyModel::NewOrNotInTree));
    connect(m_presenceCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_proxy->setPresenceFilter(m_presenceCombo->itemData(i).toInt());
    });

    m_checkButton = new QPushButton(tr("Check Against Library"), this);
    m_checkButton->setToolTip(
        tr("Re-checks these results against your reference trees and library (e.g. after "
           "adding a taxon to a tree or running Match Library). NEW = no photos of that "
           "taxon in your library, matched or named in a filename; NO TREE = the taxon "
           "isn't in any reference tree. Results are checked automatically after a search."));
    m_checkButton->setEnabled(false);
    connect(m_checkButton, &QPushButton::clicked, this, &InatDownloadPage::recheckAgainstLibrary);

    auto *browseRow = new QHBoxLayout;
    browseRow->addWidget(m_filterEdit, 1);
    browseRow->addWidget(m_sortCombo);
    browseRow->addWidget(m_presenceCombo);
    browseRow->addWidget(m_hideFaded);
    browseRow->addWidget(m_checkButton);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setText(
        tr("Enter your iNaturalist username above and search to find your own observations "
           "that aren't in your library yet — of one reference tree's species, or all of them."));

    m_grid = new QListView(this);
    m_grid->setModel(m_proxy);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setIconSize(QSize(192, 192));
    m_grid->setGridSize(QSize(212, 232));
    m_grid->setWordWrap(true);
    m_grid->setSpacing(6);
    m_grid->setItemDelegate(new InatCandidateDelegate(m_grid));

    auto *selectAllButton = new QPushButton(tr("Select All"), this);
    selectAllButton->setToolTip(
        tr("Selects every photo currently shown except the faded ones — those look like "
           "something already in your library. Click a faded photo directly if you want it "
           "included anyway."));
    connect(selectAllButton, &QPushButton::clicked, this, [this] {
        QItemSelection selection;
        for (int r = 0; r < m_proxy->rowCount(); ++r) {
            const QModelIndex idx = m_proxy->index(r, 0);
            if (!idx.data(inat::InatDownloadModel::LikelyDuplicateRole).toBool())
                selection.select(idx, idx);
        }
        m_grid->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    });

    m_downloadButton = new QPushButton(tr("Download Selected…"), this);
    connect(m_downloadButton, &QPushButton::clicked, this, &InatDownloadPage::requestDownload);

    auto *footer = new QHBoxLayout;
    footer->addWidget(m_status, 1);
    footer->addWidget(selectAllButton);
    footer->addWidget(m_downloadButton);

    connect(m_fetcher, &inat::InatObservationFetcher::progress, this,
            [this](int done, int total, int found) {
                if (m_scopeCombo->currentIndex() == 1)
                    emit statusMessage(
                        tr("Searching all your iNaturalist observations — %1 of %2 fetched")
                            .arg(found)
                            .arg(total),
                        0);
                else
                    emit statusMessage(
                        tr("Searching iNaturalist — %1 / %2 taxa checked, %3 observation(s) found")
                            .arg(done)
                            .arg(total)
                            .arg(found),
                        0);
            });
    connect(m_fetcher, &inat::InatObservationFetcher::finished, this,
            &InatDownloadPage::onSearchFinished);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(header);
    layout->addWidget(m_taxonLimit);
    layout->addLayout(browseRow);
    layout->addWidget(m_grid, 1);
    layout->addLayout(footer);

    updateLocalityEnabled();
}

void InatDownloadPage::setTrees(const QList<std::pair<int, QString>> &trees, int activeProjectId)
{
    m_treeCombo->setTrees(trees, activeProjectId);
}

void InatDownloadPage::followActiveTree(int projectId)
{
    m_treeCombo->followActiveTree(projectId);
}

void InatDownloadPage::setTreeSelection(qint64 inatId, const QString &name)
{
    m_taxonLimit->setTreeSelection(inatId, name);
}

void InatDownloadPage::limitToTaxon(qint64 inatId, const QString &label)
{
    m_taxonLimit->setTaxon(inatId, label);
    m_scopeCombo->setCurrentIndex(1);   // All my observations
}

int InatDownloadPage::selectedProjectId() const
{
    return m_treeCombo->projectId();
}

void InatDownloadPage::updateLocalityEnabled()
{
    m_restrictToLocality->setEnabled(selectedProjectId() > 0);
}

void InatDownloadPage::setDownloadEnabled(bool enabled)
{
    m_downloadButton->setEnabled(enabled);
}

void InatDownloadPage::selectPhotos(const QSet<qint64> &photoIds)
{
    QItemSelection selection;
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        const QModelIndex idx = m_proxy->index(r, 0);
        if (photoIds.contains(idx.data(inat::InatDownloadModel::PhotoIdRole).toLongLong()))
            selection.select(idx, idx);
    }
    m_grid->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
}

void InatDownloadPage::search()
{
    if (m_fetcher->isRunning())
        return;

    const bool allObservations = m_scopeCombo->currentIndex() == 1;
    const int pid = selectedProjectId();
    if (pid <= 0 && !allObservations) {
        QMessageBox::information(this, tr("Search iNaturalist"),
                                 tr("Choose a reference tree first, or search \"All my "
                                    "observations\"."));
        return;
    }
    const QString username = m_account.username();
    if (username.isEmpty()) {
        QMessageBox::information(this, tr("Search iNaturalist"),
                                 tr("Enter an iNaturalist username first."));
        m_account.focusUsername();
        return;
    }
    m_account.commit();

    // Leaf-rank taxa only (species and below) -- projectTaxonInatIds() returns
    // every taxon in the tree, including higher-rank nodes and the synthetic
    // "Life" root every tree carries for structure. iNat's taxon_id filter
    // matches a taxon's descendants too, so including a broad ancestor like
    // that would turn one "batch" into a search over the user's entire
    // observation history -- exactly the kind of single search that can take
    // a very long time while the batch counter sits still.
    // A taxon limit narrows the tree's species to those at or below it (the
    // walk follows the taxon cache's parent links, so a group above the
    // tree's own root works too); for "All my observations" iNat applies it.
    QList<qint64> leafTaxonIds;
    if (!allObservations) {
        for (const auto &leaf : m_app.taxonomyStore().projectLeafPhotos(pid, m_taxonLimit->taxonId()))
            leafTaxonIds << leaf.inatId;
        if (leafTaxonIds.isEmpty()) {
            QMessageBox::information(
                this, tr("Search iNaturalist"),
                m_taxonLimit->taxonId() > 0
                    ? tr("No species in \"%1\" fall within %2. Choose another taxon, or "
                         "search \"All my observations\" instead.")
                          .arg(m_treeCombo->currentText(), m_taxonLimit->label())
                    : tr("\"%1\" has no species to search for.").arg(m_treeCombo->currentText()));
            return;
        }
    }

    // Confine the search to the tree's own locality too, not just its taxa --
    // otherwise a species that also occurs elsewhere would pull in the user's
    // observations of it from anywhere in the world. Trees built without a
    // place (e.g. from a checklist with no region) simply search worldwide
    // either way; the checkbox lets the user opt out of the restriction too.
    const qint64 placeId = m_restrictToLocality->isChecked() && pid > 0
                               ? m_app.taxonomyStore().projectPlaceInatId(pid).value_or(0)
                               : 0;

    m_searchButton->setEnabled(false);
    m_cancelSearchButton->setEnabled(true);
    m_scopeCombo->setEnabled(false);   // the progress text depends on it
    m_status->setText(placeId > 0 ? tr("Searching…")
                                  : tr("Searching — results aren't restricted by location…"));
    emit statusMessage(tr("Searching iNaturalist for %1's observations…").arg(username), 0);
    if (allObservations)
        m_fetcher->startAll(username, placeId, m_taxonLimit->taxonId());
    else
        m_fetcher->start(username, leafTaxonIds, placeId);
}

void InatDownloadPage::onSearchFinished(bool ok, const QString &error,
                                        QList<inat::Candidate> candidates)
{
    m_searchButton->setEnabled(true);
    m_cancelSearchButton->setEnabled(false);
    m_scopeCombo->setEnabled(true);
    if (!ok) {
        if (error == QLatin1String("cancelled")) {
            m_status->setText(tr("Search stopped."));
            emit statusMessage(tr("Search stopped."), 4000);
        } else {
            m_status->setText(tr("Search failed: %1").arg(error));
            emit statusMessage(tr("Searching iNaturalist failed: %1").arg(error), 10000);
        }
        return;
    }

    m_candidates = candidates;
    recheckAgainstLibrary();
    const int total = m_model->rowCountTotal();
    if (total == 0)
        m_status->setText(
            tr("No candidate photos found — either everything is already in your library, "
               "or this user has no matching observations."));
    emit statusMessage(tr("Found %n candidate photo(s).", nullptr, total), 6000);
}

void InatDownloadPage::recheckAgainstLibrary()
{
    inat::LibraryPresence::annotate(m_app.database().connectionName(), m_candidates);
    m_model->setCandidates(m_candidates);
    m_checkButton->setEnabled(!m_candidates.isEmpty());

    const int total = m_model->rowCountTotal();
    if (total == 0)
        return;

    // Counted per taxon, not per photo -- that's what the user acts on.
    QSet<qint64> taxa, newTaxa, noTreeTaxa;
    for (const inat::Candidate &c : std::as_const(m_candidates)) {
        const qint64 id = c.observation.taxonInatId;
        if (id <= 0 || c.observation.photos.isEmpty())
            continue;
        taxa.insert(id);
        if (!c.inLibrary)
            newTaxa.insert(id);
        if (!c.inAnyTree)
            noTreeTaxa.insert(id);
    }
    m_status->setText(
        tr("%1 candidate photo(s) of %2 taxa. %3 taxa are NEW (no photos in your library) "
           "and %4 are in NO reference TREE — use Show to list just those. Faded photos "
           "look like ones you already have.")
            .arg(total)
            .arg(taxa.size())
            .arg(newTaxa.size())
            .arg(noTreeTaxa.size()));
}

void InatDownloadPage::requestDownload()
{
    const QModelIndexList selected = m_grid->selectionModel()->selectedIndexes();
    if (selected.isEmpty()) {
        QMessageBox::information(this, tr("Download Selected"),
                                 tr("Select at least one photo first."));
        return;
    }

    QList<inat::ImportItem> items;
    for (const QModelIndex &idx : selected) {
        inat::ImportItem item;
        item.observationId = idx.data(inat::InatDownloadModel::ObservationIdRole).toLongLong();
        item.photoId = idx.data(inat::InatDownloadModel::PhotoIdRole).toLongLong();
        item.downloadUrl = idx.data(inat::InatDownloadModel::DownloadUrlRole).toString();
        item.observedOn = idx.data(inat::InatDownloadModel::ObservedOnRole).toString();
        if (idx.data(inat::InatDownloadModel::HasGpsRole).toBool()) {
            item.latitude = idx.data(inat::InatDownloadModel::LatitudeRole).toDouble();
            item.longitude = idx.data(inat::InatDownloadModel::LongitudeRole).toDouble();
        }
        item.taxonName = idx.data(inat::InatDownloadModel::NameRole).toString();
        item.placeGuess = idx.data(inat::InatDownloadModel::PlaceGuessRole).toString();
        items.append(item);
    }
    emit downloadRequested(items);
}

} // namespace pl
