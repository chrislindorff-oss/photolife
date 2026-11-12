#include "ui/InatUploadPage.h"

#include "ui/AnnotationsDialog.h"

#include "app/Application.h"
#include "db/Database.h"
#include "inat/InatDuplicateCheck.h"
#include "inat/InatToken.h"
#include "geo/LatLon.h"
#include "inat/ObservationUploader.h"
#include "net/INatClient.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"
#include "ui/IdentifyPanel.h"
#include "ui/ImageViewer.h"
#include "ui/InatTokenGetter.h"
#include "ui/InatToolsView.h"
#include "ui/TaxonPickerDialog.h"
#include "ui/Theme.h"
#include "ui/UploadDuplicateDialog.h"
#include "ui/UploadPhotoPicker.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPainter>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace pl {

namespace {

enum Column { SpeciesColumn, DateColumn, PlaceColumn, PhotosColumn, NoteColumn, StatusColumn };

constexpr int kRowRole = Qt::UserRole;        // a top-level item's row
constexpr int kPhotoRole = Qt::UserRole + 1;  // a photo item's index in its row
constexpr int kSentRole = Qt::UserRole + 3;   // on iNaturalist already: washed green

QString speciesText(const taxonomy::Taxon &taxon)
{
    if (taxon.inatId <= 0)
        return QObject::tr("(no species)");
    if (taxon.commonName.isEmpty())
        return taxon.name;
    return QStringLiteral("%1 · %2").arg(taxon.commonName, taxon.name);
}

QUrl observationUrl(qint64 id)
{
    return QUrl(QStringLiteral("https://www.inaturalist.org/observations/%1").arg(id));
}

// `one` for 1, else `many` with the number.
QString plural(int n, const QString &one, const QString &many)
{
    return n == 1 ? one : many.arg(n);
}

const QStringList kGeoprivacy{QStringLiteral("open"), QStringLiteral("obscured"), QStringLiteral("private")};

// The observations, whose photos can be dragged: to reorder them, or onto
// another observation. The move is the page's, made once the drag is over
// (queued), so nothing Qt is holding goes away under it.
class UploadTree : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;
    std::function<void(int fromRow, int from, int toRow, int to)> onDrop;

protected:
    // What's on iNaturalist already, in a light green wash across the whole
    // row. (The theme's item style would hide an item's own background.)
    void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.siblingAtColumn(0).data(kSentRole).toBool()) {
            QColor wash = themeColors(currentThemeVariant()).positive;
            wash.setAlpha(55);
            painter->fillRect(option.rect, wash);
        }
        QTreeWidget::drawRow(painter, option, index);
    }

    void dropEvent(QDropEvent *event) override
    {
        const QList<QTreeWidgetItem *> dragged = selectedItems();
        QTreeWidgetItem *target = itemAt(event->position().toPoint());
        const DropIndicatorPosition position = dropIndicatorPosition();
        event->setDropAction(Qt::CopyAction);   // so Qt takes nothing out itself
        event->accept();
        if (dragged.size() != 1 || !dragged.first()->parent() || !target || !onDrop)
            return;
        const int fromRow = dragged.first()->parent()->data(0, kRowRole).toInt();
        const int from = dragged.first()->data(0, kPhotoRole).toInt();
        int toRow = 0, to = 0;
        if (target->parent()) {
            toRow = target->parent()->data(0, kRowRole).toInt();
            to = target->data(0, kPhotoRole).toInt() + (position == BelowItem ? 1 : 0);
        } else {
            toRow = target->data(0, kRowRole).toInt();
            to = target->childCount();
        }
        if (toRow == fromRow && to > from)
            --to;   // counted with the photo still in its place
        QMetaObject::invokeMethod(
            this, [drop = onDrop, fromRow, from, toRow, to] { drop(fromRow, from, toRow, to); }, Qt::QueuedConnection);
    }
};

} // namespace

