#include "ui/OrganiseView.h"

#include "app/Application.h"
#include "db/Database.h"
#include "match/CandidateFinder.h"
#include "organise/OrganiseMover.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"
#include "ui/ImageViewer.h"
#include "ui/Theme.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

namespace pl {

namespace ui_organise {

constexpr int kThumbPx = 512;   // sharp enough for the largest filmstrip frames

enum Role {
    KindsRole = Qt::UserRole + 1,
    TakenRole,
    RunNameRole,
    RunStartRole,
    RunNumberRole,
    RejectedRole,
};

// The session's shots, with their thumbnails.
class OrganiseModel : public QAbstractListModel
{
public:
    OrganiseModel(organise::OrganiseSession &session, thumb::ThumbnailCache &thumbs, QObject *parent)
        : QAbstractListModel(parent), m_session(session), m_thumbs(thumbs)
    {
        QObject::connect(&m_thumbs, &thumb::ThumbnailCache::ready, this,
                         [this](const QString &key, int px) {
                             if (px != kThumbPx)
                                 return;
                             if (const auto it = m_rowByKey.constFind(key); it != m_rowByKey.constEnd())
                                 emit dataChanged(index(*it), index(*it), {Qt::DecorationRole});
                         });
    }

    void reset()
    {
        beginResetModel();
        m_rowByKey.clear();
        for (int i = 0; i < m_session.count(); ++i)
            m_rowByKey.insert(thumb::ThumbnailCache::keyFor({}, m_session.items().at(i).previewPath), i);
        endResetModel();
    }

