#include "ui/TargetsPage.h"

#include "app/Application.h"
#include "catalogue/SeenStore.h"
#include "coverage/LifeList.h"
#include "db/Database.h"
#include "geo/LatLon.h"
#include "net/GeocodeClient.h"
#include "settings/Settings.h"
#include "taxonomy/TaxonomyStore.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {
namespace {
enum Column { CountColumn, NameColumn, SeenColumn };
constexpr int kInatIdRole = Qt::UserRole + 1;
constexpr int kPerPage = 500;   // iNaturalist's most per page: the most recorded 500 are plenty
} // namespace

TargetsPage::TargetsPage(Application &app, QWidget *parent) : QWidget(parent), m_app(app)
{
    auto *intro = new QLabel(tr("What could you photograph there? The species of this tree that iNaturalist has "
                                "records of near a place, in a month, that you haven't photographed yet — the most "
                                "recorded first."),
                             this);
    intro->setWordWrap(true);
    m_where = new QLineEdit(this);
    m_where->setPlaceholderText(tr("A place (Anakie, Victoria) or coordinates (-37.90, 144.22)"));
    m_where->setClearButtonEnabled(true);
    connect(m_where, &QLineEdit::returnPressed, this, &TargetsPage::findPlace);
    m_place = new QLabel(this);
    m_place->setObjectName(QStringLiteral("muted"));
    m_radius = new QSpinBox(this);
    m_radius->setRange(1, 200);
    m_radius->setValue(25);
    m_radius->setSuffix(tr(" km"));
    m_radius->setToolTip(tr("How far from the place to count records."));
    m_month = new QComboBox(this);
    m_month->addItem(tr("Any month"), 0);
    for (int m = 1; m <= 12; ++m)
        m_month->addItem(QLocale().standaloneMonthName(m), m);
    m_month->setCurrentIndex(QDate::currentDate().month());
    m_hideSeen = new QCheckBox(tr("Hide ones I've seen"), this);
    m_hideSeen->setToolTip(tr("Leave out species ticked in the Seen column (seen, not photographed)."));
    connect(m_hideSeen, &QCheckBox::toggled, this, &TargetsPage::fill);
    m_find = new QPushButton(tr("Find Targets"), this);
    m_find->setDefault(true);
    connect(m_find, &QPushButton::clicked, this, &TargetsPage::findPlace);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Near:"), this));
    row->addWidget(m_where, 1);
    row->addWidget(new QLabel(tr("within"), this));
    row->addWidget(m_radius);
    row->addWidget(new QLabel(tr("in"), this));
    row->addWidget(m_month);
    row->addWidget(m_hideSeen);
    row->addWidget(m_find);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Records nearby"), tr("Species"), tr("Seen")});
    m_tree->setRootIsDecorated(false);
    m_tree->setSortingEnabled(true);
    m_tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        emit showTaxon(item->data(NameColumn, kInatIdRole).toLongLong());
    });
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_tree->itemAt(pos);
        if (!item)
            return;
        const qint64 id = item->data(NameColumn, kInatIdRole).toLongLong();
        QMenu menu(this);
        menu.addAction(tr("Show in the Reference Tree"), this, [this, id] { emit showTaxon(id); });
        menu.addAction(tr("Open on iNaturalist"), this, [id] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(id)));
        });
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(row);
    layout->addWidget(m_place);
    layout->addWidget(m_status);
    layout->addWidget(m_tree, 1);
}

void TargetsPage::setProject(int projectId)
{
    if (projectId == m_projectId)
        return;
    m_projectId = projectId;
    m_results.clear();
    m_tree->clear();
    m_status->setText(projectId > 0 ? QString() : tr("Select a reference tree."));
}

void TargetsPage::findPlace()
{
    const QString text = m_where->text().trimmed();
    if (text.isEmpty()) {
        m_place->setText(tr("Type a place, or paste coordinates."));
        return;
    }
    if (const std::optional<QPointF> at = geo::parseLatLon(text)) {
        m_at = QPointF(at->y(), at->x());   // parseLatLon: x = longitude
        m_place->setText(tr("At %1, %2").arg(m_at->x(), 0, 'f', 4).arg(m_at->y(), 0, 'f', 4));
        find();
        return;
    }
    m_place->setText(tr("Finding %1…").arg(text));
    QPointer<TargetsPage> self(this);
    m_app.geocoder().search(text, [self, text](net::Outcome<QList<net::geocode::Place>> out) {
        if (!self)
            return;
        if (!out.ok() || out.value.isEmpty()) {
            self->m_place->setText(out.ok() ? tr("Nothing found for “%1”.").arg(text)
                                            : tr("The place couldn't be looked up (%1).").arg(out.error));
            return;
        }
        const net::geocode::Place &p = out.value.first();
        self->m_at = QPointF(p.latitude, p.longitude);
        self->m_place->setText(p.name);
        self->find();
    });
}