InatUploadPage::InatUploadPage(Application &app, InatAccountBar &account, QWidget *parent)
    : QWidget(parent), m_app(app), m_account(account)
{
    m_uploader = new inat::ObservationUploader(m_app.inat(), m_app.database().connectionName(), this);
    m_check = new inat::InatDuplicateCheck(m_app.inat(), m_app.database().connectionName(), this);
    connect(m_check, &inat::InatDuplicateCheck::finished, this, &InatUploadPage::checkFinished);
    connect(m_check, &inat::InatDuplicateCheck::progress, this, [this](int done, int total) {
        m_message->setText(tr("Looking for these among your iNaturalist observations… %1 of %2").arg(done).arg(total));
    });

    m_who = new QLabel(this);
    m_who->setTextFormat(Qt::RichText);
    m_who->setWordWrap(true);

    m_gap = new QSpinBox(this);
    m_gap->setRange(1, 24 * 60);
    m_gap->setSuffix(tr(" min"));
    m_gap->setValue(m_app.settings().inatUploadGapMinutes());
    m_gap->setToolTip(tr("Photos of a species taken no more than this far apart go up as one observation."));
    connect(m_gap, &QSpinBox::valueChanged, this, [this](int v) { m_app.settings().setInatUploadGapMinutes(v); });
    m_byDistance = new QCheckBox(tr("and within"), this);
    m_byDistance->setChecked(m_app.settings().inatUploadSplitByDistance());
    m_byDistance->setToolTip(tr("Also start a new observation where a photo was taken further than this from the "
                                "last one with a location. Photos without a location go by time alone."));
    m_distance = new QSpinBox(this);
    m_distance->setRange(50, 50000);
    m_distance->setSingleStep(100);
    m_distance->setSuffix(tr(" m"));
    m_distance->setValue(m_app.settings().inatUploadDistanceMetres());
    m_distance->setEnabled(m_byDistance->isChecked());
    m_distance->setToolTip(m_byDistance->toolTip());
    connect(m_byDistance, &QCheckBox::toggled, this, [this](bool on) {
        m_app.settings().setInatUploadSplitByDistance(on);
        m_distance->setEnabled(on);
    });
    connect(m_distance, &QSpinBox::valueChanged, this, [this](int v) { m_app.settings().setInatUploadDistanceMetres(v); });
    m_regroup = new QPushButton(tr("Regroup"), this);
    m_regroup->setToolTip(tr("Groups the photos again with this gap (and distance). Merges and splits made here "
                             "are undone; species chosen here are kept."));
    connect(m_regroup, &QPushButton::clicked, this, &InatUploadPage::regroup);

    m_geoprivacy = new QComboBox(this);
    m_geoprivacy->addItem(tr("Open"), kGeoprivacy.at(0));
    m_geoprivacy->addItem(tr("Obscured"), kGeoprivacy.at(1));
    m_geoprivacy->addItem(tr("Private"), kGeoprivacy.at(2));
    m_geoprivacy->setCurrentIndex(std::max(0, int(kGeoprivacy.indexOf(m_app.settings().inatUploadGeoprivacy()))));
    m_geoprivacy->setToolTip(tr("Open: the location is public. Obscured: the public sees an area of about 20 km, "
                                "you see the point. Private: the location is hidden."));
    connect(m_geoprivacy, &QComboBox::currentIndexChanged, this,
            [this] { m_app.settings().setInatUploadGeoprivacy(m_geoprivacy->currentData().toString()); });

    m_photoSize = new QSlider(Qt::Horizontal, this);
    m_photoSize->setRange(64, 256);
    m_photoSize->setSingleStep(16);
    m_photoSize->setPageStep(32);
    m_photoSize->setValue(m_app.settings().inatUploadThumbPx());
    m_photoSize->setFixedWidth(120);
    m_photoSize->setToolTip(tr("How big the photos are in the list. Double-click a photo to see it full screen."));
    connect(m_photoSize, &QSlider::valueChanged, this, [this](int px) {
        m_app.settings().setInatUploadThumbPx(px);
        m_tree->setIconSize(QSize(px, px));
    });

    // Two rows, to keep the view narrow enough to sit beside the docked panels.
    auto *top = new QHBoxLayout;
    top->addWidget(m_who, 1);
    top->addWidget(new QLabel(tr("Photo size:"), this));
    top->addWidget(m_photoSize);
    top->addSpacing(16);
    top->addWidget(new QLabel(tr("Geoprivacy:"), this));
    top->addWidget(m_geoprivacy);
    auto *grouping = new QHBoxLayout;
    grouping->addWidget(new QLabel(tr("One observation per species within"), this));
    grouping->addWidget(m_gap);
    grouping->addWidget(m_byDistance);
    grouping->addWidget(m_distance);
    grouping->addWidget(m_regroup);
    grouping->addStretch(1);

    auto *tree = new UploadTree(this);
    tree->onDrop = [this](int fromRow, int from, int toRow, int to) { movePhoto(fromRow, from, toRow, to); };
    m_tree = tree;
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setDropIndicatorShown(true);
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({tr("Observation"), tr("Taken"), tr("Location"), tr("Photos"), tr("Note"), tr("Status")});
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setIconSize(QSize(m_photoSize->value(), m_photoSize->value()));
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &InatUploadPage::contextMenuAt);
    m_tree->header()->setSectionResizeMode(SpeciesColumn, QHeaderView::Stretch);
    for (int column : {DateColumn, PlaceColumn, PhotosColumn, StatusColumn})
        m_tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(NoteColumn, QHeaderView::Interactive);
    m_tree->header()->resizeSection(NoteColumn, 200);
    m_tree->header()->setStretchLastSection(false);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &InatUploadPage::updateButtons);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (item->parent())
            return;
        const int r = item->data(0, kRowRole).toInt();
        if (r < 0 || r >= m_rows.size())
            return;
        if (column == SpeciesColumn) {
            m_rows[r].use = item->checkState(SpeciesColumn) == Qt::Checked;
            // Ticking one that's held: the user says it's new.
            if (m_rows[r].use && m_rows[r].held()) {
                m_rows[r].asNew = true;
                refreshRow(r);
            }
        }
        updateButtons();
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (item->parent()) {   // a photo: see it
            viewPhoto(item->parent()->data(0, kRowRole).toInt(), item->data(0, kPhotoRole).toInt());
            return;
        }
        const int r = item->data(0, kRowRole).toInt();
        if (r < 0 || r >= m_rows.size())
            return;
        if (column == NoteColumn && !isBusy() && m_rows.at(r).obs.observationId <= 0)
            editNote(r);
        else if (column == StatusColumn && !m_rows.at(r).hits.isEmpty() && !isBusy())
            reviewMatch();
        else if (m_rows.at(r).obs.observationId > 0)
            openOnInat();
        else
            viewPhoto(r, 0);
    });

    // Shown while there's nothing to upload.
    auto *empty = new QWidget(this);
    auto *emptyAdd = new QPushButton(tr("Add Photos…"), empty);
    connect(emptyAdd, &QPushButton::clicked, this, &InatUploadPage::addPhotos);
    auto *emptyText = new QLabel(
        tr("Add the photos to upload: search the library for them, or choose a folder of new photos, here; "
           "or select them in the Library and right-click → <b>Upload to iNaturalist…</b><br><br>"
           "Photos of the same species taken close together become one observation, with the species, "
           "date and location the photos already have. Before anything is sent, PhotoLife looks for each "
           "among your observations already on iNaturalist. Each photo is sent as a 2048 px copy; your files "
           "aren't changed."),
        empty);
    emptyText->setWordWrap(true);
    emptyText->setAlignment(Qt::AlignCenter);
    QPalette muted = emptyText->palette();
    muted.setColor(QPalette::WindowText, themeColors(currentThemeVariant()).mutedText);
    emptyText->setPalette(muted);
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch(1);
    emptyLayout->addWidget(emptyText);
    emptyLayout->addWidget(emptyAdd, 0, Qt::AlignHCenter);
    emptyLayout->addStretch(2);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(empty);
    m_stack->addWidget(m_tree);

    auto button = [this](const QString &text, const QString &tip, void (InatUploadPage::*slot)()) {
        auto *b = new QPushButton(text, this);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, slot);
        return b;
    };
    m_add = button(tr("Add Photos…"), tr("Search the library, or choose a folder, for photos to upload."),
                   &InatUploadPage::addPhotos);
    m_species = button(tr("Choose Species…"), tr("Give the selected observations a species from the catalogue."),
                       &InatUploadPage::chooseSpecies);
    m_identify = button(tr("Identify…"), tr("Ask iNaturalist's computer vision what the selected photo shows "
                                            "(or the observation's first photo)."),
                        &InatUploadPage::identify);
    m_location = button(tr("Set Location…"), tr("Put a pin on a map where these photos were taken — saved to "
                                                "the photos, as Set Location on Map does."),
                        &InatUploadPage::setLocation);
    m_date = button(tr("Set Date…"), tr("When it was seen, for an observation none of whose photos has a date."),
                    &InatUploadPage::setDate);
    m_merge = button(tr("Merge"), tr("Make the selected observations one."), &InatUploadPage::merge);
    m_split = button(tr("Split Here"), tr("Start a new observation at the selected photo."), &InatUploadPage::split);
    auto arrow = [this](QStyle::StandardPixmap icon, const QString &tip, const QKeySequence &keys, int by) {
        auto *b = new QPushButton(this);
        b->setIcon(style()->standardIcon(icon));
        b->setToolTip(tip);
        b->setShortcut(keys);
        connect(b, &QPushButton::clicked, this, [this, by] { moveSelected(by); });
        return b;
    };
    m_up = arrow(QStyle::SP_ArrowUp, tr("Move the selected photo earlier (Alt+Up). The first photo is the one "
                                 "iNaturalist shows. You can drag photos too, also onto another observation."),
                 QKeySequence(Qt::ALT | Qt::Key_Up), -1);
    m_down = arrow(QStyle::SP_ArrowDown, tr("Move the selected photo later (Alt+Down)."), QKeySequence(Qt::ALT | Qt::Key_Down), 1);
    m_remove = button(tr("Remove"), tr("Take the selected observations or photos out of this upload."),
                      &InatUploadPage::removeSelected);
    m_checkButton = button(tr("Check iNaturalist"),
                           tr("Look for these among your observations already on iNaturalist, by species, "
                              "day and place. Upload does this too."),
                           &InatUploadPage::checkNow);
    m_review = button(tr("Review Match…"), tr("Compare the selected observation with the one on iNaturalist it "
                                              "looks like, and say whether it's the same."),
                      &InatUploadPage::reviewMatch);
    m_open = button(tr("Open on iNaturalist"), tr("Open the selected observation in your browser."),
                    &InatUploadPage::openOnInat);
    // As tall as the buttons beside them, and square.
    const int side = m_split->sizeHint().height();
    m_up->setFixedSize(side, side);
    m_down->setFixedSize(side, side);
    // Three short rows rather than one long one -- what to send, how it's
    // grouped and checked, and sending it: the view's minimum width is its
    // widest row, and the main window has the tree panel and the guide
    // beside it.
    auto *edits = new QHBoxLayout;
    for (QPushButton *b : {m_add, m_species, m_identify, m_location, m_date, m_remove})
        edits->addWidget(b);
    edits->addStretch(1);
    auto *arrange = new QHBoxLayout;
    for (QPushButton *b : {m_merge, m_split, m_up, m_down})
        arrange->addWidget(b);
    arrange->addStretch(1);
    for (QPushButton *b : {m_checkButton, m_review, m_open})
        arrange->addWidget(b);

    m_upload = button(tr("Upload"), {}, &InatUploadPage::upload);
    m_upload->setDefault(true);
    m_stop = new QPushButton(tr("Stop"), this);
    m_stop->setToolTip(tr("Stops once the photo being sent has gone."));
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_check->isRunning()) {
            m_checkStopped = true;
            m_check->cancel();
        }
        else
            m_uploader->stop();
        m_stop->setEnabled(false);
        m_message->setText(tr("Stopping…"));
    });
    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    m_message->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_message->setOpenExternalLinks(true);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_message, 1);
    bottom->addWidget(m_progress);
    bottom->addWidget(m_stop);
    bottom->addWidget(m_upload);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addLayout(grouping);
    layout->addWidget(m_stack, 1);
    layout->addLayout(edits);
    layout->addLayout(arrange);
    layout->addLayout(bottom);

    connect(m_uploader, &inat::ObservationUploader::jobStarted, this, [this](int r) {
        if (r < 0 || r >= m_rows.size())
            return;
        if (m_rows[r].obs.observationId <= 0)
            m_rows[r].obs.uuidSent = true;
        m_rows[r].status = tr("Sending…");
        m_rows[r].failed = false;
        refreshRow(r);
    });
    connect(m_uploader, &inat::ObservationUploader::observationMade, this, [this](int r, qint64 id) {
        if (r < 0 || r >= m_rows.size())
            return;
        m_rows[r].obs.observationId = id;
        for (inat::UploadPhoto &p : m_rows[r].obs.photos)
            p.observationId = id;
        refreshRow(r);
    });
    connect(m_uploader, &inat::ObservationUploader::photoSent, this,
            [this](int r, int captureId, qint64 photoId, int sent, int total) {
        if (r < 0 || r >= m_rows.size())
            return;
        for (inat::UploadPhoto &p : m_rows[r].obs.photos)
            if (p.captureId == captureId)
                p.photoId = photoId;
        m_rows[r].status = tr("Sent %1 of %2 photos").arg(sent).arg(total);
        m_progress->setValue(m_progress->value() + 1);
        refreshRow(r);
    });
    connect(m_uploader, &inat::ObservationUploader::jobFinished, this, [this](int r, const QString &error, int) {
        if (r < 0 || r >= m_rows.size())
            return;
        m_rows[r].failed = !error.isEmpty();
        m_rows[r].status = error.isEmpty() ? QString() : tr("Failed: %1").arg(error);
        if (error.isEmpty()) {
            m_rows[r].use = false;
            m_sentThisRun << r;
        }
        refreshRow(r);
    });
    connect(m_uploader, &inat::ObservationUploader::annotationsFailed, this, [this](int r, const QString &error) {
        if (r < 0 || r >= m_rows.size())
            return;
        m_rows[r].status = tr("Made, but its annotations couldn't be added (%1): add them on iNaturalist").arg(error);
        refreshRow(r);
    });
    connect(m_uploader, &inat::ObservationUploader::finished, this, &InatUploadPage::uploadFinished);

    connect(&m_app, &Application::inatApiTokenChanged, this, [this] { showWho(); });
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, [this](const QString &, int px) {
        if (px == thumb::ThumbnailCache::kGridPx && !isBusy())
            rebuild();
    });

    showWho();
    rebuild();
}