    // Everything from `row` on may have changed run.
    void runChanged(int row)
    {
        if (m_session.count() > 0)
            emit dataChanged(index(std::max(0, row)), index(m_session.count() - 1));
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : m_session.count();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_session.count())
            return {};
        const int row = index.row();
        const organise::Item &item = m_session.items().at(row);
        switch (role) {
        case Qt::DisplayRole:
            return item.baseName;
        case Qt::DecorationRole:
            return m_thumbs.thumbnail({}, item.previewPath, kThumbPx);
        case Qt::ToolTipRole:
            return QStringList(item.files).replaceInStrings(QRegularExpression(QStringLiteral(".*/")), QString())
                .join(QLatin1Char('\n'));
        case KindsRole:
            return item.kinds.join(QLatin1Char('+'));
        case TakenRole:
            return item.taken;
        case RunNameRole:
            return m_session.runName(row);
        case RunStartRole:
            return !m_session.nameAt(row).isEmpty();
        case RunNumberRole:
            return m_session.runNumber(row);
        case RejectedRole:
            return m_session.isRejected(row);
        default:
            return {};
        }
    }

private:
    organise::OrganiseSession &m_session;
    thumb::ThumbnailCache &m_thumbs;
    QHash<QString, int> m_rowByKey;
};

// A framed photo: its run's band and name on top, the picture, then the file
// name and when it was taken; rejects crossed out.
class OrganiseDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize cell{260, 240};
    static constexpr int kBand = 24;
    static constexpr int kCaption = 38;
    static constexpr int kMargin = 5;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return cell; }

    static QRect frameRect(const QRect &cellRect) { return cellRect.adjusted(kMargin, kMargin, -kMargin, -kMargin); }
    static QRect imageRect(const QRect &cellRect)
    {
        const QRect f = frameRect(cellRect);
        return QRect(f.left() + 6, f.top() + kBand + 4, f.width() - 12, f.height() - kBand - kCaption - 8);
    }
    // The corner button that opens the photo full size (a click elsewhere names it).
    static QRect zoomRect(const QRect &cellRect)
    {
        const QRect img = imageRect(cellRect);
        return QRect(img.right() - 26, img.bottom() - 26, 24, 24);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const ThemeColors &t = themeColors(currentThemeVariant());
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect frame = frameRect(option.rect);
        const bool selected = option.state & QStyle::State_Selected;

        p->setPen(QPen(selected ? t.primaryAccent : t.border, selected ? 3 : 1));
        p->setBrush(t.panel);
        p->drawRoundedRect(frame, t.radius, t.radius);

        // The run: its colour alternates so neighbouring runs stand apart.
        const QString run = index.data(RunNameRole).toString();
        const int runNumber = index.data(RunNumberRole).toInt();
        if (!run.isEmpty()) {
            const QColor colour = runNumber % 2 == 0 ? t.primaryAccent : t.secondaryAccent;
            QRect band(frame.left() + 1, frame.top() + 1, frame.width() - 2, kBand);
            if (index.data(RunStartRole).toBool()) {
                QPainterPath path;
                path.addRoundedRect(band, t.radius, t.radius);
                p->fillPath(path, colour);
                QFont f = option.font;
                f.setBold(true);
                p->setFont(f);
                p->setPen(Qt::white);
                p->drawText(band.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                            QFontMetrics(f).elidedText(run, Qt::ElideRight, band.width() - 16));
            } else {
                band.setHeight(6);
                QColor faint = colour;
                faint.setAlpha(150);
                p->fillRect(band.adjusted(t.radius, 0, -t.radius, 0), faint);
                p->setFont(option.font);
                p->setPen(t.mutedText);
                p->drawText(QRect(frame.left() + 8, frame.top() + 7, frame.width() - 16, kBand - 6),
                            Qt::AlignVCenter | Qt::AlignLeft,
                            QFontMetrics(option.font).elidedText(run, Qt::ElideRight, frame.width() - 16));
            }
        }

        // The picture.
        const QRect img = imageRect(option.rect);
        const QPixmap px = index.data(Qt::DecorationRole).value<QPixmap>();
        if (!px.isNull()) {
            const QSize scaled = px.size().scaled(img.size(), Qt::KeepAspectRatio);
            const QRect target(img.center().x() - scaled.width() / 2, img.center().y() - scaled.height() / 2,
                               scaled.width(), scaled.height());
            p->drawPixmap(target, px);
        } else {
            p->setPen(t.mutedText);
            p->drawText(img, Qt::AlignCenter, QObject::tr("Loading…"));
        }

        const bool rejected = index.data(RejectedRole).toBool();
        if (rejected) {
            p->fillRect(img, QColor(0, 0, 0, 140));
            p->setPen(QPen(t.statusUnmatched, 6, Qt::SolidLine, Qt::RoundCap));
            const int s = std::min(img.width(), img.height()) / 4;
            const QPoint c = img.center();
            p->drawLine(c + QPoint(-s, -s), c + QPoint(s, s));
            p->drawLine(c + QPoint(-s, s), c + QPoint(s, -s));
        }

        // Full-size button.
        const QRect zoom = zoomRect(option.rect);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 120));
        p->drawEllipse(zoom);
        p->setPen(Qt::white);
        p->drawText(zoom, Qt::AlignCenter, QStringLiteral("⤢"));

        // File name and kinds, then the date taken.
        QFont small = option.font;
        p->setFont(small);
        p->setPen(rejected ? t.mutedText : t.text);
        const QRect caption(frame.left() + 8, img.bottom() + 4, frame.width() - 16, kCaption);
        const QString line1 = index.data(Qt::DisplayRole).toString() + QStringLiteral("  ·  ")
                              + index.data(KindsRole).toString()
                              + (rejected ? QObject::tr("  ·  reject") : QString());
        const QDateTime taken = index.data(TakenRole).toDateTime();
        const QString line2 = QLocale().toString(taken, QLocale::ShortFormat);
        const QFontMetrics fm(small);
        p->drawText(caption.left(), caption.top() + fm.ascent(), fm.elidedText(line1, Qt::ElideRight, caption.width()));
        p->setPen(t.mutedText);
        p->drawText(caption.left(), caption.top() + fm.height() + fm.ascent(),
                    fm.elidedText(line2, Qt::ElideRight, caption.width()));
        p->restore();
    }
};

