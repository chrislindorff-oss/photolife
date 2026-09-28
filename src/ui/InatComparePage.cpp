#include "ui/InatComparePage.h"

#include "app/Application.h"
#include "db/Database.h"
#include "inat/InatSpeciesListFetcher.h"
#include "inat/LibraryPresence.h"
#include "settings/Settings.h"
#include "taxonomy/TaxonomyStore.h"
#include "ui/InatScopeWidgets.h"
#include "ui/InatToolsView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTextStream>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace pl {
namespace {

// Rows of the Show combo; the summary counts select the same subsets.
enum Subset {
    AllRows,
    OnInat,
    InTrees,
    InBoth,
    OnInatNotInTree,
    OnInatNotInLibrary,
    InTreeNotOnInat,
    SubsetCount,
};

// Bits stored on each row's first column (FlagsRole).
enum Flag { FlagOnInat = 1, FlagInTree = 2, FlagInLibrary = 4 };

constexpr int IdRole = Qt::UserRole + 1;
constexpr int FlagsRole = Qt::UserRole + 2;

enum Column { ColName, ColCommon, ColObservations, ColInTree, ColInLibrary, ColumnCount };

int flagsOf(const inat::ComparisonRow &row)
{
    return (row.onInat ? FlagOnInat : 0) | (row.inTree ? FlagInTree : 0)
           | (row.inLibrary ? FlagInLibrary : 0);
}

bool inSubset(int flags, int subset)
{
    const bool onInat = flags & FlagOnInat;
    const bool inTree = flags & FlagInTree;
    const bool inLibrary = flags & FlagInLibrary;
    switch (subset) {
    case OnInat: return onInat;
    case InTrees: return inTree;
    case InBoth: return onInat && inTree;
    case OnInatNotInTree: return onInat && !inTree;
    case OnInatNotInLibrary: return onInat && !inLibrary;
    case InTreeNotOnInat: return inTree && !onInat;
    default: return true;
    }
}

QString csvField(const QString &text)
{
    if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"'))
        && !text.contains(QLatin1Char('\n')))
        return text;
    QString quoted = text;
    quoted.replace(QLatin1String("\""), QLatin1String("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

} // namespace

// Name filter (scientific or common) plus one Subset.
class ComparisonFilterProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setSubset(int subset)
    {
        m_subset = subset;
        invalidateFilter();
    }
    void setText(const QString &text)
    {
        m_text = text.trimmed();
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const QModelIndex name = sourceModel()->index(row, ColName, parent);
        if (!inSubset(name.data(FlagsRole).toInt(), m_subset))
            return false;
        if (m_text.isEmpty())
            return true;
        return name.data().toString().contains(m_text, Qt::CaseInsensitive)
               || sourceModel()
                      ->index(row, ColCommon, parent)
                      .data()
                      .toString()
                      .contains(m_text, Qt::CaseInsensitive);
    }

private:
    int m_subset = AllRows;
    QString m_text;
};

InatComparePage::InatComparePage(Application &app, InatAccountBar &account, QWidget *parent)
    : QWidget(parent), m_app(app), m_account(account)
{
    m_fetcher = new inat::InatSpeciesListFetcher(m_app.inat(), m_app.taxonomyStore(), this);

    m_observerEdit = new QLineEdit(this);
    m_observerEdit->setPlaceholderText(tr("iNaturalist username"));
    m_observerEdit->setText(m_app.settings().inatUsername());
    m_observerEdit->setToolTip(
        tr("Whose observations to compare. Starts as your account's username; type another "
           "observer's to compare their list with your trees instead."));
    connect(m_observerEdit, &QLineEdit::textEdited, this, [this] { m_observerEdited = true; });
    connect(&m_account, &InatAccountBar::usernameCommitted, this, [this](const QString &name) {
        if (!m_observerEdited)
            m_observerEdit->setText(name);
    });

    m_treeCombo = new ReferenceTreeCombo(tr("All reference trees"), this);
    m_treeCombo->setToolTip(
        tr("The reference tree to compare with, or all of them together. Follows the tree "
           "selected in the Reference Trees panel until you pick one here."));
    connect(m_treeCombo, &QComboBox::currentIndexChanged, this,
            [this] { updateLocalityEnabled(); });

    m_rankCombo = new QComboBox(this);
    for (const auto &[rank, label] :
         {std::pair{QStringLiteral("species"), tr("Species")},
          std::pair{QStringLiteral("genus"), tr("Genus")},
          std::pair{QStringLiteral("family"), tr("Family")},
          std::pair{QStringLiteral("order"), tr("Order")},
          std::pair{QStringLiteral("class"), tr("Class")},
          std::pair{QStringLiteral("phylum"), tr("Phylum")},
          std::pair{QStringLiteral("kingdom"), tr("Kingdom")}})
        m_rankCombo->addItem(label, rank);
    m_rankCombo->setToolTip(
        tr("Compare at this rank: subspecies count as their species, species as their genus, "
           "and so on."));

    m_qualityCombo = new QComboBox(this);
    m_qualityCombo->addItem(tr("All observations"), int(net::ObservationQuality::Any));
    m_qualityCombo->addItem(tr("Verifiable"), int(net::ObservationQuality::Verifiable));
    m_qualityCombo->addItem(tr("Research grade"), int(net::ObservationQuality::Research));
    m_qualityCombo->setToolTip(
        tr("Which of the observer's observations count. Verifiable = research grade plus "
           "needs ID; casual observations are only included under All observations."));

    m_restrictToLocality = new QCheckBox(tr("Restrict to this tree's locality"), this);
    m_restrictToLocality->setToolTip(
        tr("Only count observations made within the chosen tree's own locality. Available when "
           "a single tree with a locality is chosen."));

    m_compareButton = new QPushButton(tr("Compare"), this);
    connect(m_compareButton, &QPushButton::clicked, this, &InatComparePage::compare);
    m_stopButton = new QPushButton(tr("Stop"), this);
    m_stopButton->setEnabled(false);
    connect(m_stopButton, &QPushButton::clicked, this, [this] { m_fetcher->cancel(); });

    auto *observerRow = new QHBoxLayout;
    observerRow->addWidget(new QLabel(tr("Observer:"), this));
    observerRow->addWidget(m_observerEdit, 1);
    observerRow->addWidget(new QLabel(tr("Rank:"), this));
    observerRow->addWidget(m_rankCombo);
    observerRow->addWidget(new QLabel(tr("Observations:"), this));
    observerRow->addWidget(m_qualityCombo);

    auto *treeRow = new QHBoxLayout;
    treeRow->addWidget(new QLabel(tr("Compare with:"), this));
    treeRow->addWidget(m_treeCombo, 1);
    treeRow->addWidget(m_restrictToLocality);
    treeRow->addWidget(m_compareButton);
    treeRow->addWidget(m_stopButton);

    m_taxonLimit = new TaxonLimitRow(m_app.inat(), this);

    auto *summary = new QHBoxLayout;
    m_subsetButtons.resize(SubsetCount, nullptr);
    for (int subset = OnInat; subset < SubsetCount; ++subset) {
        auto *button = new QToolButton(this);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setEnabled(false);
        connect(button, &QToolButton::clicked, this, [this, subset] {
            m_showCombo->setCurrentIndex(m_showCombo->findData(subset));
        });
        m_subsetButtons[subset] = button;
        summary->addWidget(button);
    }

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setText(
        tr("Compare an iNaturalist observer's taxa with your reference trees and photo "
           "library — for example, to find species you've recorded on iNaturalist that are "
           "missing from a tree. Choose a rank and optionally a group, then Compare."));

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(tr("Filter by scientific or common name…"));
    m_filterEdit->setClearButtonEnabled(true);

    m_showCombo = new QComboBox(this);
    for (int subset = AllRows; subset < SubsetCount; ++subset)
        m_showCombo->addItem(tr("Show: %1").arg(subsetLabel(subset)), subset);

    m_exportButton = new QPushButton(tr("Export CSV…"), this);
    m_exportButton->setToolTip(tr("Saves the rows currently shown as a CSV file."));
    m_exportButton->setEnabled(false);
    connect(m_exportButton, &QPushButton::clicked, this, &InatComparePage::exportCsv);

    auto *browseRow = new QHBoxLayout;
    browseRow->addWidget(m_filterEdit, 1);
    browseRow->addWidget(m_showCombo);
    browseRow->addWidget(m_exportButton);

    m_model = new QStandardItemModel(0, ColumnCount, this);
    m_model->setHorizontalHeaderLabels({tr("Taxon"), tr("Common name"),
                                        tr("iNat observations"), tr("In tree"),
                                        tr("In library")});
    m_proxy = new ComparisonFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    connect(m_filterEdit, &QLineEdit::textChanged, m_proxy,
            [this](const QString &text) { m_proxy->setText(text); });
    connect(m_showCombo, &QComboBox::currentIndexChanged, this,
            [this](int i) { m_proxy->setSubset(m_showCombo->itemData(i).toInt()); });

    m_table = new QTreeView(this);
    m_table->setModel(m_proxy);
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(ColName, Qt::AscendingOrder);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->header()->setSectionResizeMode(ColCommon, QHeaderView::Stretch);
    for (int column : {ColObservations, ColInTree, ColInLibrary})
        m_table->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    connect(m_table, &QWidget::customContextMenuRequested, this,
            &InatComparePage::showContextMenu);

    connect(m_fetcher, &inat::InatSpeciesListFetcher::progress, this,
            [this](const QString &phase, int done, int total) {
                emit statusMessage(total > 0 ? tr("%1 %2 / %3").arg(phase).arg(done).arg(total)
                                             : phase,
                                   0);
            });
    connect(m_fetcher, &inat::InatSpeciesListFetcher::finished, this,
            &InatComparePage::onFetchFinished);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(observerRow);
    layout->addLayout(treeRow);
    layout->addWidget(m_taxonLimit);
    layout->addLayout(summary);
    layout->addWidget(m_note);
    layout->addLayout(browseRow);
    layout->addWidget(m_table, 1);

    showResult();
    updateLocalityEnabled();
}

void InatComparePage::setTrees(const QList<std::pair<int, QString>> &trees, int activeProjectId)
{
    m_treeCombo->setTrees(trees, activeProjectId);
}

void InatComparePage::followActiveTree(int projectId)
{
    m_treeCombo->followActiveTree(projectId);
}

void InatComparePage::setTreeSelection(qint64 inatId, const QString &name)
{
    m_taxonLimit->setTreeSelection(inatId, name);
}

QString InatComparePage::subsetLabel(int subset) const
{
    const bool oneTree = m_resultProjectId > 0;
    switch (subset) {
    case OnInat: return tr("On iNaturalist");
    case InTrees: return oneTree ? tr("In this tree") : tr("In my trees");
    case InBoth: return tr("In both");
    case OnInatNotInTree: return tr("On iNat, not in tree");
    case OnInatNotInLibrary: return tr("On iNat, not in library");
    case InTreeNotOnInat: return tr("In tree, not on iNat");
    default: return tr("all");
    }
}

void InatComparePage::updateLocalityEnabled()
{
    const int pid = m_treeCombo->projectId();
    m_restrictToLocality->setEnabled(
        pid > 0 && m_app.taxonomyStore().projectPlaceInatId(pid).value_or(0) > 0);
}

void InatComparePage::compare()
{
    if (m_fetcher->isRunning())
        return;
    const QString observer = m_observerEdit->text().trimmed();
    if (observer.isEmpty()) {
        QMessageBox::information(this, tr("Compare Species"),
                                 tr("Enter an iNaturalist username to compare first."));
        m_observerEdit->setFocus();
        return;
    }
    m_account.commit();   // applies the account's API token, if any

    const int pid = m_treeCombo->projectId();
    const qint64 groupId = m_taxonLimit->taxonId();
    const QList<int> projects = pid > 0 ? QList<int>{pid} : m_treeCombo->allProjectIds();
    QSet<qint64> treeLeaves;
    for (int project : projects) {
        for (const auto &leaf : m_app.taxonomyStore().projectLeafPhotos(project, groupId))
            treeLeaves.insert(leaf.inatId);
    }

    inat::InatSpeciesListFetcher::Request request;
    request.userLogin = observer;
    request.taxonId = groupId;
    request.placeId = m_restrictToLocality->isEnabled() && m_restrictToLocality->isChecked()
                          ? m_app.taxonomyStore().projectPlaceInatId(pid).value_or(0)
                          : 0;
    request.quality = net::ObservationQuality(m_qualityCombo->currentData().toInt());
    request.treeLeafIds = treeLeaves.values();
    m_pendingTreeLeaves = request.treeLeafIds;

    m_resultRank = m_rankCombo->currentData().toString();
    m_resultObserver = observer;
    m_resultProjectId = pid;
    m_resultGroupLabel = groupId > 0 ? m_taxonLimit->label() : QString();

    // A name filter typed for the previous results would otherwise silently
    // hide some or all of the new ones.
    m_filterEdit->clear();

    m_compareButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_note->setText(tr("Comparing…"));
    m_fetcher->start(request);
}

void InatComparePage::onFetchFinished(bool ok, const QString &error,
                                      QList<net::SpeciesCount> inatLeaves,
                                      QHash<qint64, taxonomy::Taxon> known)
{
    m_compareButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    if (!ok) {
        const bool cancelled = error == QLatin1String("cancelled");
        m_note->setText(cancelled ? tr("Comparison stopped.")
                                  : tr("Comparison failed: %1").arg(error));
        emit statusMessage(cancelled ? tr("Comparison stopped.")
                                     : tr("Comparing with iNaturalist failed: %1").arg(error),
                           cancelled ? 4000 : 10000);
        return;
    }

    inat::LibraryPresence presence(m_app.database().connectionName());
    presence.load();
    m_result = inat::compare(inatLeaves, m_pendingTreeLeaves, known, m_resultRank,
                             [&presence](qint64 id, const QString &name, const QString &common) {
                                 return presence.inLibrary(id, name, common);
                             });
    showResult();
    emit statusMessage(tr("Compared %n taxa.", nullptr, int(m_result.rows.size())), 6000);
}

void InatComparePage::showResult()
{
    QList<int> counts(SubsetCount, 0);
    m_model->removeRows(0, m_model->rowCount());
    for (const inat::ComparisonRow &row : std::as_const(m_result.rows)) {
        const int flags = flagsOf(row);
        for (int subset = OnInat; subset < SubsetCount; ++subset) {
            if (inSubset(flags, subset))
                ++counts[subset];
        }

        auto *name = new QStandardItem(row.name);
        name->setData(row.inatId, IdRole);
        name->setData(flags, FlagsRole);
        auto *observations = new QStandardItem;
        observations->setData(row.inatObservations, Qt::DisplayRole);
        auto mark = [](bool on) {
            auto *item = new QStandardItem(on ? QStringLiteral("✓") : QStringLiteral("–"));
            item->setTextAlignment(Qt::AlignCenter);
            return item;
        };
        m_model->appendRow({name, new QStandardItem(row.commonName), observations,
                            mark(row.inTree), mark(row.inLibrary)});
    }

    const bool hasResult = !m_resultRank.isEmpty();
    for (int subset = OnInat; subset < SubsetCount; ++subset) {
        m_subsetButtons[subset]->setText(
            QStringLiteral("%1\n%2").arg(hasResult ? QLocale().toString(counts[subset])
                                                   : QStringLiteral("—"),
                                         subsetLabel(subset)));
        m_subsetButtons[subset]->setEnabled(hasResult);
    }
    for (int i = 0; i < m_showCombo->count(); ++i)
        m_showCombo->setItemText(i, tr("Show: %1").arg(subsetLabel(m_showCombo->itemData(i).toInt())));
    m_exportButton->setEnabled(!m_result.rows.isEmpty());

    if (!hasResult)
        return;
    const QString rankLabel = m_rankCombo->itemText(m_rankCombo->findData(m_resultRank));
    QString note = tr("%1's taxa at rank %2, compared with %3%4.")
                       .arg(m_resultObserver, rankLabel.toLower(),
                            m_resultProjectId > 0 ? tr("\"%1\"").arg(m_treeCombo->itemText(
                                                        m_treeCombo->findData(m_resultProjectId)))
                                                  : tr("all your reference trees"),
                            m_resultGroupLabel.isEmpty()
                                ? QString()
                                : tr(", within %1").arg(m_resultGroupLabel));
    if (m_result.unplacedTaxa > 0)
        note += QLatin1Char(' ')
                + tr("%n observed taxa (%1 observations) couldn't be placed at this rank — "
                     "usually because they're identified only above it — and aren't counted.",
                     nullptr, m_result.unplacedTaxa)
                      .arg(m_result.unplacedObservations);
    m_note->setText(note);
}

void InatComparePage::showContextMenu(const QPoint &pos)
{
    const QModelIndex proxyIndex = m_table->indexAt(pos);
    if (!proxyIndex.isValid())
        return;
    const QModelIndex nameIndex = m_proxy->mapToSource(proxyIndex).siblingAtColumn(ColName);
    const qint64 id = nameIndex.data(IdRole).toLongLong();
    const int flags = nameIndex.data(FlagsRole).toInt();
    const QString name = nameIndex.data().toString();
    const QString common = nameIndex.siblingAtColumn(ColCommon).data().toString();
    const QString label = common.isEmpty() ? name : tr("%1 (%2)").arg(name, common);

    QMenu menu(this);
    QAction *observations =
        menu.addAction(tr("View %1's Observations on iNaturalist").arg(m_resultObserver));
    connect(observations, &QAction::triggered, this, [this, id] {
        QUrl url(QStringLiteral("https://www.inaturalist.org/observations"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("user_id"), m_resultObserver);
        q.addQueryItem(QStringLiteral("taxon_id"), QString::number(id));
        url.setQuery(q);
        QDesktopServices::openUrl(url);
    });
    observations->setEnabled(flags & FlagOnInat);
    menu.addAction(tr("Open Taxon on iNaturalist"), this, [id] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(id)));
    });

    menu.addSeparator();
    const int pid = m_resultProjectId;
    QAction *show = menu.addAction(tr("Show in Reference Tree"), this,
                                   [this, pid, id] { emit showInTreeRequested(pid, id); });
    show->setEnabled(pid > 0 && (flags & FlagInTree));
    QAction *add = menu.addAction(tr("Add to Reference Tree"), this,
                                  [this, pid, id, name] { emit addToTreeRequested(pid, id, name); });
    add->setEnabled(pid > 0 && !(flags & FlagInTree));
    if (pid <= 0) {
        show->setToolTip(tr("Compare with a single tree to use this."));
        add->setToolTip(tr("Compare with a single tree to use this."));
        menu.setToolTipsVisible(true);
    }

    const bool ownList =
        m_resultObserver.compare(m_account.username(), Qt::CaseInsensitive) == 0;
    if (ownList) {
        menu.addSeparator();
        QAction *download = menu.addAction(tr("Download My Observations of This Taxon"), this,
                                           [this, id, label] {
                                               emit downloadTaxonRequested(id, label);
                                           });
        download->setEnabled(flags & FlagOnInat);
    }
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void InatComparePage::exportCsv()
{
    const QString suggested = QDir::home().filePath(
        QStringLiteral("%1-%2-comparison.csv").arg(m_resultObserver, m_resultRank));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Comparison"), suggested,
                                                      tr("CSV files (*.csv)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export Comparison"),
                             tr("Could not write %1: %2").arg(path, file.errorString()));
        return;
    }
    QTextStream out(&file);
    out << "iNat taxon id,Taxon,Common name,iNat observations,In tree,In library\n";
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        const QModelIndex name = m_proxy->mapToSource(m_proxy->index(r, ColName));
        const int flags = name.data(FlagsRole).toInt();
        out << name.data(IdRole).toLongLong() << ',' << csvField(name.data().toString()) << ','
            << csvField(name.siblingAtColumn(ColCommon).data().toString()) << ','
            << name.siblingAtColumn(ColObservations).data().toInt() << ','
            << ((flags & FlagInTree) ? "yes" : "no") << ','
            << ((flags & FlagInLibrary) ? "yes" : "no") << '\n';
    }
    emit statusMessage(tr("Exported %n row(s) to %1.", nullptr, m_proxy->rowCount()).arg(path),
                       6000);
}

} // namespace pl