bool InatUploadPage::isUploading() const
{
    return m_uploader->isRunning();
}

bool InatUploadPage::isBusy() const
{
    return m_uploader->isRunning() || m_check->isRunning();
}

void InatUploadPage::changed(int r)
{
    Row &row = m_rows[r];
    row.obs.checked = false;
    row.hits.clear();
    row.asNew = false;
}

void InatUploadPage::showWho()
{
    const QString token = m_app.settings().inatApiToken();
    if (!InatTokenGetter::isUsable(token)) {
        m_who->setText(tr("Uploading needs your iNaturalist token: <b>Get Token…</b> above."));
        return;
    }
    m_who->setText(tr("Checking your iNaturalist account…"));
    m_login.clear();
    m_app.inat().setAccessToken(token);
    QPointer<InatUploadPage> self(this);
    m_app.inat().fetchMe([self](net::Outcome<QString> out) {
        if (!self)
            return;
        if (out.ok()) {
            self->m_login = out.value;
            self->m_who->setText(tr("Uploading as <b>@%1</b>").arg(out.value.toHtmlEscaped()));
        }
        else if (out.status == 401)
            self->m_who->setText(tr("Your token has expired: <b>Get Token…</b> above."));
        else
            self->m_who->setText(tr("Couldn't reach iNaturalist (%1).").arg(out.error.toHtmlEscaped()));
    });
}

QString InatUploadPage::statusOf(const Row &row) const
{
    if (!row.status.isEmpty())
        return row.status;
    if (row.obs.uploaded()) {
        const auto &photos = row.obs.photos;
        if (std::all_of(photos.cbegin(), photos.cend(), [](const inat::UploadPhoto &p) { return !p.copyOf.isEmpty(); }))
            return tr("On iNaturalist (copies of photos there)");
        if (std::all_of(photos.cbegin(), photos.cend(), [](const inat::UploadPhoto &p) { return p.linked; }))
            return tr("On iNaturalist (the same sighting; these photos weren't sent)");
        return tr("On iNaturalist");
    }
    if (row.held()) {
        const inat::DuplicateHit &hit = row.hits.constFirst();
        return tr("Maybe already on iNaturalist (#%1: %2)").arg(hit.existing.id).arg(hit.reasons);
    }
    QStringList needs;
    const int problems = row.obs.problems();
    if (problems & inat::ProposedObservation::NoSpecies)
        needs << tr("a species");
    if (problems & inat::ProposedObservation::NoLocation)
        needs << tr("a location");
    if (problems & inat::ProposedObservation::NoDate)
        needs << tr("a date");
    if (!needs.isEmpty())
        return tr("Needs %1").arg(needs.join(tr(", ")));
    if (row.obs.observationId > 0)
        return plural(row.obs.remaining(), tr("On iNaturalist, 1 photo still to send"),
                      tr("On iNaturalist, %1 photos still to send"));
    if (row.asNew)
        return tr("Ready (a different sighting from #%1)").arg(row.hits.constFirst().existing.id);
    return tr("Ready");
}

void InatUploadPage::rebuild()
{
    // What was selected, by capture, so it can be selected again.
    QSet<int> selectedRowsBefore, selectedCaptures;
    for (QTreeWidgetItem *item : m_tree->selectedItems()) {
        if (item->parent())
            selectedCaptures.insert(item->data(0, kPhotoRole + 1).toInt());
        else
            selectedRowsBefore.insert(item->data(0, kRowRole).toInt());
    }

    const QSignalBlocker block(m_tree);
    m_tree->clear();
    for (int r = 0; r < m_rows.size(); ++r) {
        auto *top = new QTreeWidgetItem(m_tree);
        top->setData(0, kRowRole, r);
        top->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsEditable
                      | Qt::ItemIsDropEnabled);
        const Row &row = m_rows.at(r);
        for (int i = 0; i < row.obs.photos.size(); ++i) {
            const inat::UploadPhoto &p = row.obs.photos.at(i);
            auto *child = new QTreeWidgetItem(top);
            child->setData(0, kPhotoRole, i);
            child->setData(0, kPhotoRole + 1, p.captureId);
            const bool movable = inat::ObservationGrouper::canMove({row.obs}, 0, i, 0);
            child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (movable ? Qt::ItemIsDragEnabled : Qt::NoItemFlags));
            child->setText(SpeciesColumn, p.label);
            if (p.taken.isValid())
                child->setText(DateColumn, p.hasTime ? QLocale().toString(p.taken, QLocale::ShortFormat)
                                                     : QLocale().toString(p.taken.date(), QLocale::ShortFormat));
            if (p.latitude && p.longitude)
                child->setText(PlaceColumn, QStringLiteral("%1, %2").arg(*p.latitude, 0, 'f', 4).arg(*p.longitude, 0, 'f', 4));
            if (p.photoId > 0)
                child->setText(StatusColumn, tr("Sent"));
            else if (!p.copyOf.isEmpty())
                child->setText(StatusColumn, tr("A copy of %1 is there").arg(p.copyOf));
            else if (p.linked)
                child->setText(StatusColumn, tr("Not sent: the same sighting"));
            child->setData(0, kSentRole, p.done());
            if (p.done())
                child->setToolTip(StatusColumn, tr("Already on iNaturalist: it won't be sent again."));
            if (i == 0 && row.obs.photos.size() > 1) {
                child->setText(NoteColumn, tr("Cover"));
                child->setToolTip(NoteColumn, tr("The photo iNaturalist shows for this observation."));
            }
            const QPixmap thumb = m_app.thumbnails().thumbnail(p.contentHash, p.path, thumb::ThumbnailCache::kGridPx);
            if (!thumb.isNull())
                child->setIcon(SpeciesColumn, QIcon(thumb));
            child->setSelected(selectedCaptures.contains(p.captureId));
        }
        top->setExpanded(true);
        top->setSelected(selectedRowsBefore.contains(r));
        refreshRow(r);
    }
    m_stack->setCurrentIndex(m_rows.isEmpty() ? 0 : 1);
    updateButtons();
}

