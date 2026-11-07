#include "ui/TaxonColumnView.h"

#include "app/Application.h"
#include "coverage/CoverageCalculator.h"
#include "db/Database.h"
#include "model/TaxonomyTreeModel.h"
#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace pl {

namespace {

using Model = model::TaxonomyTreeModel;

constexpr int kPad = 6;
constexpr int kGap = 8;
constexpr int kArrowW = 10;
constexpr int kCheckW = 16;
constexpr int kColumnWidth = 300;
constexpr int kCardPhotoW = 280;
constexpr int kCardPhotoH = 210;
constexpr int kOwnPhotoPx = 140;

QFont smallFont(const QFont &base)
{
    QFont f = base;
    f.setPointSizeF(base.pointSizeF() * 0.85);
    return f;
}

bool isLeaf(const QModelIndex &index)
{
    return index.data(Model::SeenRole).isValid();   // species and lower only
}

QString coverageText(const QModelIndex &index)
{
    if (isLeaf(index))
        return index.data(Model::HasPhotosRole).toBool() ? QStringLiteral("✔") : QStringLiteral("·");
    const int total = index.data(Model::SpeciesTotalRole).toInt();
    if (total <= 0)
        return {};
    return QStringLiteral("%1 / %2").arg(index.data(Model::SpeciesWithPhotosRole).toInt()).arg(total);
}

bool coverageComplete(const QModelIndex &index)
{
    if (isLeaf(index))
        return index.data(Model::HasPhotosRole).toBool();
    const int total = index.data(Model::SpeciesTotalRole).toInt();
    return total > 0 && index.data(Model::SpeciesWithPhotosRole).toInt() == total;
}

// The rank to show: iNaturalist's top rank for Life ("stateofmatter") says nothing.
QString rankText(const QModelIndex &index)
{
    const QString rank = index.data(Model::RankRole).toString();
    return rank == QLatin1String("stateofmatter") ? QString() : rank;
}

struct RowLayout
{
    QRect name, rank, coverage, check, arrow;
};

RowLayout layoutRow(const QRect &rect, const QModelIndex &index, const QFont &font, bool showSeen)
{
    RowLayout l;   // `name` spans both lines: the common name goes under the scientific one
    const QFontMetrics fm(font);
    const QFontMetrics small(smallFont(font));
    int right = rect.right() - kPad;
    l.arrow = QRect(right - kArrowW + 1, rect.top(), kArrowW, rect.height());
    right -= kArrowW + kGap;
    if (showSeen) {
        if (isLeaf(index))
            l.check = QRect(right - kCheckW + 1, rect.center().y() - kCheckW / 2, kCheckW, kCheckW);
        right -= kCheckW + kGap;
    }
    const QString cover = coverageText(index);
    const int coverW = std::max(fm.horizontalAdvance(cover), fm.horizontalAdvance(QStringLiteral("00 / 00")));
    l.coverage = QRect(right - coverW + 1, rect.top(), coverW, rect.height());
    right -= coverW + kGap;
    const int rankW = small.horizontalAdvance(rankText(index));
    l.rank = QRect(right - rankW + 1, rect.top(), rankW, rect.height());
    right -= rankW + kGap;
    l.name = QRect(rect.left() + kPad, rect.top(), std::max(0, right - rect.left() - kPad), rect.height());
    return l;
}

} // namespace

// ---------------------------------------------------------------- delegate

QRect TaxonColumnDelegate::checkRect(const QRect &rect, const QModelIndex &index) const
{
    return layoutRow(rect, index, QApplication::font(), m_showSeen).check;
}

QSize TaxonColumnDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    const int lines = option.fontMetrics.height() + QFontMetrics(smallFont(option.font)).height();
    return {kColumnWidth - 20, std::max(lines + 10, kCheckW + 8)};
}

void TaxonColumnDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    // The row's background, selection and focus as the view draws them.
    const QString text = index.data(Model::NameRole).toString();
    const QString common = index.data(Model::CommonNameRole).toString();
    QFont nameFont = opt.font;
    opt.text.clear();
    opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
    style->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget);

    const ThemeColors &theme = themeColors(currentThemeVariant());
    const RowLayout l = layoutRow(option.rect, index, nameFont, m_showSeen);
    const bool selected = option.state & QStyle::State_Selected;
    QColor nameColor = option.palette.color(QPalette::Text);
    if (const QVariant fg = index.data(Qt::ForegroundRole); fg.isValid() && !selected)
        nameColor = fg.value<QColor>();

    p->save();
    const QFont small = smallFont(option.font);
    const QFontMetrics nameFm(nameFont), smallFm(small);
    const int textH = nameFm.height() + (common.isEmpty() ? 0 : smallFm.height());
    QRect line1 = l.name;
    line1.setTop(l.name.top() + (l.name.height() - textH) / 2);
    line1.setHeight(nameFm.height());
    p->setFont(nameFont);
    p->setPen(nameColor);
    p->drawText(line1, Qt::AlignLeft | Qt::AlignVCenter, nameFm.elidedText(text, Qt::ElideRight, line1.width()));
    if (!common.isEmpty()) {
        const QRect line2(line1.left(), line1.bottom() + 1, line1.width(), smallFm.height());
        p->setFont(small);
        p->setPen(selected ? nameColor : theme.mutedText);
        p->drawText(line2, Qt::AlignLeft | Qt::AlignVCenter, smallFm.elidedText(common, Qt::ElideRight, line2.width()));
    }

    p->setFont(small);
    p->setPen(theme.mutedText);
    p->drawText(l.rank, Qt::AlignRight | Qt::AlignVCenter, rankText(index));

    p->setFont(option.font);
    p->setPen(coverageComplete(index) ? theme.positive : theme.mutedText);
    p->drawText(l.coverage, Qt::AlignRight | Qt::AlignVCenter, coverageText(index));

    if (!l.check.isNull()) {
        QStyleOptionViewItem check;
        check.rect = l.check;
        check.palette = option.palette;
        check.state = QStyle::State_None;
        if (index.data(Model::SeenEditableRole).toBool())
            check.state |= QStyle::State_Enabled;
        check.state |= index.data(Model::SeenRole).toInt() == Qt::Checked ? QStyle::State_On : QStyle::State_Off;
        style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &check, p, widget);
    }

    if (index.model() && index.model()->hasChildren(index)) {
        // A chevron: there's more in the next column.
        const QRectF a = QRectF(l.arrow).adjusted(2, 0, -2, 0);
        const qreal cy = a.center().y();
        QPainterPath chevron;
        chevron.moveTo(a.left(), cy - 4);
        chevron.lineTo(a.right(), cy);
        chevron.lineTo(a.left(), cy + 4);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(theme.mutedText, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        p->drawPath(chevron);
    }
    p->restore();
}

bool TaxonColumnDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                                      const QModelIndex &index)
{
    if (!m_showSeen || !isLeaf(index))
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    const auto type = event->type();
    if (type != QEvent::MouseButtonRelease && type != QEvent::MouseButtonDblClick)
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    const auto *mouse = static_cast<QMouseEvent *>(event);
    const QRect check = layoutRow(option.rect, index, option.font, true).check;
    if (mouse->button() != Qt::LeftButton || !check.adjusted(-3, -3, 3, 3).contains(mouse->position().toPoint()))
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    if (type == QEvent::MouseButtonRelease && index.data(Model::SeenEditableRole).toBool()) {
        const bool on = index.data(Model::SeenRole).toInt() == Qt::Checked;
        model->setData(index.sibling(index.row(), Model::SeenColumn), on ? Qt::Unchecked : Qt::Checked,
                       Qt::CheckStateRole);
    }
    return true;   // a click on the tick isn't a double-click on the taxon
}

// ---------------------------------------------------------------- card