// "What is it?": a name for a photo (and the run it starts), with
// suggestions from the taxonomy as it's typed -- common and scientific names
// -- and the names already given this session. Free text is fine too.
class NamePopup : public QDialog
{
public:
    NamePopup(const QString &connection, const QString &photo, const QString &current,
              const QStringList &recent, QWidget *parent)
        : QDialog(parent), m_finder(connection), m_recent(recent)
    {
        setWindowTitle(tr("Name %1").arg(photo));
        auto *label = new QLabel(tr("What is <b>%1</b> — and the photos after it, up to the next "
                                    "named one?").arg(photo.toHtmlEscaped()),
                                 this);
        label->setWordWrap(true);
        m_edit = new QLineEdit(current, this);
        m_edit->setPlaceholderText(tr("Common or scientific name"));
        m_edit->setClearButtonEnabled(true);
        m_list = new QListWidget(this);
        m_list->setMinimumHeight(180);
        auto *hint = new QLabel(tr("↓ to choose a suggestion · Enter uses what's typed"), this);
        QPalette pal = hint->palette();
        pal.setColor(QPalette::WindowText, themeColors(currentThemeVariant()).mutedText);
        hint->setPalette(pal);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        if (!current.isEmpty()) {
            QPushButton *clear = buttons->addButton(tr("Clear Name"), QDialogButtonBox::DestructiveRole);
            connect(clear, &QPushButton::clicked, this, [this] {
                m_chosen.clear();
                m_cleared = true;
                accept();
            });
        }
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { acceptTyped(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(label);
        layout->addWidget(m_edit);
        layout->addWidget(m_list, 1);
        layout->addWidget(hint);
        layout->addWidget(buttons);
        resize(460, 360);

        connect(m_edit, &QLineEdit::textEdited, this, [this] { refresh(); });
        connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
            m_chosen = item->data(Qt::UserRole).toString();
            accept();
        });
        connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
            m_chosen = item->data(Qt::UserRole).toString();
            accept();
        });
        m_edit->installEventFilter(this);
        m_edit->selectAll();
        refresh();
    }

    QString chosen() const { return m_chosen; }
    bool cleared() const { return m_cleared; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_edit && event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if ((key == Qt::Key_Down || key == Qt::Key_Up) && m_list->count() > 0) {
                m_list->setFocus();
                m_list->setCurrentRow(key == Qt::Key_Down ? 0 : m_list->count() - 1);
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                acceptTyped();
                return true;
            }
        }
        return QDialog::eventFilter(watched, event);
    }

private:
    void acceptTyped()
    {
        if (m_list->hasFocus() && m_list->currentItem()) {
            m_chosen = m_list->currentItem()->data(Qt::UserRole).toString();
            accept();
            return;
        }
        const QString text = m_edit->text().simplified();
        if (text.isEmpty())
            return;
        m_chosen = text;
        accept();
    }

    void add(const QString &label, const QString &value)
    {
        auto *item = new QListWidgetItem(label, m_list);
        item->setData(Qt::UserRole, value);
    }

    void refresh()
    {
        m_list->clear();
        const QString text = m_edit->text().simplified();
        const QString folded = text.toLower();
        for (const QString &r : m_recent) {
            if (text.isEmpty() || r.toLower().contains(folded))
                add(tr("%1   (named earlier)").arg(r), r);
        }
        if (text.size() < 2)
            return;
        QSet<QString> seen(m_recent.cbegin(), m_recent.cend());
        for (const match::TaxonCandidate &c : m_finder.search(text, 30)) {
            // The form that was typed: the common name, or the scientific one.
            const bool common = !c.commonName.isEmpty()
                                && c.commonName.toLower().contains(folded)
                                && !c.name.toLower().contains(folded);
            const QString value = common ? c.commonName : c.name;
            if (seen.contains(value))
                continue;
            seen.insert(value);
            const QString other = common ? c.name : c.commonName;
            add(other.isEmpty() ? QStringLiteral("%1   [%2]").arg(value, c.rank)
                                : QStringLiteral("%1  ·  %2   [%3]").arg(value, other, c.rank),
                value);
        }
    }

    match::CandidateFinder m_finder;
    QStringList m_recent;
    QLineEdit *m_edit = nullptr;
    QListWidget *m_list = nullptr;
    QString m_chosen;
    bool m_cleared = false;
};

} // namespace ui_organise

using namespace ui_organise;

