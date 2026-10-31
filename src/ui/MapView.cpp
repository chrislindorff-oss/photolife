#include "ui/MapView.h"

#include "ui/MapTiles.h"

#include "geo/WebMercator.h"
#include "model/CaptureListModel.h"
#include "net/TileCache.h"
#include "ui/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineF>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace pl {
namespace {

constexpr int kPanelThumb = 48;

// A photo's thumbnail centred in a fixed square, so landscape and portrait
// shots line their text up the same in the pin panel.
QIcon squareThumb(const QModelIndex &idx)
{
    const QIcon icon = idx.data(Qt::DecorationRole).value<QIcon>();
    QPixmap square(kPanelThumb, kPanelThumb);
    square.fill(Qt::transparent);
    if (!icon.isNull()) {
        const QPixmap pm = icon.pixmap(kPanelThumb * 2, kPanelThumb * 2)
                               .scaled(kPanelThumb, kPanelThumb, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
        QPainter p(&square);
        p.drawPixmap((kPanelThumb - pm.width()) / 2, (kPanelThumb - pm.height()) / 2, pm);
    }
    return QIcon(square);
}

} // namespace

MapView::MapView(model::CaptureListModel &model, net::TileCache &tiles, QWidget *parent)
    : QWidget(parent), m_model(model), m_tiles(tiles)
{
    setMouseTracking(false);
    setMinimumSize(200, 200);

    connect(&m_model, &QAbstractItemModel::modelReset, this, &MapView::rebuildPins);
    connect(&m_tiles, &net::TileCache::ready, this, [this](int, int, int) { update(); });

    m_fitButton = new QPushButton(tr("Fit to Photos"), this);
    connect(m_fitButton, &QPushButton::clicked, this, [this] {
        fitToPins();
        update();
    });

    m_localityButton = new QPushButton(tr("Restrict to Active Reference Tree Locality"), this);
    m_localityButton->setCheckable(true);
    m_localityButton->setEnabled(false);   // enabled once setActiveLocality() has a box
    m_localityButton->setToolTip(
        tr("Hide map pins outside the active reference tree's locality, even if their "
           "matched species also occurs elsewhere."));
    connect(m_localityButton, &QPushButton::toggled, this, [this] { rebuildPins(); });

    m_attribution = new QLabel(this);
    m_attribution->setTextFormat(Qt::RichText);
    m_attribution->setText(tr("<a href=\"https://www.openstreetmap.org/copyright\">"
                              "© OpenStreetMap contributors</a>"));
    m_attribution->setOpenExternalLinks(true);
    {
        const QColor panel = pl::themeColors(pl::currentThemeVariant()).panel;
        m_attribution->setStyleSheet(
            QStringLiteral("background-color: rgba(%1,%2,%3,190); padding: 2px 5px;")
                .arg(panel.red()).arg(panel.green()).arg(panel.blue()));
    }
    m_attribution->adjustSize();

    m_emptyOverlay = new QLabel(tr("No geotagged photos in this selection."), this);
    m_emptyOverlay->setAlignment(Qt::AlignCenter);
    m_emptyOverlay->setWordWrap(true);
    m_emptyOverlay->setEnabled(false);
    m_emptyOverlay->hide();

    m_panel = new QFrame(this);
    m_panel->setObjectName(QStringLiteral("mapClusterPanel"));
    {
        const pl::ThemeColors &theme = pl::themeColors(pl::currentThemeVariant());
        m_panel->setStyleSheet(
            QStringLiteral("#mapClusterPanel { background: %1; border: 1px solid %2; "
                           "border-radius: %3px; }")
                .arg(theme.panel.name(), theme.border.name())
                .arg(theme.radius));
    }
    m_panelBack = new QToolButton(m_panel);
    m_panelBack->setText(tr("‹ All taxa"));
    m_panelBack->setAutoRaise(true);
    connect(m_panelBack, &QToolButton::clicked, this, &MapView::showPanelTaxa);
    m_panelTitle = new QLabel(m_panel);
    m_panelTitle->setTextFormat(Qt::PlainText);
    auto *close = new QToolButton(m_panel);
    close->setText(QStringLiteral("✕"));
    close->setAutoRaise(true);
    close->setToolTip(tr("Close (Esc)"));
    connect(close, &QToolButton::clicked, this, &MapView::closePanel);
    m_panelList = new QListWidget(m_panel);
    m_panelList->setIconSize(QSize(kPanelThumb, kPanelThumb));
    m_panelList->setUniformItemSizes(false);
    m_panelList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_panelList->setTextElideMode(Qt::ElideRight);
    m_panelList->setWordWrap(false);
    m_panelTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // never widens the panel
    m_panelList->setSelectionMode(QAbstractItemView::SingleSelection);
    // One click, like the pins themselves; Enter works too.
    connect(m_panelList, &QListWidget::itemClicked, this, &MapView::onPanelItemActivated);
    connect(m_panelList, &QListWidget::itemActivated, this, &MapView::onPanelItemActivated);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(m_panelBack);
    header->addWidget(m_panelTitle, 1);
    header->addWidget(close);
    auto *panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setContentsMargins(6, 4, 6, 6);
    panelLayout->setSpacing(4);
    panelLayout->addLayout(header);
    panelLayout->addWidget(m_panelList);
    m_panel->hide();

    // Thumbnails arrive after the list is drawn: refresh the photo icons.
    connect(&m_model, &QAbstractItemModel::dataChanged, this, [this] {
        if (m_panel->isVisible() && m_panelTaxon) {
            for (int i = 0; i < m_panelList->count(); ++i) {
                QListWidgetItem *item = m_panelList->item(i);
                item->setIcon(squareThumb(m_model.index(item->data(Qt::UserRole).toInt())));
            }
        }
    });

    rebuildPins();
}

void MapView::rebuildPins()
{
    closePanel();   // its rows belong to the model as it was
    QList<geo::Pin> candidates;
    const int rows = m_model.rowCount();
    for (int r = 0; r < rows; ++r) {
        const QModelIndex idx = m_model.index(r);
        if (!idx.data(model::CaptureListModel::HasGpsRole).toBool())
            continue;
        geo::Pin pin;
        pin.lon = idx.data(model::CaptureListModel::LongitudeRole).toDouble();
        pin.lat = idx.data(model::CaptureListModel::LatitudeRole).toDouble();
        pin.row = r;
        candidates.append(pin);
    }

    const bool restrict = m_localityButton->isChecked() && m_locality.has_value();
    if (restrict) {
        m_pins.clear();
        for (const geo::Pin &pin : candidates) {
            if (inLocality(pin.lon, pin.lat))
                m_pins.append(pin);
        }
    } else {
        m_pins = candidates;
    }

    if (m_pins.isEmpty()) {
        m_clusters.clear();
        m_emptyOverlay->setText(restrict && !candidates.isEmpty()
                                    ? tr("No geotagged photos within this reference tree's "
                                         "locality.")
                                    : tr("No geotagged photos in this selection."));
        m_emptyOverlay->show();
    } else {
        m_emptyOverlay->hide();
        fitToPins();
    }
    layoutOverlays();
    update();
}

bool MapView::inLocality(double lon, double lat) const
{
    if (m_localityShape)
        return geo::shapeContains(*m_localityShape, lon, lat);
    return m_locality && geo::boxContains(*m_locality, lon, lat);
}

void MapView::setShowLocality(bool show)
{
    m_showLocality = show;
    update();
}

void MapView::setActiveLocality(std::optional<geo::GeoBox> box,
                                std::optional<geo::GeoShape> shape)
{
    m_locality = box;
    m_localityShape = std::move(shape);
    m_localityPathZoom = -1;
    m_localityButton->setEnabled(box.has_value());
    if (!box && m_localityButton->isChecked()) {
        QSignalBlocker blocker(m_localityButton);
        m_localityButton->setChecked(false);
    }
    rebuildPins();
}

void MapView::recluster()
{
    m_clusters = geo::clusterPins(m_pins, m_zoom, kClusterPixelRadius);
}

void MapView::fitToPins()
{
    if (m_pins.isEmpty())
        return;

    double minLon = m_pins.first().lon, maxLon = minLon;
    double minLat = m_pins.first().lat, maxLat = minLat;
    for (const geo::Pin &p : m_pins) {
        minLon = std::min(minLon, p.lon);
        maxLon = std::max(maxLon, p.lon);
        minLat = std::min(minLat, p.lat);
        maxLat = std::max(maxLat, p.lat);
    }
    m_centerLon = (minLon + maxLon) / 2.0;
    m_centerLat = (minLat + maxLat) / 2.0;

    const int margin = 40;
    const double availW = std::max(1, width() - 2 * margin);
    const double availH = std::max(1, height() - 2 * margin);

    int zoom = kMaxFitZoom;
    for (; zoom > kMinZoom; --zoom) {
        const QPointF a = geo::lonLatToWorldPixel(minLon, maxLat, zoom);
        const QPointF b = geo::lonLatToWorldPixel(maxLon, minLat, zoom);
        if (std::abs(b.x() - a.x()) <= availW && std::abs(b.y() - a.y()) <= availH)
            break;
    }
    m_zoom = zoom;
    recluster();
}

void MapView::layoutOverlays()
{
    m_fitButton->adjustSize();
    m_fitButton->move(8, 8);
    m_localityButton->adjustSize();
    m_localityButton->move(8, m_fitButton->geometry().bottom() + 8);
    m_attribution->adjustSize();
    m_attribution->move(width() - m_attribution->width() - 8,
                        height() - m_attribution->height() - 8);
    m_emptyOverlay->setGeometry(rect());
    // The empty-state label covers the whole widget and would otherwise sit
    // above these buttons in sibling stacking order -- if toggling the
    // locality restriction itself empties the pin set, the user must still be
    // able to reach the button to turn it back off.
    m_fitButton->raise();
    m_localityButton->raise();
    if (m_panel->isVisible()) {
        placePanel();
        m_panel->raise();
    }
}

QPointF MapView::centerWorldPixel() const
{
    return geo::lonLatToWorldPixel(m_centerLon, m_centerLat, m_zoom);
}

QPointF MapView::lonLatToScreen(double lon, double lat) const
{
    const QPointF px = geo::lonLatToWorldPixel(lon, lat, m_zoom);
    const QPointF origin = centerWorldPixel() - QPointF(width() / 2.0, height() / 2.0);
    return px - origin;
}

QPointF MapView::screenToLonLat(QPointF screen) const
{
    const QPointF origin = centerWorldPixel() - QPointF(width() / 2.0, height() / 2.0);
    return geo::worldPixelToLonLat(origin + screen, m_zoom);
}

void MapView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0xdd, 0xdd, 0xd5));

    const QPointF origin = centerWorldPixel() - QPointF(width() / 2.0, height() / 2.0);
    drawMapTiles(painter, m_tiles, origin, m_zoom, size());

    painter.setRenderHint(QPainter::Antialiasing);
    paintLocality(painter, origin);
    for (const geo::Cluster &c : m_clusters) {
        const QPointF pos = lonLatToScreen(c.centerLonLat.x(), c.centerLonLat.y());
        const bool multi = c.rows.size() > 1;
        painter.setBrush(multi ? QColor(0xe0, 0x60, 0x20) : QColor(0x20, 0x60, 0xd0));
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawEllipse(pos, 11, 11);
        if (multi) {
            painter.setPen(Qt::white);
            painter.drawText(QRectF(pos.x() - 15, pos.y() - 10, 30, 20), Qt::AlignCenter,
                             QString::number(c.rows.size()));
        }
    }
}