void TargetsPage::find()
{
    if (!m_at || m_projectId <= 0 || !m_app.database().isOpen()) {
        if (m_projectId <= 0)
            m_status->setText(tr("Select a reference tree."));
        return;
    }
    const taxonomy::TaxonomyStore store(m_app.database().connectionName());
    const qint64 root = store.projectRootTaxonInatId(m_projectId).value_or(0);
    const int generation = ++m_generation;
    m_status->setText(tr("Asking iNaturalist…"));
    m_find->setEnabled(false);
    QPointer<TargetsPage> self(this);
    m_app.inat().speciesCountsNear(
        root, m_at->x(), m_at->y(), m_radius->value(), m_month->currentData().toInt(), 1, kPerPage,
        [self, generation](net::Outcome<net::SpeciesCountsPage> out) {
            if (!self || generation != self->m_generation)
                return;
            self->m_find->setEnabled(true);
            if (!out.ok()) {
                self->m_status->setText(tr("iNaturalist couldn't be asked: %1").arg(out.error));
                return;
            }
            // The tree's species only, not photographed: a photo of a
            // subspecies counts for its species.
            const QString connection = self->m_app.database().connectionName();
            const QList<qint64> inTree = taxonomy::TaxonomyStore(connection).projectTaxonInatIds(self->m_projectId);
            const QSet<qint64> tree(inTree.cbegin(), inTree.cend());
            QSet<qint64> photographed;
            for (const coverage::LifeListEntry &e : coverage::loadLifeList(connection, self->m_projectId))
                photographed.insert(e.inatId);
            self->m_results.clear();
            int alreadyHave = 0, outside = 0;
            for (const net::SpeciesCount &c : std::as_const(out.value.results)) {
                if (!tree.contains(c.taxon.inatId)) {
                    ++outside;
                    continue;
                }
                if (photographed.contains(c.taxon.inatId)) {
                    ++alreadyHave;
                    continue;
                }
                self->m_results << c;
            }
            self->fill();
            self->m_status->setText(
                tr("%n species recorded there that you haven't photographed", nullptr, int(self->m_results.size()))
                + tr("; you've photographed %n of the tree's others there", nullptr, alreadyHave)
                + (outside > 0 ? tr(" (%n more aren't in this tree)", nullptr, outside) : QString())
                + (out.value.totalResults > kPerPage
                       ? QStringLiteral(". ")
                             + tr("Only the %1 most recorded of %2 were looked at.").arg(kPerPage).arg(out.value.totalResults)
                       : QStringLiteral(".")));
        });
}

void TargetsPage::fill()
{
    m_tree->setSortingEnabled(false);
    m_tree->clear();
    const QSet<qint64> seen = catalogue::SeenStore(m_app.database().connectionName()).seenTaxa();
    for (const net::SpeciesCount &c : std::as_const(m_results)) {
        const bool isSeen = seen.contains(c.taxon.inatId);
        if (isSeen && m_hideSeen->isChecked())
            continue;
        auto *item = new QTreeWidgetItem(m_tree);
        item->setData(CountColumn, Qt::DisplayRole, c.count);
        item->setTextAlignment(CountColumn, Qt::AlignRight | Qt::AlignVCenter);
        const bool hasCommon = !c.taxon.commonName.isEmpty();
        item->setText(NameColumn, hasCommon ? QStringLiteral("%1 (%2)").arg(c.taxon.commonName, c.taxon.name) : c.taxon.name);
        item->setData(NameColumn, kInatIdRole, c.taxon.inatId);
        item->setText(SeenColumn, isSeen ? QStringLiteral("✓") : QString());
        item->setTextAlignment(SeenColumn, Qt::AlignCenter);
    }
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(CountColumn, Qt::DescendingOrder);
    m_tree->resizeColumnToContents(CountColumn);
}

} // namespace pl