TaxonCard::TaxonCard(Application &app, QWidget *parent) : QWidget(parent), m_app(app)
{
    const ThemeColors &theme = themeColors(currentThemeVariant());
    m_reference = new QLabel(this);
    m_reference->setAlignment(Qt::AlignCenter);
    m_reference->setFixedSize(kCardPhotoW, kCardPhotoH);
    m_credit = new QLabel(this);
    m_credit->setWordWrap(true);
    m_credit->setAlignment(Qt::AlignCenter);
    m_credit->setFont(smallFont(m_credit->font()));
    m_names = new QLabel(this);
    m_names->setWordWrap(true);
    m_names->setAlignment(Qt::AlignCenter);
    m_names->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details = new QLabel(this);
    m_details->setWordWrap(true);
    m_details->setAlignment(Qt::AlignCenter);
    m_own = new QLabel(this);
    m_own->setAlignment(Qt::AlignCenter);
    m_own->setFixedSize(kOwnPhotoPx, kOwnPhotoPx);
    m_ownCaption = new QLabel(tr("Your photo"), this);
    m_ownCaption->setAlignment(Qt::AlignCenter);
    m_ownCaption->setFont(smallFont(m_ownCaption->font()));
    for (QLabel *muted : {m_credit, m_ownCaption}) {
        QPalette pal = muted->palette();
        pal.setColor(QPalette::WindowText, theme.mutedText);
        muted->setPalette(pal);
    }
    m_showPhotos = new QPushButton(tr("Show Photos"), this);
    m_showPhotos->setToolTip(tr("This taxon's photos in Photos of Tree Selection"));
    m_openInat = new QPushButton(tr("Open on iNaturalist"), this);
    connect(m_showPhotos, &QPushButton::clicked, this, [this] {
        if (m_inatId > 0)
            emit showPhotosRequested(m_inatId);
    });
    connect(m_openInat, &QPushButton::clicked, this, [this] {
        if (m_inatId > 0)
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(m_inatId)));
    });
    connect(&m_app.photoCache(), &net::PhotoCache::ready, this, [this](const QString &url) {
        if (url == m_photoUrl)
            showReferencePhoto();
    });
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, [this](const QString &key, int px) {
        if (px == thumb::ThumbnailCache::kGridPx && !m_ownPath.isEmpty()
            && key == thumb::ThumbnailCache::keyFor(m_ownHash, m_ownPath))
            showOwnPhoto();
    });

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_showPhotos);
    buttons->addWidget(m_openInat);
    buttons->addStretch(1);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(m_reference, 0, Qt::AlignHCenter);
    layout->addWidget(m_credit);
    layout->addSpacing(4);
    layout->addWidget(m_names);
    layout->addWidget(m_details);
    layout->addSpacing(4);
    layout->addWidget(m_own, 0, Qt::AlignHCenter);
    layout->addWidget(m_ownCaption);
    layout->addLayout(buttons);
    layout->addStretch(1);
    setMinimumWidth(kCardPhotoW + 20);
}

QString TaxonCard::namesText() const
{
    return m_names->text();
}

void TaxonCard::setTaxon(const QModelIndex &index, int projectId)
{
    m_inatId = index.isValid() ? index.data(Model::InatIdRole).toLongLong() : 0;
    m_photoUrl.clear();
    m_ownPath.clear();
    m_ownHash.clear();
    if (m_inatId <= 0) {
        m_names->clear();
        m_details->clear();
        m_reference->clear();
        m_credit->clear();
        m_own->hide();
        m_ownCaption->hide();
        return;
    }
    const QString name = index.data(Model::NameRole).toString();
    const QString rank = index.data(Model::RankRole).toString();
    const taxonomy::TaxonomyStore::TaxonPhoto photo = m_app.taxonomyStore().taxonPhoto(m_inatId);
    const QString common = photo.commonName == name ? QString() : photo.commonName;
    m_names->setText(common.isEmpty()
                         ? QStringLiteral("<i>%1</i>").arg(name.toHtmlEscaped())
                         : QStringLiteral("<b>%1</b><br><i>%2</i>").arg(common.toHtmlEscaped(), name.toHtmlEscaped()));

    QStringList details{rank};
    const bool photographed = index.data(Model::HasPhotosRole).toBool();
    const int photos = index.data(Model::CaptureCountRole).toInt();
    details << (photographed ? tr("Photographed — %n photo(s)", nullptr, photos) : tr("Not photographed yet"));
    if (const QString status = index.data(Model::StatusRole).toString(); !status.isEmpty())
        details << status;
    if (!photographed && index.data(Model::SeenRole).toInt() == Qt::Checked)
        details << tr("Seen");
    m_details->setText(details.join(QStringLiteral(" · ")));

    m_photoUrl = photo.photoUrl;
    m_credit->setText(photo.attribution);
    showReferencePhoto();

    if (photographed && projectId > 0) {
        const coverage::RepresentativeImage cover =
            coverage::representativeFor(m_app.database().connectionName(), projectId, m_inatId);
        if (cover.isValid()) {
            m_ownPath = cover.previewPath;
            m_ownHash = cover.previewHash;
        }
    }
    showOwnPhoto();
}