// The active tree's region: a lightly tinted area with a clear edge, under
// the pins. Built in world pixels once per zoom level, then just shifted.
void MapView::paintLocality(QPainter &painter, QPointF origin)
{
    if (!m_showLocality || !m_localityShape)
        return;
    if (m_localityPathZoom != m_zoom) {
        m_localityPath = QPainterPath();
        m_localityPath.setFillRule(Qt::OddEvenFill);   // holes stay holes
        for (const QList<QPointF> &ring : m_localityShape->rings) {
            QPolygonF poly;
            poly.reserve(ring.size());
            for (const QPointF &p : ring)
                poly << geo::lonLatToWorldPixel(p.x(), p.y(), m_zoom);
            m_localityPath.addPolygon(poly);
            m_localityPath.closeSubpath();
        }
        m_localityPathZoom = m_zoom;
    }
    painter.save();
    painter.translate(-origin);
    painter.setPen(QPen(QColor(0x7b, 0x1f, 0xa2, 210), 2.0));
    painter.setBrush(QColor(0x7b, 0x1f, 0xa2, 28));
    painter.drawPath(m_localityPath);
    painter.restore();
}

void MapView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutOverlays();
}

void MapView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        m_dragLastPos = event->pos();
    }
    QWidget::mousePressEvent(event);
}