void InatUploadPage::refreshRow(int r)
{
    QTreeWidgetItem *top = m_tree->topLevelItem(r);
    if (!top || r >= m_rows.size())
        return;
    const QSignalBlocker block(m_tree);
    const Row &row = m_rows.at(r);
    const inat::ProposedObservation &obs = row.obs;
    top->setText(SpeciesColumn, speciesText(obs.taxon) + (obs.taxonFromVision ? tr("  (from Identify)") : QString()));
    const QDateTime at = obs.observedAt();
    top->setText(DateColumn, !at.isValid() ? QString()
                             : obs.hasTime() ? QLocale().toString(at, QLocale::ShortFormat)
                                             : QLocale().toString(at.date(), QLocale::ShortFormat));
    if (const std::optional<QPointF> where = obs.where())
        top->setText(PlaceColumn, QStringLiteral("%1, %2").arg(where->y(), 0, 'f', 4).arg(where->x(), 0, 'f', 4));
    else
        top->setText(PlaceColumn, QString());
    top->setText(PhotosColumn, QString::number(obs.photos.size()));
    // The first line; the whole note in the tooltip.
    const QString firstLine = obs.note.section(QLatin1Char('\n'), 0, 0);
    const QString marked = inat::annotationsText(obs.annotations);
    top->setText(NoteColumn, (marked.isEmpty() ? QString() : QStringLiteral("[%1] ").arg(marked)) + firstLine
                                 + (obs.note.contains(QLatin1Char('\n')) ? QStringLiteral(" …") : QString()));
    top->setToolTip(NoteColumn, obs.observationId > 0 ? obs.note
                                : obs.note.isEmpty()
                                    ? tr("Double-click to write a note, sent as the observation's description.")
                                    : obs.note.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))
                                          + tr("<br><br><i>Double-click to change it.</i>"));
    top->setText(StatusColumn, statusOf(row));
    const ThemeColors &colors = themeColors(currentThemeVariant());
    top->setForeground(StatusColumn, row.failed                             ? QBrush(colors.danger)
                                     : obs.problems() != 0 || row.held() ? QBrush(colors.statusPending)
                                                                         : QBrush());
    top->setData(0, kSentRole, obs.uploaded());
    if (obs.uploaded()) {
        top->setFlags(top->flags() & ~Qt::ItemIsUserCheckable);
        top->setData(SpeciesColumn, Qt::CheckStateRole, QVariant());
        top->setToolTip(StatusColumn, tr("Already on iNaturalist, so it won't be sent again. Double-click to open "
                                         "it there."));
    } else {
        top->setCheckState(SpeciesColumn, row.use && !obs.problems() ? Qt::Checked : Qt::Unchecked);
        top->setToolTip(StatusColumn, row.hits.isEmpty() ? QString()
                                                         : tr("Double-click to compare it with the one on iNaturalist."));
    }
    QFont font = top->font(SpeciesColumn);
    font.setBold(true);
    top->setFont(SpeciesColumn, font);
}

QList<int> InatUploadPage::selectedRows() const
{
    QList<int> rows;
    for (QTreeWidgetItem *item : m_tree->selectedItems()) {
        QTreeWidgetItem *top = item->parent() ? item->parent() : item;
        const int r = top->data(0, kRowRole).toInt();
        if (!rows.contains(r))
            rows << r;
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

std::pair<int, int> InatUploadPage::selectedPhoto() const
{
    const QList<QTreeWidgetItem *> items = m_tree->selectedItems();
    if (items.size() != 1 || !items.first()->parent())
        return {-1, -1};
    return {items.first()->parent()->data(0, kRowRole).toInt(), items.first()->data(0, kPhotoRole).toInt()};
}

void InatUploadPage::updateButtons()
{
    const bool busy = isBusy();
    const QList<int> rows = selectedRows();
    bool anySent = false, anyNew = false, noDate = false;
    QList<inat::ProposedObservation> chosen;
    for (int r : rows) {
        const inat::ProposedObservation &obs = m_rows.at(r).obs;
        chosen << obs;
        anySent = anySent || obs.observationId > 0;
        anyNew = anyNew || !obs.uploaded();
        noDate = noDate || (obs.problems() & inat::ProposedObservation::NoDate);
    }
    const auto [photoRow, photoIndex] = selectedPhoto();

    m_add->setEnabled(!busy);
    m_regroup->setEnabled(!busy && !m_rows.isEmpty());
    m_gap->setEnabled(!busy);
    m_byDistance->setEnabled(!busy);
    m_distance->setEnabled(!busy && m_byDistance->isChecked());
    m_geoprivacy->setEnabled(!busy);
    // A species can't change once the observation is made: that's an
    // identification, made on iNaturalist.
    m_species->setEnabled(!busy && !rows.isEmpty() && !anySent);
    m_identify->setEnabled(!busy && rows.size() == 1 && !anySent);
    m_location->setEnabled(!busy && !rows.isEmpty() && !anySent && bool(m_setLocation));
    m_date->setEnabled(!busy && rows.size() == 1 && noDate && !anySent);
    m_merge->setEnabled(!busy && inat::ObservationGrouper::canMerge(chosen));
    m_split->setEnabled(!busy && photoRow >= 0
                        && inat::ObservationGrouper::canSplit(m_rows.at(photoRow).obs, photoIndex));
    QList<inat::ProposedObservation> all;
    for (const Row &row : std::as_const(m_rows))
        all << row.obs;
    const bool movable = photoRow >= 0 && inat::ObservationGrouper::canMove(all, photoRow, photoIndex, photoRow);
    const int sent = photoRow >= 0 ? int(std::count_if(all.at(photoRow).photos.cbegin(), all.at(photoRow).photos.cend(),
                                                       [](const inat::UploadPhoto &p) { return p.done(); }))
                                   : 0;
    m_up->setEnabled(!busy && movable && photoIndex > sent);
    m_down->setEnabled(!busy && movable && photoIndex + 1 < all.at(photoRow).photos.size());
    m_remove->setEnabled(!busy && !m_tree->selectedItems().isEmpty());
    m_review->setEnabled(!busy && rows.size() == 1 && !m_rows.at(rows.first()).hits.isEmpty()
                         && m_rows.at(rows.first()).obs.observationId <= 0);
    m_checkButton->setEnabled(!busy && std::any_of(m_rows.cbegin(), m_rows.cend(), [](const Row &row) {
        return row.obs.observationId <= 0 && row.obs.observedAt().isValid();
    }));
    m_open->setEnabled(rows.size() == 1 && m_rows.at(rows.first()).obs.observationId > 0);
    m_stop->setVisible(busy);

    int ready = 0, photos = 0;
    for (const Row &row : std::as_const(m_rows))
        if (row.use && !row.obs.problems() && !row.obs.uploaded()) {
            ++ready;
            photos += row.obs.remaining();
        }
    m_upload->setText(ready > 0 ? plural(ready, tr("Upload 1 Observation"), tr("Upload %1 Observations"))
                                : tr("Upload"));
    // The default button's text is bold (theme.qss), wider than its size hint allows for.
    QFont bold = m_upload->font();
    bold.setWeight(QFont::DemiBold);
    m_upload->setMinimumWidth(QFontMetrics(bold).horizontalAdvance(m_upload->text()) + 40);
    m_upload->setEnabled(!busy && ready > 0);
    m_upload->setToolTip(ready > 0 ? plural(photos, tr("1 photo to send."), tr("%1 photos to send."))
                                   : tr("Tick the observations to upload; each needs a species, a location and a date."));
}

void InatUploadPage::addCaptures(const QList<int> &captureIds)
{
    if (isBusy()) {
        m_message->setText(tr("Wait for the upload to finish before adding photos."));
        return;
    }
    QSet<int> present;
    for (const Row &row : std::as_const(m_rows))
        for (const inat::UploadPhoto &p : row.obs.photos)
            present.insert(p.captureId);
    QList<int> fresh;
    for (int id : captureIds)
        if (!present.contains(id))
            fresh << id;
    const QList<inat::UploadPhoto> photos = inat::ObservationUploader::loadPhotos(m_app.database().connectionName(), fresh);
    const QList<inat::ProposedObservation> groups = inat::ObservationGrouper::group(photos, m_gap->value() * 60, maxKm());
    const int copies = int(std::count_if(photos.cbegin(), photos.cend(),
                                         [](const inat::UploadPhoto &p) { return !p.copyOf.isEmpty(); }));
    int already = 0;
    for (const inat::ProposedObservation &g : groups) {
        Row row;
        row.obs = g;
        row.use = !g.uploaded();
        already += g.uploaded() ? 1 : 0;
        m_rows << row;
    }
    rebuild();
    if (!photos.isEmpty())
        m_message->setText(plural(int(photos.size()), tr("Added 1 photo."), tr("Added %1 photos."))
                           + (already > 0 ? QLatin1Char(' ')
                                                + plural(already, tr("1 of its observations is already on iNaturalist."),
                                                         tr("%1 of their observations are already on iNaturalist."))
                                          : QString())
                           + (copies > 0 ? QLatin1Char(' ')
                                               + plural(copies, tr("1 is a copy of a photo that's on iNaturalist already."),
                                                        tr("%1 are copies of photos that are on iNaturalist already."))
                                         : QString()));
    else if (!captureIds.isEmpty())
        m_message->setText(tr("Those photos are here already."));
}

void InatUploadPage::addPhotos()
{
    UploadPhotoPicker picker(m_app, this);
    picker.setFolderAdder(m_addFolder);
    if (picker.exec() == QDialog::Accepted)
        addCaptures(picker.selectedCaptureIds());
}

void InatUploadPage::setSpecies(int r, const taxonomy::Taxon &taxon, bool fromVision)
{
    changed(r);
    Row &row = m_rows[r];
    row.obs.taxon = taxon;
    row.obs.taxonFromVision = fromVision;
    // Kept on the photos too, so a regroup keeps it.
    for (inat::UploadPhoto &p : row.obs.photos) {
        p.taxon = taxon;
        if (fromVision)
            m_fromVision.insert(p.captureId);
        else
            m_fromVision.remove(p.captureId);
    }
    row.status.clear();
    row.failed = false;
    row.use = true;
}

void InatUploadPage::chooseSpecies()
{
    const QList<int> rows = selectedRows();
    if (rows.isEmpty())
        return;
    TaxonPickerDialog picker(m_app.database().connectionName(), this);
    if (picker.exec() != QDialog::Accepted || picker.chosenTaxonInatId() <= 0)
        return;
    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    q.prepare(QStringLiteral("SELECT name, common_name, rank FROM taxon WHERE inat_id = ?"));
    q.addBindValue(picker.chosenTaxonInatId());
    if (!q.exec() || !q.next())
        return;
    taxonomy::Taxon taxon;
    taxon.inatId = picker.chosenTaxonInatId();
    taxon.name = q.value(0).toString();
    taxon.commonName = q.value(1).toString();
    taxon.rank = q.value(2).toString();
    for (int r : rows)
        setSpecies(r, taxon, false);
    rebuild();
}

void InatUploadPage::identify()
{
    const QList<int> rows = selectedRows();
    if (rows.size() != 1)
        return;
    const int r = rows.first();
    const auto [photoRow, photoIndex] = selectedPhoto();
    const inat::ProposedObservation &obs = m_rows.at(r).obs;
    const inat::UploadPhoto photo = obs.photos.value(photoRow == r ? photoIndex : 0);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Identify with iNaturalist"));
    auto *panel = new IdentifyPanel(m_app, &dialog);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(tr("Pick what <b>%1</b> shows:").arg(photo.label.toHtmlEscaped()), &dialog);
    layout->addWidget(label);
    layout->addWidget(panel, 1);
    layout->addWidget(buttons);
    dialog.resize(560, 520);

    std::optional<taxonomy::Taxon> picked;
    connect(panel, &IdentifyPanel::chosen, &dialog, [&](const taxonomy::Taxon &taxon) {
        picked = taxon;
        dialog.accept();
    });
    inat::IdentifyRequest request;
    request.key = QString::number(photo.captureId);
    request.path = photo.path;
    if (photo.latitude && photo.longitude)
        request.lonLat = QPointF(*photo.longitude, *photo.latitude);
    else
        request.lonLat = obs.where();
    if (obs.observedAt().isValid())
        request.date = obs.observedAt().date();
    panel->identify(request);
    if (dialog.exec() != QDialog::Accepted || !picked)
        return;
    setSpecies(r, *picked, true);
    rebuild();
}

void InatUploadPage::setLocation()
{
    const QList<int> rows = selectedRows();
    if (rows.isEmpty() || !m_setLocation)
        return;
    QList<int> ids;
    for (int r : rows)
        for (const inat::UploadPhoto &p : m_rows.at(r).obs.photos)
            ids << p.captureId;
    if (m_setLocation(ids))
        reloadLocations(ids);   // where the photos are now
}

void InatUploadPage::setDate()
{
    const QList<int> rows = selectedRows();
    if (rows.size() != 1)
        return;
    Row &row = m_rows[rows.first()];
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Set Date"));
    auto *edit = new QDateTimeEdit(row.obs.when.value_or(QDateTime(QDate::currentDate(), QTime(12, 0))), &dialog);
    edit->setCalendarPopup(true);
    edit->setDisplayFormat(QStringLiteral("d MMM yyyy  HH:mm"));
    edit->setMaximumDate(QDate::currentDate());
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("None of these photos has a date. When was it seen?"), &dialog));
    layout->addWidget(edit);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    row.obs.when = edit->dateTime();
    changed(rows.first());
    rebuild();
}