OrganiseView::OrganiseView(Application &app, QWidget *parent) : QWidget(parent), m_app(app)
{
    m_folderLabel = new QLabel(tr("No folder open"), this);
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *open = new QPushButton(tr("Open Download Folder…"), this);
    connect(open, &QPushButton::clicked, this, &OrganiseView::chooseFolder);

    m_filmstripButton = new QToolButton(this);
    m_filmstripButton->setText(tr("Filmstrip"));
    m_filmstripButton->setCheckable(true);
    m_filmstripButton->setChecked(true);
    m_gridButton = new QToolButton(this);
    m_gridButton->setText(tr("Grid"));
    m_gridButton->setCheckable(true);
    connect(m_filmstripButton, &QToolButton::clicked, this, [this] { setFilmstrip(true); });
    connect(m_gridButton, &QToolButton::clicked, this, [this] { setFilmstrip(false); });

    m_counts = new QLabel(this);
    m_commitButton = new QPushButton(tr("Organise into Folders…"), this);
    m_commitButton->setToolTip(tr("Move each named run of photos into a folder of its name inside "
                                  "this folder, and rejects into \"Rejects\". File names are kept; "
                                  "it can be undone."));
    connect(m_commitButton, &QPushButton::clicked, this, &OrganiseView::commit);
    m_undoButton = new QPushButton(tr("Undo Last Organise"), this);
    connect(m_undoButton, &QPushButton::clicked, this, &OrganiseView::undoLast);

    auto *bar = new QHBoxLayout;
    bar->addWidget(open);
    bar->addWidget(m_folderLabel, 1);
    bar->addWidget(m_counts);
    bar->addSpacing(12);
    bar->addWidget(m_filmstripButton);
    bar->addWidget(m_gridButton);
    bar->addSpacing(12);
    bar->addWidget(m_undoButton);
    bar->addWidget(m_commitButton);

    m_hint = new QLabel(tr("Click the first photo of each taxon to name it — the photos after it "
                           "go with it, up to the next named one. R marks a reject · Space or ⤢ "
                           "shows it full size · the wheel scrolls · F11 full screen."),
                        this);
    m_hint->setWordWrap(true);
    QPalette pal = m_hint->palette();
    pal.setColor(QPalette::WindowText, themeColors(currentThemeVariant()).mutedText);
    m_hint->setPalette(pal);

    m_model = new OrganiseModel(m_session, m_app.thumbnails(), this);
    m_delegate = new OrganiseDelegate(this);
    m_list = new QListView(this);
    m_list->setModel(m_model);
    m_list->setItemDelegate(m_delegate);
    m_list->setViewMode(QListView::IconMode);
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->viewport()->installEventFilter(this);
    m_list->installEventFilter(this);
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex &index) {
        if (QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            return;   // extending a selection
        const QPoint at = m_list->viewport()->mapFromGlobal(QCursor::pos());
        if (OrganiseDelegate::zoomRect(m_list->visualRect(index)).contains(at))
            showFullSize(index.row());
        else
            nameAt(index.row());
    });
    connect(m_list, &QListView::customContextMenuRequested, this, &OrganiseView::contextMenuAt);

    // Under the filmstrip: the selected photo, large enough to tell what it is.
    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumHeight(120);
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_previewCaption = new QLabel(this);
    m_previewCaption->setAlignment(Qt::AlignCenter);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { updatePreview(); });
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this,
            [this](const QString &key, int px) {
                if (px == thumb::ThumbnailCache::kPreviewPx && key == m_previewKey)
                    updatePreview();
            });

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(bar);
    layout->addWidget(m_hint);
    layout->addWidget(m_list);
    layout->addWidget(m_preview, 1);
    layout->addWidget(m_previewCaption);

    setFilmstrip(true);
    updateCounts();
}

OrganiseView::~OrganiseView() = default;

void OrganiseView::chooseFolder()
{
    QString start = m_session.folder();
    if (start.isEmpty())
        start = m_app.settings().organiseFolder();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open a Folder of New Photos"), start.isEmpty() ? QDir::homePath() : start);
    if (!dir.isEmpty())
        openFolder(dir);
}

void OrganiseView::openFolder(const QString &folder)
{
    if (m_loading)
        return;
    m_loading = true;
    m_app.settings().setOrganiseFolder(folder);
    m_folderLabel->setText(QDir::toNativeSeparators(folder));
    m_counts->setText(tr("Reading the photos…"));
    m_commitButton->setEnabled(false);
    m_undoButton->setEnabled(false);
    auto *watcher = new QFutureWatcher<QList<organise::Item>>(this);
    connect(watcher, &QFutureWatcher<QList<organise::Item>>::finished, this, [this, watcher, folder] {
        watcher->deleteLater();
        m_loading = false;
        m_session.setItems(folder, watcher->result());
        m_session.loadProgress();
        m_model->reset();
        updateCellSize();
        updateCounts();
        if (m_session.count() > 0) {
            // Carry on from the last photo named.
            int at = 0;
            for (int i = 0; i < m_session.count(); ++i) {
                if (!m_session.nameAt(i).isEmpty())
                    at = i;
            }
            m_list->setCurrentIndex(m_model->index(at));
            m_list->scrollTo(m_model->index(at), QAbstractItemView::PositionAtCenter);
        }
        m_list->setFocus();
    });
    watcher->setFuture(QtConcurrent::run([folder] { return organise::OrganiseSession::scanFolder(folder); }));
}