void MapView::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        const QPoint delta = event->pos() - m_dragLastPos;
        if (!m_dragging && delta.manhattanLength() < 4)
            return;
        m_dragging = true;
        m_dragLastPos = event->pos();

        const QPointF newCenterPx = centerWorldPixel() - QPointF(delta);
        const QPointF newCenter = geo::worldPixelToLonLat(newCenterPx, m_zoom);
        m_centerLon = newCenter.x();
        m_centerLat = newCenter.y();
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void MapView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !m_dragging) {
        if (const geo::Cluster *c = clusterAt(event->pos()))
            activateCluster(*c, event->pos());
        else
            closePanel();   // a click on the map itself dismisses the panel
    }
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void MapView::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0)
        return;
    const int newZoom = qBound(kMinZoom, m_zoom + steps, kMaxZoom);
    if (newZoom == m_zoom)
        return;

    const QPointF cursor = event->position();
    const QPointF lonLatUnderCursor = screenToLonLat(cursor);

    m_zoom = newZoom;
    const QPointF newOrigin =
        geo::lonLatToWorldPixel(lonLatUnderCursor.x(), lonLatUnderCursor.y(), m_zoom) - cursor;
    const QPointF newCenterPx = newOrigin + QPointF(width() / 2.0, height() / 2.0);
    const QPointF newCenter = geo::worldPixelToLonLat(newCenterPx, m_zoom);
    m_centerLon = newCenter.x();
    m_centerLat = newCenter.y();

    recluster();
    update();
    event->accept();
}