void InatUploadPage::merge()
{
    const QList<int> rows = selectedRows();
    QList<inat::ProposedObservation> groups;
    for (int r : rows)
        groups << m_rows.at(r).obs;
    if (!inat::ObservationGrouper::canMerge(groups))
        return;
    const QList<taxonomy::Taxon> species = inat::ObservationGrouper::speciesOf(groups);
    taxonomy::Taxon taxon = species.first();
    // Already on iNaturalist: it keeps the species it has there.
    for (const inat::ProposedObservation &g : std::as_const(groups))
        if (g.observationId > 0)
            taxon = g.taxon;
    const bool made = std::any_of(groups.cbegin(), groups.cend(),
                                  [](const inat::ProposedObservation &g) { return g.observationId > 0; });
    if (species.size() > 1 && !made) {
        QStringList names;
        for (const taxonomy::Taxon &t : species)
            names << speciesText(t);
        bool ok = false;
        const QString name = QInputDialog::getItem(this, tr("Merge"),
                                                   tr("These photos have different species. The observation is of:"),
                                                   names, 0, false, &ok);
        if (!ok)
            return;
        taxon = species.value(int(names.indexOf(name)));
    }
    Row merged;
    merged.obs = inat::ObservationGrouper::merge(groups, taxon);
    for (inat::UploadPhoto &p : merged.obs.photos)
        if (p.observationId <= 0)
            p.taxon = taxon;
    for (auto it = rows.crbegin(); it != rows.crend(); ++it)
        m_rows.removeAt(*it);
    m_rows.insert(rows.first(), merged);
    rebuild();
}

void InatUploadPage::split()
{
    const auto [r, at] = selectedPhoto();
    if (r < 0 || !inat::ObservationGrouper::canSplit(m_rows.at(r).obs, at))
        return;
    const QList<inat::ProposedObservation> parts = inat::ObservationGrouper::splitAt(m_rows.at(r).obs, at);
    m_rows[r].obs = parts.at(0);
    Row second;
    second.obs = parts.at(1);
    second.use = m_rows.at(r).use;
    m_rows.insert(r + 1, second);
    rebuild();
}

void InatUploadPage::removeSelected()
{
    QSet<int> wholeRows;
    QHash<int, QSet<int>> photos;   // row -> photo indexes
    for (QTreeWidgetItem *item : m_tree->selectedItems()) {
        if (item->parent())
            photos[item->parent()->data(0, kRowRole).toInt()].insert(item->data(0, kPhotoRole).toInt());
        else
            wholeRows.insert(item->data(0, kRowRole).toInt());
    }
    for (int r = int(m_rows.size()) - 1; r >= 0; --r) {
        if (wholeRows.contains(r)) {
            m_rows.removeAt(r);
            continue;
        }
        const QSet<int> drop = photos.value(r);
        if (drop.isEmpty())
            continue;
        QList<inat::UploadPhoto> kept;
        for (int i = 0; i < m_rows.at(r).obs.photos.size(); ++i)
            if (!drop.contains(i))
                kept << m_rows.at(r).obs.photos.at(i);
        if (kept.isEmpty())
            m_rows.removeAt(r);
        else if (kept.size() != m_rows.at(r).obs.photos.size()) {
            m_rows[r].obs.photos = kept;
            changed(r);
        }
    }
    rebuild();
}

void InatUploadPage::regroup()
{
    QList<inat::UploadPhoto> photos;
    QHash<qint64, QString> uuids;   // observation id -> its uuid, kept
    for (const Row &row : std::as_const(m_rows)) {
        photos << row.obs.photos;
        if (row.obs.observationId > 0)
            uuids.insert(row.obs.observationId, row.obs.uuid);
    }
    m_rows.clear();
    for (const inat::ProposedObservation &g : inat::ObservationGrouper::group(photos, m_gap->value() * 60, maxKm())) {
        Row row;
        row.obs = g;
        row.obs.uuid = uuids.value(g.observationId);
        row.obs.taxonFromVision = std::any_of(g.photos.cbegin(), g.photos.cend(),
                                              [this](const inat::UploadPhoto &p) { return m_fromVision.contains(p.captureId); });
        row.use = !g.uploaded();
        m_rows << row;
    }
    rebuild();
}

void InatUploadPage::openOnInat()
{
    const QList<int> rows = selectedRows();
    if (rows.size() == 1 && m_rows.at(rows.first()).obs.observationId > 0)
        QDesktopServices::openUrl(observationUrl(m_rows.at(rows.first()).obs.observationId));
}

double InatUploadPage::maxKm() const
{
    return m_byDistance->isChecked() ? m_distance->value() / 1000.0 : 0;
}

void InatUploadPage::editNote(int r)
{
    if (r < 0 || r >= m_rows.size() || isBusy() || m_rows.at(r).obs.observationId > 0)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Note"));
    auto *label = new QLabel(tr("Note for <b>%1</b>, sent as the observation's description:")
                                 .arg(speciesText(m_rows.at(r).obs.taxon).toHtmlEscaped()),
                             &dialog);
    label->setWordWrap(true);
    auto *edit = new QPlainTextEdit(m_rows.at(r).obs.note, &dialog);
    edit->setTabChangesFocus(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label);
    layout->addWidget(edit, 1);
    layout->addWidget(buttons);
    dialog.resize(480, 260);
    edit->setFocus();
    edit->moveCursor(QTextCursor::End);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_rows[r].obs.note = edit->toPlainText().trimmed();
    refreshRow(r);
}