void TaxonCard::showReferencePhoto()
{
    if (m_photoUrl.isEmpty()) {
        m_reference->setText(tr("No iNaturalist photo of this taxon"));
        return;
    }
    const QPixmap pm = m_app.photoCache().photo(m_photoUrl, net::PhotoCache::Urgency::Now);
    if (pm.isNull()) {
        m_reference->setText(m_app.photoCache().hasFailed(m_photoUrl) ? tr("Couldn't download the photo")
                                                                       : tr("Loading from iNaturalist…"));
        return;
    }
    const qreal dpr = devicePixelRatioF();
    QPixmap shown = pm.scaled(m_reference->size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    shown.setDevicePixelRatio(dpr);
    m_reference->setPixmap(shown);
}

void TaxonCard::showOwnPhoto()
{
    const bool have = !m_ownPath.isEmpty();
    m_own->setVisible(have);
    m_ownCaption->setVisible(have);
    if (!have)
        return;
    const QPixmap pm = m_app.thumbnails().thumbnail(m_ownHash, m_ownPath, thumb::ThumbnailCache::kGridPx);
    if (pm.isNull()) {
        m_own->setText(tr("Loading…"));
        return;
    }
    const qreal dpr = devicePixelRatioF();
    QPixmap shown = pm.scaled(m_own->size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    shown.setDevicePixelRatio(dpr);
    m_own->setPixmap(shown);
}

// ---------------------------------------------------------------- view

TaxonColumnView::TaxonColumnView(Application &app, QWidget *parent) : QColumnView(parent)
{
    m_delegate = new TaxonColumnDelegate(this);
    setItemDelegate(m_delegate);
    setResizeGripsVisible(true);
    setColumnWidths(QList<int>(32, kColumnWidth));
    m_card = new TaxonCard(app);
    m_card->hide();
}

TaxonColumnView::~TaxonColumnView()
{
    if (m_card && !m_card->parent())
        delete m_card;   // never placed anywhere
}

void TaxonColumnView::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
    QColumnView::currentChanged(current, previous);
    updateCard();
}

void TaxonColumnView::updateCard()
{
    const QModelIndex current = currentIndex();
    // Species and lower (the ones with a Seen tick) get a card.
    if (!current.isValid() || !current.data(Model::SeenRole).isValid()) {
        m_card->hide();
        return;
    }
    m_card->setTaxon(current, m_projectId);
    m_card->show();
}

void TaxonColumnView::setShowSeen(bool on)
{
    if (m_delegate->showSeen() == on)
        return;
    m_delegate->setShowSeen(on);
    viewport()->update();
    for (QAbstractItemView *column : findChildren<QAbstractItemView *>())
        column->viewport()->update();
}

QAbstractItemView *TaxonColumnView::createColumn(const QModelIndex &rootIndex)
{
    QAbstractItemView *column = QColumnView::createColumn(rootIndex);
    if (auto *list = qobject_cast<QListView *>(column))
        list->setUniformItemSizes(true);
    column->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(column, &QWidget::customContextMenuRequested, this, [this, column](const QPoint &pos) {
        const QModelIndex index = column->indexAt(pos);
        if (index.isValid())
            emit taxonContextMenu(index, column->viewport()->mapToGlobal(pos));
    });
    connect(column, &QAbstractItemView::doubleClicked, this, &TaxonColumnView::taxonActivated);
    return column;
}

} // namespace pl