const geo::Cluster *MapView::clusterAt(QPoint pos) const
{
    for (const geo::Cluster &c : m_clusters) {
        const QPointF screen = lonLatToScreen(c.centerLonLat.x(), c.centerLonLat.y());
        if (QLineF(screen, QPointF(pos)).length() <= kClusterHitRadius)
            return &c;
    }
    return nullptr;
}

void MapView::activateCluster(const geo::Cluster &cluster, QPoint screenPos)
{
    if (cluster.rows.size() == 1) {
        closePanel();
        emit captureActivated(m_model.index(cluster.rows.first()));
        return;
    }
    m_panelRows = cluster.rows;
    m_panelAnchor = screenPos;
    showPanelTaxa();
}

namespace {

// A photo's taxon, for grouping: its matched iNat id, or 0 when unmatched.
qint64 taxonKeyOf(const QModelIndex &idx)
{
    return idx.data(model::CaptureListModel::MatchedTaxonInatIdRole).toLongLong();
}

} // namespace

void MapView::showPanelTaxa()
{
    struct Group
    {
        qint64 key = 0;
        QString name;
        QString commonName;
        int count = 0;
    };
    QHash<qint64, Group> groups;
    for (int row : std::as_const(m_panelRows)) {
        const QModelIndex idx = m_model.index(row);
        const qint64 key = taxonKeyOf(idx);
        Group &g = groups[key];
        g.key = key;
        if (g.name.isEmpty()) {
            g.name = key > 0 ? idx.data(model::CaptureListModel::MatchedNameRole).toString()
                             : tr("Unidentified");
            g.commonName = idx.data(model::CaptureListModel::MatchedCommonNameRole).toString();
        }
        ++g.count;
    }

    // One taxon only: straight to its photos -- a list of one says nothing.
    if (groups.size() == 1) {
        showPanelPhotos(groups.cbegin().key());
        m_panelBack->hide();
        return;
    }

    QList<Group> sorted = groups.values();
    std::sort(sorted.begin(), sorted.end(), [](const Group &a, const Group &b) {
        if ((a.key == 0) != (b.key == 0))
            return b.key == 0;   // Unidentified last
        return a.count != b.count ? a.count > b.count
                                  : a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    m_panelTaxon.reset();
    m_panelBack->hide();
    setPanelTitle(tr("%n photo(s)", nullptr, int(m_panelRows.size())) + QStringLiteral(" · ")
                  + tr("%n taxa", nullptr, int(sorted.size())));
    m_panelList->clear();
    // Two lines per taxon: the scientific name (italic) and photo count, then
    // the common name, muted. Drawn by a label, since an item's own text has
    // one font for the whole row.
    const QColor muted = pl::themeColors(pl::currentThemeVariant()).mutedText;
    for (const Group &g : std::as_const(sorted)) {
        const QString name = g.name.toHtmlEscaped();
        QString html = QStringLiteral("%1 &nbsp;<span style=\"color:%2\">(%3)</span>")
                           .arg(g.key > 0 ? QStringLiteral("<i>%1</i>").arg(name) : name,
                                muted.name())
                           .arg(g.count);
        if (!g.commonName.isEmpty())
            html += QStringLiteral("<br><span style=\"color:%1\">%2</span>")
                        .arg(muted.name(), g.commonName.toHtmlEscaped());

        auto *item = new QListWidgetItem;
        item->setData(Qt::UserRole, g.key);
        // Plain text too, for keyboard search and accessibility.
        item->setData(Qt::AccessibleTextRole, g.commonName.isEmpty()
                                                  ? g.name
                                                  : g.name + QStringLiteral(", ") + g.commonName);
        auto *label = new QLabel(html);
        label->setTextFormat(Qt::RichText);
        label->setContentsMargins(4, 3, 4, 3);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);   // clicks reach the list
        item->setSizeHint(label->sizeHint());
        m_panelList->addItem(item);
        m_panelList->setItemWidget(item, label);
    }
    placePanel();
    m_panel->show();
    m_panel->raise();
    m_panelList->setFocus();
    m_panelList->setCurrentRow(0);
}

void MapView::showPanelPhotos(qint64 taxonKey)
{
    m_panelTaxon = taxonKey;
    m_panelBack->show();
    m_panelList->clear();
    QString name;
    for (int row : std::as_const(m_panelRows)) {
        const QModelIndex idx = m_model.index(row);
        if (taxonKeyOf(idx) != taxonKey)
            continue;
        if (name.isEmpty()) {
            name = taxonKey > 0 ? idx.data(model::CaptureListModel::MatchedNameRole).toString()
                                : tr("Unidentified");
            const QString common =
                idx.data(model::CaptureListModel::MatchedCommonNameRole).toString();
            if (!common.isEmpty())
                name += QStringLiteral(" (%1)").arg(common);
        }
        QString text = idx.data(model::CaptureListModel::DisplayNameRole).toString();
        const QString date = idx.data(model::CaptureListModel::CapturedOnRole).toString().left(10);
        if (!date.isEmpty())
            text += QLatin1Char('\n') + date;
        auto *item = new QListWidgetItem(squareThumb(idx), text);
        item->setData(Qt::UserRole, row);
        m_panelList->addItem(item);
    }
    setPanelTitle(QStringLiteral("%1 · %2").arg(
        name, tr("%n photo(s)", nullptr, m_panelList->count())));
    placePanel();
    m_panel->show();
    m_panel->raise();
    m_panelList->setFocus();
    m_panelList->setCurrentRow(0);
}

void MapView::onPanelItemActivated(QListWidgetItem *item)
{
    if (!item)
        return;
    if (!m_panelTaxon) {
        showPanelPhotos(item->data(Qt::UserRole).toLongLong());
        return;
    }
    // The panel stays open, so the next photo is one click away.
    emit captureActivated(m_model.index(item->data(Qt::UserRole).toInt()));
}

// Beside the clicked pin, kept inside the map.
void MapView::placePanel()
{
    const int w = std::min(340, width() - 16);
    const int rowH = m_panelTaxon ? 56 : 40;
    const int h = std::min(std::max(160, height() - 16),
                           std::min(420, 44 + rowH * int(m_panelList->count()) + 8));
    int x = m_panelAnchor.x() + 16;
    if (x + w > width() - 8)
        x = m_panelAnchor.x() - 16 - w;   // flip to the pin's left
    x = std::clamp(x, 8, std::max(8, width() - w - 8));
    const int y = std::clamp(m_panelAnchor.y() - 24, 8, std::max(8, height() - h - 8));
    m_panel->setGeometry(x, y, w, h);
    m_panel->layout()->activate();
    setPanelTitle(m_panelTitleText);   // re-elide for the new width
}

void MapView::setPanelTitle(const QString &text)
{
    m_panelTitleText = text;
    const int width = m_panelTitle->width();
    m_panelTitle->setText(width > 0 ? m_panelTitle->fontMetrics().elidedText(text, Qt::ElideRight,
                                                                              width)
                                    : text);
    m_panelTitle->setToolTip(text);
}

void MapView::closePanel()
{
    if (!m_panel->isVisible())
        return;
    m_panel->hide();
    m_panelList->clear();
    m_panelRows.clear();
    m_panelTaxon.reset();
    setFocus();
}

void MapView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_panel->isVisible()) {
        closePanel();
        return;
    }
    if (event->key() == Qt::Key_Backspace && m_panel->isVisible() && m_panelTaxon
        && m_panelBack->isVisible()) {
        showPanelTaxa();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace pl