void InatUploadPage::viewPhoto(int r, int index)
{
    if (r < 0 || r >= m_rows.size())
        return;
    const inat::ProposedObservation &obs = m_rows.at(r).obs;
    QVector<ImageViewer::Item> items;
    int start = 0;
    for (int i = 0; i < obs.photos.size(); ++i) {
        const inat::UploadPhoto &p = obs.photos.at(i);
        if (p.path.isEmpty())
            continue;
        ImageViewer::Item item;
        item.path = p.path;
        item.title = p.label;
        QStringList caption{p.label};
        if (p.taken.isValid())
            caption << (p.hasTime ? QLocale().toString(p.taken, QLocale::ShortFormat)
                                  : QLocale().toString(p.taken.date(), QLocale::ShortFormat));
        caption << speciesText(p.taxon);
        item.caption = caption.join(QStringLiteral(" · "));
        item.captureId = p.captureId;
        item.hasGps = p.latitude && p.longitude;
        item.latitude = p.latitude.value_or(0);
        item.longitude = p.longitude.value_or(0);
        if (p.taken.isValid())
            item.taken = p.taken.date();
        if (i == index)
            start = int(items.size());
        items << item;
    }
    if (items.isEmpty())
        return;
    if (!m_viewer)
        m_viewer = new ImageViewer(this);
    m_viewer->setItems(items, start);
    m_viewer->showFullScreen();
    m_viewer->raise();
    m_viewer->activateWindow();
}

// The capture ids of the selected photos: those of whole observations too.
QList<int> InatUploadPage::selectedCaptures() const
{
    QList<int> ids;
    for (QTreeWidgetItem *item : m_tree->selectedItems()) {
        if (item->parent()) {
            const int id = item->data(0, kPhotoRole + 1).toInt();
            if (!ids.contains(id))
                ids << id;
            continue;
        }
        const int r = item->data(0, kRowRole).toInt();
        for (const inat::UploadPhoto &p : m_rows.value(r).obs.photos)
            if (!ids.contains(p.captureId))
                ids << p.captureId;
    }
    return ids;
}

void InatUploadPage::reloadLocations(const QList<int> &captureIds)
{
    const QList<inat::UploadPhoto> now = inat::ObservationUploader::loadPhotos(m_app.database().connectionName(),
                                                                               captureIds);
    for (int r = 0; r < m_rows.size(); ++r) {
        bool touched = false;
        for (inat::UploadPhoto &p : m_rows[r].obs.photos)
            for (const inat::UploadPhoto &n : now)
                if (n.captureId == p.captureId) {
                    touched = touched || p.latitude != n.latitude || p.longitude != n.longitude;
                    p.latitude = n.latitude;
                    p.longitude = n.longitude;
                }
        if (touched)
            changed(r);
    }
    rebuild();
}