void OrganiseView::setFilmstrip(bool on)
{
    m_filmstrip = on;
    m_filmstripButton->setChecked(on);
    m_gridButton->setChecked(!on);
    m_list->setFlow(on ? QListView::LeftToRight : QListView::LeftToRight);
    m_list->setWrapping(!on);
    m_list->setHorizontalScrollBarPolicy(on ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(on ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    m_preview->setVisible(on);
    m_previewCaption->setVisible(on);
    // The strip is a band across the top; the grid fills the view.
    if (!on)
        m_list->setMaximumHeight(QWIDGETSIZE_MAX);
    m_list->setSizePolicy(QSizePolicy::Expanding, on ? QSizePolicy::Fixed : QSizePolicy::Expanding);
    updateCellSize();
    updatePreview();
    if (m_list->currentIndex().isValid())
        m_list->scrollTo(m_list->currentIndex(), QAbstractItemView::PositionAtCenter);
}

void OrganiseView::updateCellSize()
{
    QSize cell;
    if (m_filmstrip) {
        // A band of 3:2 frames, about a third of the view's height.
        const int h = std::clamp(height() * 3 / 10, 200, 320);
        const int imageH = h - OrganiseDelegate::kBand - OrganiseDelegate::kCaption - 2 * OrganiseDelegate::kMargin - 12;
        cell = QSize(imageH * 3 / 2 + 2 * OrganiseDelegate::kMargin + 12, h);
        m_list->setFixedHeight(h + m_list->horizontalScrollBar()->sizeHint().height() + 2 * m_list->frameWidth() + 2);
    } else {
        cell = QSize(260, 240);
    }
    if (cell != m_delegate->cell) {
        m_delegate->cell = cell;
        m_list->setGridSize(cell);
        m_list->doItemsLayout();
    }
}

void OrganiseView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateCellSize();
    updatePreview();
}

void OrganiseView::updatePreview()
{
    if (!m_filmstrip)
        return;
    const int row = m_list->currentIndex().row();
    if (row < 0 || row >= m_session.count()) {
        m_preview->clear();
        m_previewCaption->clear();
        m_previewKey.clear();
        return;
    }
    const organise::Item &item = m_session.items().at(row);
    m_previewKey = thumb::ThumbnailCache::keyFor({}, item.previewPath);
    const QPixmap px = m_app.thumbnails().thumbnail({}, item.previewPath, thumb::ThumbnailCache::kPreviewPx);
    if (px.isNull()) {
        m_preview->setText(tr("Loading…"));
    } else {
        const qreal dpr = devicePixelRatioF();
        QPixmap scaled = px.scaled(m_preview->size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        m_preview->setPixmap(scaled);
    }
    const QString run = m_session.runName(row);
    m_previewCaption->setText(
        QStringLiteral("<b>%1</b> · %2 · %3%4%5")
            .arg(item.baseName.toHtmlEscaped(), item.kinds.join(QLatin1Char('+')),
                 QLocale().toString(item.taken, QLocale::LongFormat).toHtmlEscaped(),
                 run.isEmpty() ? tr(" · not named yet") : QStringLiteral(" · <b>%1</b>").arg(run.toHtmlEscaped()),
                 m_session.isRejected(row) ? tr(" · <span style='color:#c33'>reject</span>") : QString()));
}

void OrganiseView::updateCounts()
{
    const bool open = !m_session.folder().isEmpty();
    m_undoButton->setEnabled(open && organise::OrganiseMover::canUndo(m_session.folder()));
    if (!open || m_session.count() == 0) {
        m_counts->setText(open ? tr("No photos in this folder") : QString());
        m_commitButton->setEnabled(false);
        return;
    }
    const organise::OrganisePlan plan = m_session.plan();
    m_counts->setText(tr("%Ln photo(s)", nullptr, m_session.count()) + QStringLiteral(" · ")
                      + tr("%Ln taxa", nullptr, int(plan.folders.size())) + QStringLiteral(" · ")
                      + tr("%Ln reject(s)", nullptr, int(plan.rejects.size())) + QStringLiteral(" · ")
                      + tr("%Ln unnamed", nullptr, plan.unnamed));
    m_commitButton->setEnabled(!plan.folders.isEmpty() || !plan.rejects.isEmpty());
}

void OrganiseView::changed(int row)
{
    m_model->runChanged(row);
    m_session.saveProgress();
    updateCounts();
    updatePreview();
}

void OrganiseView::nameAt(int row)
{
    if (row < 0 || row >= m_session.count())
        return;
    const organise::Item &item = m_session.items().at(row);
    NamePopup popup(m_app.database().connectionName(), item.baseName, m_session.nameAt(row),
                    m_session.recentNames(), this);
    // Beside the photo it names.
    const QRect r = m_list->visualRect(m_model->index(row));
    QPoint at = m_list->viewport()->mapToGlobal(QPoint(r.right() + 4, r.top()));
    popup.move(at);
    if (popup.exec() != QDialog::Accepted)
        return;
    m_session.setName(row, popup.cleared() ? QString() : popup.chosen());
    changed(row);
}

void OrganiseView::toggleRejected()
{
    const QModelIndexList rows = m_list->selectionModel()->selectedIndexes();
    if (rows.isEmpty())
        return;
    // All rejected: un-reject; otherwise reject them all.
    bool all = true;
    for (const QModelIndex &i : rows)
        all = all && m_session.isRejected(i.row());
    int first = m_session.count();
    for (const QModelIndex &i : rows) {
        m_session.setRejected(i.row(), !all);
        first = std::min(first, i.row());
    }
    changed(first);
    // On to the next photo, as when culling.
    if (rows.size() == 1 && rows.first().row() + 1 < m_session.count()) {
        const QModelIndex next = m_model->index(rows.first().row() + 1);
        m_list->setCurrentIndex(next);
        m_list->scrollTo(next);
    }
}

void OrganiseView::showFullSize(int row)
{
    if (row < 0 || row >= m_session.count())
        return;
    QVector<ImageViewer::Item> items;
    for (int i = 0; i < m_session.count(); ++i) {
        const organise::Item &it = m_session.items().at(i);
        ImageViewer::Item v;
        v.path = it.previewPath;
        v.title = it.baseName;
        const QString run = m_session.runName(i);
        v.caption = it.baseName + QStringLiteral(" · ") + it.kinds.join(QLatin1Char('+'))
                    + QStringLiteral(" · ") + QLocale().toString(it.taken, QLocale::ShortFormat)
                    + (run.isEmpty() ? QString() : QStringLiteral("\n") + run)
                    + (m_session.isRejected(i) ? tr("\nreject") : QString());
        items << v;
    }
    if (!m_viewer)
        m_viewer = new ImageViewer(window());
    m_viewer->show();
    m_viewer->raise();
    m_viewer->activateWindow();
    m_viewer->setItems(items, row);
}

void OrganiseView::contextMenuAt(const QPoint &pos)
{
    const QModelIndex index = m_list->indexAt(pos);
    if (!index.isValid())
        return;
    if (!m_list->selectionModel()->isSelected(index))
        m_list->setCurrentIndex(index);
    const int row = index.row();
    QMenu menu(this);
    menu.addAction(m_session.nameAt(row).isEmpty() ? tr("Name This Photo…") : tr("Rename…"), this,
                   [this, row] { nameAt(row); });
    if (!m_session.nameAt(row).isEmpty())
        menu.addAction(tr("Clear Name"), this, [this, row] {
            m_session.setName(row, QString());
            changed(row);
        });
    menu.addAction(m_session.isRejected(row) ? tr("Not a Reject") : tr("Reject"), this,
                   [this] { toggleRejected(); });
    menu.addSeparator();
    menu.addAction(tr("View Full Size"), this, [this, row] { showFullSize(row); });
    menu.addAction(tr("Show in File Manager"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_session.folder()));
    });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

bool OrganiseView::eventFilter(QObject *watched, QEvent *event)
{
    // The wheel scrolls the filmstrip sideways.
    if (watched == m_list->viewport() && event->type() == QEvent::Wheel && m_filmstrip) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const QPoint d = wheel->angleDelta();
        const int delta = std::abs(d.y()) > std::abs(d.x()) ? d.y() : d.x();
        QScrollBar *bar = m_list->horizontalScrollBar();
        bar->setValue(bar->value() - delta * m_delegate->cell.width() / 240);
        return true;
    }
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int row = m_list->currentIndex().row();
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            nameAt(row);
            return true;
        case Qt::Key_R:
        case Qt::Key_Delete:
            toggleRejected();
            return true;
        case Qt::Key_Space:
            showFullSize(row);
            return true;
        case Qt::Key_F11: {
            QWidget *w = window();
            w->isFullScreen() ? w->showNormal() : w->showFullScreen();
            return true;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OrganiseView::commit()
{
    const organise::OrganisePlan plan = m_session.plan();
    if (plan.folders.isEmpty() && plan.rejects.isEmpty())
        return;
    QStringList lines;
    for (const organise::FolderMove &f : plan.folders)
        lines << tr("%1 — %Ln photo(s)", nullptr, int(f.items.size())).arg(f.folderName);
    if (!plan.rejects.isEmpty())
        lines << tr("%1 — %Ln photo(s)", nullptr, int(plan.rejects.size()))
                     .arg(QLatin1String(organise::OrganiseSession::kRejectsFolder));
    QMessageBox box(QMessageBox::Question, tr("Organise into Folders"),
                    tr("Move %Ln photo(s) into %1 folder(s) inside %2?", nullptr,
                       plan.photos() + int(plan.rejects.size()))
                        .arg(plan.folders.size() + (plan.rejects.isEmpty() ? 0 : 1))
                        .arg(QDir::toNativeSeparators(m_session.folder())),
                    QMessageBox::NoButton, this);
    box.setInformativeText(
        (plan.unnamed > 0 ? tr("%Ln photo(s) before the first name stay where they are. ", nullptr, plan.unnamed)
                          : QString())
        + tr("File names are kept, RAW and JPEG and sidecar files move together, nothing is "
             "overwritten, and Undo Last Organise puts everything back."));
    box.setDetailedText(lines.join(QLatin1Char('\n')));
    QPushButton *go = box.addButton(tr("Organise"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(go);
    box.exec();
    if (box.clickedButton() != go)
        return;

    QList<organise::ShotMove> moves;
    for (const organise::FolderMove &f : plan.folders) {
        for (int i : f.items) {
            const organise::Item &it = m_session.items().at(i);
            moves.append({it.baseName, it.files, f.folderName});
        }
    }
    for (int i : plan.rejects) {
        const organise::Item &it = m_session.items().at(i);
        moves.append({it.baseName, it.files, QLatin1String(organise::OrganiseSession::kRejectsFolder)});
    }
    const QString folder = m_session.folder();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const organise::OrganiseMover::Result result = organise::OrganiseMover::execute(folder, moves);
    QApplication::restoreOverrideCursor();
    if (!result.failures.isEmpty())
        QMessageBox::warning(this, tr("Organise into Folders"),
                             tr("%Ln photo(s) couldn't be moved and were left where they were:\n\n%1",
                                nullptr, int(result.failures.size()))
                                 .arg(result.failures.mid(0, 10).join(QLatin1Char('\n'))));
    emit statusMessage(tr("Organised %Ln photo(s) into folders.", nullptr, result.moved), 8000);
    emit organised(folder);
    openFolder(folder);
}

void OrganiseView::undoLast()
{
    const QString folder = m_session.folder();
    if (folder.isEmpty() || !organise::OrganiseMover::canUndo(folder))
        return;
    if (QMessageBox::question(this, tr("Undo Last Organise"),
                              tr("Move the photos the last Organise moved back into %1?")
                                  .arg(QDir::toNativeSeparators(folder)))
        != QMessageBox::Yes)
        return;
    const organise::OrganiseMover::Result result = organise::OrganiseMover::undo(folder);
    if (!result.failures.isEmpty())
        QMessageBox::warning(this, tr("Undo Last Organise"),
                             tr("%Ln file(s) couldn't be moved back:\n\n%1", nullptr,
                                int(result.failures.size()))
                                 .arg(result.failures.mid(0, 10).join(QLatin1Char('\n'))));
    emit statusMessage(tr("Moved %Ln photo(s) back.", nullptr, result.moved), 8000);
    emit organised(folder);
    openFolder(folder);
}

} // namespace pl