void InatUploadPage::contextMenuAt(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item)
        return;
    if (!item->isSelected()) {
        m_tree->clearSelection();
        item->setSelected(true);
        m_tree->setCurrentItem(item);
    }
    const bool busy = isBusy();
    const int r = item->parent() ? item->parent()->data(0, kRowRole).toInt() : item->data(0, kRowRole).toInt();
    if (r < 0 || r >= m_rows.size())
        return;
    const inat::ProposedObservation &obs = m_rows.at(r).obs;
    const int index = item->parent() ? item->data(0, kPhotoRole).toInt() : 0;

    // Where the photo is (or the observation's first with a location).
    std::optional<QPointF> where;
    if (item->parent()) {
        const inat::UploadPhoto &p = obs.photos.at(index);
        if (p.latitude && p.longitude)
            where = QPointF(*p.longitude, *p.latitude);
    } else {
        where = obs.where();
    }
    const QList<int> ids = selectedCaptures();
    const std::optional<QPointF> clip = geo::parseLatLon(QApplication::clipboard()->text());

    QMenu menu(this);
    QAction *view = menu.addAction(tr("View Photo"));
    view->setToolTip(tr("Full screen. Esc closes it."));
    connect(view, &QAction::triggered, this, [this, r, index] { viewPhoto(r, index); });
    menu.addSeparator();
    QAction *copy = menu.addAction(tr("Copy Location"));
    copy->setEnabled(where.has_value());
    connect(copy, &QAction::triggered, this, [this, where] {
        const QString text = geo::formatLatLon(where->y(), where->x());
        QApplication::clipboard()->setText(text);
        m_message->setText(tr("Copied the location %1: right-click photos → Paste Location.").arg(text));
    });
    QAction *paste = menu.addAction(clip ? tr("Paste Location (%1)").arg(geo::formatLatLon(clip->y(), clip->x()))
                                         : tr("Paste Location"));
    paste->setToolTip(tr("Give the selected photos the copied location: saved to the photos, as Set Location on "
                         "Map does."));
    paste->setEnabled(!busy && clip && bool(m_pasteLocation) && !ids.isEmpty() && obs.observationId <= 0);
    connect(paste, &QAction::triggered, this, [this, ids] {
        if (m_pasteLocation && m_pasteLocation(ids))
            reloadLocations(ids);
    });
    QAction *locate = menu.addAction(tr("Set Location on Map…"));
    locate->setEnabled(m_location->isEnabled());
    connect(locate, &QAction::triggered, this, &InatUploadPage::setLocation);
    menu.addSeparator();
    QAction *note = menu.addAction(tr("Note…"));
    note->setEnabled(!busy && obs.observationId <= 0);
    connect(note, &QAction::triggered, this, [this, r] { editNote(r); });
    const QString marked = inat::annotationsText(obs.annotations);
    QAction *annotate = menu.addAction(marked.isEmpty() ? tr("Annotations…") : tr("Annotations (%1)…").arg(marked));
    annotate->setToolTip(tr("Sex, life stage, alive or dead, flowers and fruits: added to the observation once it's made."));
    annotate->setEnabled(!busy && obs.observationId <= 0);
    connect(annotate, &QAction::triggered, this, [this, r] {
        if (r < 0 || r >= m_rows.size())
            return;
        QHash<QString, QString> values = m_rows.at(r).obs.annotations;
        if (!editAnnotations(this, tr("Annotations"),
                             tr("Annotations for <b>%1</b>, added on iNaturalist once the observation is made:")
                                 .arg(speciesText(m_rows.at(r).obs.taxon).toHtmlEscaped()),
                             values))
            return;
        m_rows[r].obs.annotations = values;
        refreshRow(r);
    });
    QAction *open = menu.addAction(tr("Open on iNaturalist"));
    open->setEnabled(obs.observationId > 0);
    connect(open, &QAction::triggered, this, &InatUploadPage::openOnInat);
    bool anyDone = false;
    for (int id : ids)
        for (const Row &row : std::as_const(m_rows))
            for (const inat::UploadPhoto &p : row.obs.photos)
                anyDone = anyDone || (p.captureId == id && p.done());
    QAction *reset = menu.addAction(tr("Not on iNaturalist Any More…"));
    reset->setToolTip(tr("Deleted on iNaturalist? PhotoLife asks iNaturalist what's still there, and forgets what "
                         "isn't, so Upload sends it again."));
    reset->setEnabled(!busy && anyDone);
    connect(reset, &QAction::triggered, this, &InatUploadPage::notOnInatAnyMore);
    menu.setToolTipsVisible(true);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void InatUploadPage::notOnInatAnyMore()
{
    if (isBusy() || (m_canEdit && !m_canEdit()))
        return;
    QList<inat::UploadPhoto> photos;
    QList<qint64> observations;
    const QList<int> ids = selectedCaptures();
    for (const Row &row : std::as_const(m_rows))
        for (const inat::UploadPhoto &p : row.obs.photos)
            if (ids.contains(p.captureId) && p.done() && p.observationId > 0) {
                photos << p;
                if (!observations.contains(p.observationId))
                    observations << p.observationId;
            }
    if (photos.isEmpty())
        return;

    const QString title = tr("Not on iNaturalist Any More");
    m_message->setText(tr("Asking iNaturalist which of these are still there…"));
    m_app.inat().setAccessToken(m_app.settings().inatApiToken());
    QPointer<InatUploadPage> self(this);
    m_app.inat().fetchObservationsById(observations, [self, photos, title](
                                                         net::Outcome<net::ObservationPage> out) {
        if (!self)
            return;
        QList<int> captures;
        for (const inat::UploadPhoto &p : photos)
            captures << p.captureId;
        if (!out.ok()) {
            self->m_message->setText(tr("Couldn't reach iNaturalist: %1").arg(out.error));
            if (QMessageBox::warning(self.data(), title,
                                     tr("Couldn't ask iNaturalist what's still there (%1).\n\nMark these %2 as not "
                                        "on iNaturalist anyway? Upload then sends them as a new observation; any "
                                        "still there would be on iNaturalist twice.")
                                         .arg(out.error, plural(int(photos.size()), tr("1 photo"), tr("%1 photos"))),
                                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                == QMessageBox::Yes)
                self->forgetAndReload({}, {}, captures, captures);
            return;
        }
        QHash<qint64, QSet<qint64>> onInat;
        for (const taxonomy::Observation &o : out.value.results) {
            QSet<qint64> photoIds;
            for (const taxonomy::ObservationPhoto &p : o.photos)
                photoIds.insert(p.id);
            onInat.insert(o.id, photoIds);
        }
        const inat::ObservationUploader::ResetPlan plan = inat::ObservationUploader::planReset(photos, onInat);
        QList<int> photosGone = plan.photosGone;
        if (!plan.stillThere.isEmpty()) {
            const bool others = !plan.goneObservations.isEmpty() || !plan.photosGone.isEmpty();
            QMessageBox box(QMessageBox::Question, title,
                            plural(int(plan.stillThere.size()),
                                   tr("1 of these photos is still on iNaturalist, in its observation."),
                                   tr("%1 of these photos are still on iNaturalist, in their observations."))
                                + tr("\n\nSend them again anyway? They'd be added to that observation a second "
                                     "time."),
                            QMessageBox::Cancel, self);
            QPushButton *keep = others ? box.addButton(tr("Only the Others"), QMessageBox::AcceptRole) : nullptr;
            QPushButton *all = box.addButton(others ? tr("All of Them") : tr("Send Again Anyway"),
                                             QMessageBox::DestructiveRole);
            box.setDefaultButton(keep ? keep : static_cast<QPushButton *>(box.button(QMessageBox::Cancel)));
            box.exec();
            if (box.clickedButton() == all)
                photosGone += plan.stillThere;
            else if (!keep || box.clickedButton() != keep) {
                self->m_message->clear();
                return;
            }
        }
        if (plan.goneObservations.isEmpty() && photosGone.isEmpty()) {
            self->m_message->setText(plan.copies.isEmpty()
                                         ? tr("Those are still on iNaturalist: nothing changed.")
                                         : tr("Those are copies of photos still on iNaturalist: nothing changed."));
            return;
        }
        self->forgetAndReload(plan.goneObservations, photosGone, {}, captures);
        QStringList said;
        if (!plan.goneObservations.isEmpty())
            said << plural(int(plan.goneObservations.size()),
                           tr("1 observation isn't on iNaturalist any more: its photos go as a new one."),
                           tr("%1 observations aren't on iNaturalist any more: their photos go as new ones."));
        if (!photosGone.isEmpty())
            said << plural(int(photosGone.size()), tr("1 photo goes back into its observation."),
                           tr("%1 photos go back into their observations."));
        if (!plan.copies.isEmpty())
            said << plural(int(plan.copies.size()), tr("1 is a copy of a photo still there, so it stays."),
                           tr("%1 are copies of photos still there, so they stay."));
        self->m_message->setText(said.join(QLatin1Char(' ')) + tr(" Press Upload to send them."));
    });
}

void InatUploadPage::forgetAndReload(const QList<qint64> &observationIds, const QList<int> &photoCaptureIds,
                                     const QList<int> &wholeCaptureIds, const QList<int> &captureIds)
{
    if (!inat::ObservationUploader::forget(m_app.database().connectionName(), observationIds, photoCaptureIds,
                                           wholeCaptureIds)) {
        m_message->setText(tr("Couldn't update the catalogue: nothing changed."));
        return;
    }
    // The observations holding them, grouped again from what's recorded now.
    QList<int> again;
    for (int r = int(m_rows.size()) - 1; r >= 0; --r) {
        bool holds = false;
        for (const inat::UploadPhoto &p : m_rows.at(r).obs.photos)
            holds = holds || captureIds.contains(p.captureId)
                    || observationIds.contains(p.observationId);
        if (!holds)
            continue;
        for (const inat::UploadPhoto &p : m_rows.at(r).obs.photos)
            again << p.captureId;
        m_rows.removeAt(r);
    }
    addCaptures(again);
}

void InatUploadPage::movePhoto(int fromRow, int from, int toRow, int to)
{
    if (isBusy() || fromRow < 0 || fromRow >= m_rows.size() || toRow < 0 || toRow >= m_rows.size())
        return;
    QList<inat::ProposedObservation> groups;
    for (const Row &row : std::as_const(m_rows))
        groups << row.obs;
    if (from < 0 || from >= groups.at(fromRow).photos.size())
        return;
    const int captureId = groups.at(fromRow).photos.at(from).captureId;
    if (!inat::ObservationGrouper::movePhoto(groups, fromRow, from, toRow, to))
        return;
    if (fromRow != toRow) {
        changed(fromRow);
        changed(toRow);
        if (m_rows.at(toRow).obs.taxonFromVision)
            m_fromVision.insert(captureId);
        else
            m_fromVision.remove(captureId);
    }
    if (groups.size() < m_rows.size())
        m_rows.removeAt(fromRow);   // its last photo went
    for (int r = 0; r < m_rows.size(); ++r)
        m_rows[r].obs = groups.at(r);

    rebuild();
    // The photo stays selected, wherever it went.
    m_tree->clearSelection();
    for (int r = 0; r < m_tree->topLevelItemCount(); ++r)
        for (int i = 0; i < m_tree->topLevelItem(r)->childCount(); ++i)
            if (QTreeWidgetItem *child = m_tree->topLevelItem(r)->child(i);
                child->data(0, kPhotoRole + 1).toInt() == captureId) {
                child->setSelected(true);
                m_tree->setCurrentItem(child);
                m_tree->scrollToItem(child);
            }
}

void InatUploadPage::moveSelected(int by)
{
    const auto [r, at] = selectedPhoto();
    if (r >= 0 && at + by >= 0)
        movePhoto(r, at, r, at + by);
}

void InatUploadPage::reviewMatch()
{
    const QList<int> rows = selectedRows();
    if (rows.size() != 1 || isBusy())
        return;
    const int r = rows.first();
    if (m_rows.at(r).hits.isEmpty() || m_rows.at(r).obs.observationId > 0)
        return;
    UploadDuplicateDialog dialog(m_app, m_rows.at(r).obs.photos, m_rows.at(r).hits, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    Row &row = m_rows[r];
    const qint64 id = dialog.observationId();
    switch (dialog.choice()) {
    case UploadDuplicateDialog::Link: {
        if (id <= 0 || (m_canEdit && !m_canEdit()))
            return;
        QList<int> ids;
        for (const inat::UploadPhoto &p : std::as_const(row.obs.photos))
            if (!p.done())
                ids << p.captureId;
        if (!inat::ObservationUploader::link(m_app.database().connectionName(), ids, id)) {
            m_message->setText(tr("Couldn't record that in the catalogue."));
            return;
        }
        for (inat::UploadPhoto &p : row.obs.photos)
            if (!p.done()) {
                p.observationId = id;
                p.linked = true;
            }
        row.obs.observationId = id;
        row.use = false;
        row.hits.clear();
        m_message->setText(tr("Those photos are now recorded as of observation #%1; nothing was sent.").arg(id));
        emit uploaded();
        break;
    }
    case UploadDuplicateDialog::AddTo:
        if (id <= 0)
            return;
        // Sent to that observation; kept with it if regrouped.
        row.obs.observationId = id;
        for (inat::UploadPhoto &p : row.obs.photos)
            if (!p.done())
                p.observationId = id;
        row.obs.checked = true;
        row.hits.clear();
        row.use = true;
        break;
    case UploadDuplicateDialog::UploadNew:
        row.asNew = true;
        row.use = true;
        break;
    }
    rebuild();
}

void InatUploadPage::upload()
{
    bool any = false;
    for (const Row &row : std::as_const(m_rows))
        any = any || (row.use && !row.obs.problems() && !row.obs.uploaded());
    if (!any)
        return;
    if (m_canEdit && !m_canEdit())
        return;

    m_account.commit();
    const QString token = m_app.settings().inatApiToken();
    const std::optional<QDateTime> expires = inat::apiTokenExpiry(token);
    const bool soon = expires && QDateTime::currentDateTimeUtc().secsTo(expires->toUTC()) < 30 * 60;
    if (!InatTokenGetter::isUsable(token) || soon) {
        QMessageBox box(QMessageBox::Information, tr("Upload to iNaturalist"),
                        !InatTokenGetter::isUsable(token)
                            ? tr("Uploading needs your iNaturalist token. Get Token opens iNaturalist's token page: "
                                 "sign in there if asked, then press Ctrl+A and Ctrl+C.")
                            : tr("Your token runs out in a few minutes. Get a fresh one first?"),
                        QMessageBox::Cancel, this);
        QPushButton *get = box.addButton(tr("Get Token…"), QMessageBox::AcceptRole);
        QPushButton *anyway = InatTokenGetter::isUsable(token)
                                  ? box.addButton(tr("Upload Anyway"), QMessageBox::DestructiveRole)
                                  : nullptr;
        box.setDefaultButton(get);
        box.exec();
        if (box.clickedButton() == get)
            InatTokenGetter::start(m_app);
        if (!anyway || box.clickedButton() != anyway)
            return;
    }
    m_app.inat().setAccessToken(token);
    checkInat(true);
}

void InatUploadPage::checkNow()
{
    m_account.commit();
    m_app.inat().setAccessToken(m_app.settings().inatApiToken());
    checkInat(false);
}

void InatUploadPage::checkInat(bool thenUpload)
{
    // New observations, not yet looked for (on Upload) or any (asked for);
    // not those the user has said are different.
    QList<inat::InatDuplicateCheck::Group> groups;
    m_checking.clear();
    for (int r = 0; r < m_rows.size(); ++r) {
        const Row &row = m_rows.at(r);
        if (row.obs.observationId > 0 || row.asNew || !row.obs.observedAt().isValid())
            continue;
        if (thenUpload && (!row.use || row.obs.problems() || row.obs.checked))
            continue;
        groups.append({r, row.obs});
        m_checking << r;
    }
    if (groups.isEmpty()) {
        if (thenUpload)
            sendReady();
        else
            m_message->setText(knownOnInat().isEmpty() ? tr("Nothing to look for: each needs a date.")
                                                       : knownOnInat() + tr(" There's nothing else to look for."));
        return;
    }
    m_uploadAfterCheck = thenUpload;
    m_checkStopped = false;
    const QString login = !m_login.isEmpty() ? m_login : m_account.username();
    m_message->setText(tr("Looking for these among your iNaturalist observations…"));
    m_stop->setEnabled(true);
    m_check->start(login, groups);
    updateButtons();
}

void InatUploadPage::checkFinished(bool ok, const QString &error, int status,
                                   const QHash<int, QList<inat::DuplicateHit>> &hits)
{
    int held = 0;
    for (int r : std::as_const(m_checking)) {
        if (r >= m_rows.size())
            continue;
        Row &row = m_rows[r];
        row.hits = hits.value(r);
        row.obs.checked = ok;
        if (row.held()) {
            row.use = false;
            ++held;
        }
    }
    m_checking.clear();
    rebuild();

    const QString title = tr("Upload to iNaturalist");
    int ready = 0;
    for (const Row &row : std::as_const(m_rows))
        ready += row.use && !row.obs.problems() && !row.obs.uploaded() ? 1 : 0;
    const QString heldText = plural(held, tr("1 observation looks like one already on iNaturalist"),
                                    tr("%1 observations look like ones already on iNaturalist"));
    if (!ok && m_checkStopped) {
        m_message->setText(tr("Stopped looking; nothing was sent."));
        return;
    }
    if (!ok) {
        const QString why = status == 401 ? tr("your token has expired. Get Token, then try again.") : error;
        m_message->setText(tr("Couldn't look on iNaturalist: %1").arg(why)
                           + (held > 0 ? QLatin1Char(' ') + heldText + tr(" in your library.") : QString()));
        if (!m_uploadAfterCheck || ready == 0)
            return;
        if (QMessageBox::warning(this, title,
                                 tr("Couldn't look for these among your iNaturalist observations, so some may be "
                                    "there already: %1\n\nUpload without checking?")
                                     .arg(why),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes)
            sendReady();
        return;
    }
    const QString known = knownOnInat();
    if (held == 0) {
        m_message->setText(known.isEmpty()
                               ? tr("None of these is on iNaturalist already, as far as PhotoLife can tell.")
                               : known + tr(" Of the others, none looks like one already there."));
        if (m_uploadAfterCheck)
            sendReady();
        return;
    }
    m_message->setText((known.isEmpty() ? QString() : known + QLatin1Char(' '))
                       + heldText + tr(": they're held. Double-click their Status to compare and decide."));
    if (!m_uploadAfterCheck)
        return;
    if (ready == 0) {
        QMessageBox::information(this, title,
                                 heldText + tr(", so it's held for you to review: double-click its Status to "
                                               "compare them and decide."));
        return;
    }
    if (QMessageBox::question(this, title,
                              heldText + tr(" and %1 held for you to review.\n\nUpload the other %2 now?")
                                             .arg(held == 1 ? tr("is") : tr("are"))
                                             .arg(ready),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
        == QMessageBox::Yes)
        sendReady();
}

QString InatUploadPage::knownOnInat() const
{
    int whole = 0, part = 0;
    for (const Row &row : std::as_const(m_rows)) {
        if (row.obs.uploaded())
            ++whole;
        else if (row.obs.observationId > 0)
            ++part;
    }
    QStringList said;
    if (whole > 0)
        said << plural(whole, tr("1 observation is already on iNaturalist"),
                       tr("%1 observations are already on iNaturalist"));
    if (part > 0)
        said << plural(part, tr("1 is there with photos still to add"), tr("%1 are there with photos still to add"));
    return said.isEmpty() ? QString()
                          : said.join(tr(", and ")) + tr(" (in green): they won't be sent again.");
}

void InatUploadPage::sendReady()
{
    QList<inat::ObservationUploader::Job> jobs;
    int photos = 0;
    for (int r = 0; r < m_rows.size(); ++r) {
        const Row &row = m_rows.at(r);
        if (!row.use || row.obs.problems() || row.obs.uploaded() || row.held())
            continue;
        inat::ObservationUploader::Job job;
        job.key = r;
        job.observation = row.obs;
        if (job.observation.uuid.isEmpty()) {
            job.observation.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_rows[r].obs.uuid = job.observation.uuid;
        }
        job.geoprivacy = m_geoprivacy->currentData().toString();
        jobs << job;
        photos += row.obs.remaining();
    }
    if (jobs.isEmpty())
        return;

    const QString privacy = m_geoprivacy->currentText().toLower();
    if (QMessageBox::question(this, tr("Upload to iNaturalist"),
                              tr("Send %1 to your iNaturalist account, with %2?\n\n"
                                 "They're public as soon as they arrive, with geoprivacy “%3”.")
                                  .arg(plural(int(jobs.size()), tr("1 observation"), tr("%1 observations")),
                                       plural(photos, tr("1 photo"), tr("%1 photos")), privacy),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes)
        != QMessageBox::Yes)
        return;

    m_sentThisRun.clear();
    m_progress->setRange(0, photos);
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_stop->setEnabled(true);
    m_message->setText(tr("Uploading…"));
    m_uploader->start(jobs);
    updateButtons();
}

void InatUploadPage::uploadFinished(bool tokenExpired)
{
    m_progress->setVisible(false);
    int failed = 0;
    for (const Row &row : std::as_const(m_rows))
        failed += row.failed ? 1 : 0;
    const int done = int(m_sentThisRun.size());
    const qint64 last = m_sentThisRun.isEmpty() ? 0 : m_rows.value(m_sentThisRun.constLast()).obs.observationId;
    QStringList parts;
    if (done > 0)
        parts << plural(done, tr("1 observation sent, and taken off this list."),
                        tr("%1 observations sent, and taken off this list."));
    if (failed > 0)
        parts << plural(failed, tr("1 couldn't be sent"), tr("%1 couldn't be sent"))
                     + tr(" \u2014 see Status. Upload tries again, sending only what's missing.");
    if (tokenExpired)
        parts << tr("Your token expired: Get Token, then Upload again.");
    if (done > 0 && last > 0)
        parts << QStringLiteral("<a href=\"%1\">%2</a>")
                     .arg(observationUrl(last).toString(), tr("Open the last one"));
    m_message->setText(parts.join(QLatin1Char(' ')));

    // What went up whole is done with here (the photos say they're on
    // iNaturalist, so they aren't offered again); what didn't stays, to retry.
    for (int r = int(m_rows.size()) - 1; r >= 0; --r)
        if (m_sentThisRun.contains(r) && m_rows.at(r).obs.uploaded())
            m_rows.removeAt(r);
    m_sentThisRun.clear();
    rebuild();
    if (tokenExpired)
        showWho();
    emit uploaded();
}

} // namespace pl
