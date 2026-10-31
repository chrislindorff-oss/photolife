#include "ui/OrganiseView.h"

#include "app/Application.h"
#include "db/Database.h"
#include "geo/LocalityStore.h"
#include "match/CandidateFinder.h"
#include "net/GeocodeClient.h"
#include "organise/OrganiseMover.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"
#include "ui/ImageViewer.h"
#include "scan/FileScanner.h"
#include "ui/GeotagDialog.h"
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
#include <QPointer>
#include <QCheckBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QPromise>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTimer>
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
    NamingEndRole,
    HasGpsRole,
    BestRole,
    PlaceRole,      // the place's name; empty without GPS or while places are off
    PlaceStateRole, // a PlaceState
};

enum PlaceState { NoPlace, PlaceFinding, PlaceKnown, PlaceUnavailable };

// Photos within ~100 m share a place: the same rounding as the catalogue's
// geocode_cache.
QString placeKey(double lat, double lon)
{
    return QString::asprintf("%.3f,%.3f", lat, lon);
}

QString placeKey(const organise::Item &item)
{
    return item.hasGps() ? placeKey(*item.latitude, *item.longitude) : QString();
}

// "Town, State, Country" without the country, for a thumbnail.
QString shortPlace(const QString &place)
{
    QStringList parts = place.split(QStringLiteral(", "));
    if (parts.size() >= 3)
        parts.removeLast();
    return parts.join(QStringLiteral(", "));
}

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

    // Rows from..to have been added at the end, by `apply`.
    void appendRows(int from, int to, const std::function<void()> &apply)
    {
        beginInsertRows({}, from, to);
        apply();
        for (int i = from; i <= to && i < m_session.count(); ++i)
            m_rowByKey.insert(thumb::ThumbnailCache::keyFor({}, m_session.items().at(i).previewPath), i);
        endInsertRows();
    }

    // Places: looked up by coordinate, kept for the session (they don't
    // depend on the folder).
    bool showPlaces = false;
    QHash<QString, QString> places;   // placeKey -> "Town, State, Country" ("" = none there)
    QSet<QString> unavailable;        // lookups that failed this session

    PlaceState placeState(const organise::Item &item) const
    {
        if (!item.hasGps())
            return NoPlace;
        const QString key = placeKey(item);
        if (places.contains(key))
            return places.value(key).isEmpty() ? NoPlace : PlaceKnown;
        return unavailable.contains(key) ? PlaceUnavailable : PlaceFinding;
    }

    // Every photo at `key` has its place now.
    void placeChanged(const QString &key)
    {
        for (int i = 0; i < m_session.count(); ++i) {
            if (placeKey(m_session.items().at(i)) == key)
                emit dataChanged(index(i), index(i), {PlaceRole, PlaceStateRole, Qt::ToolTipRole});
        }
    }

    void allChanged()
    {
        if (m_session.count() > 0)
            emit dataChanged(index(0), index(m_session.count() - 1));
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
        case Qt::ToolTipRole: {
            QString tip = QStringList(item.files)
                              .replaceInStrings(QRegularExpression(QStringLiteral(".*/")), QString())
                              .join(QLatin1Char('\n'));
            if (item.hasGps()) {
                tip += QStringLiteral("\n") + QObject::tr("Taken at %1, %2")
                                                  .arg(*item.latitude, 0, 'f', 5)
                                                  .arg(*item.longitude, 0, 'f', 5);
                const QString place = places.value(placeKey(item));
                if (!place.isEmpty())
                    tip += QStringLiteral("\n") + place;
            }
            return tip;
        }
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
        case NamingEndRole:
            return m_session.isNamingEndAt(row);
        case HasGpsRole:
            return item.hasGps();
        case BestRole:
            return m_session.isBest(row);
        case PlaceRole:
            return showPlaces ? places.value(placeKey(item)) : QString();
        case PlaceStateRole:
            return showPlaces ? placeState(item) : NoPlace;
        default:
            return {};
        }
    }

private:
    organise::OrganiseSession &m_session;
    thumb::ThumbnailCache &m_thumbs;
    QHash<QString, int> m_rowByKey;
};

// A five-pointed star filling `box`.
QPolygonF starIn(const QRectF &box)
{
    constexpr double kPi = 3.14159265358979323846;
    const QPointF c = box.center();
    const qreal outer = std::min(box.width(), box.height()) / 2.0;
    const qreal inner = outer * 0.42;
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const qreal r = i % 2 == 0 ? outer : inner;
        const qreal a = -kPi / 2.0 + i * kPi / 5.0;
        star << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
    }
    return star;
}

// A framed photo: its run's band and name on top, the picture, then the file
// name and when it was taken; rejects crossed out.
class OrganiseDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize cell{260, 240};
    static constexpr int kBand = 24;
    static constexpr int kCaptionLine = 18;
    static constexpr int kMargin = 5;
    bool showPlaces = false;   // a third caption line, for the place

    // Name and kinds, the date, and the place when it's shown.
    int captionHeight() const { return 2 + kCaptionLine * (showPlaces ? 3 : 2); }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return cell; }

    static QRect frameRect(const QRect &cellRect) { return cellRect.adjusted(kMargin, kMargin, -kMargin, -kMargin); }
    QRect imageRect(const QRect &cellRect) const
    {
        const QRect f = frameRect(cellRect);
        return QRect(f.left() + 6, f.top() + kBand + 4, f.width() - 12, f.height() - kBand - captionHeight() - 8);
    }
    // The corner button that opens the photo full size.
    QRect zoomRect(const QRect &cellRect) const
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
            if (index.data(HasGpsRole).toBool())
                drawGeoPill(p, t, target, option.font);
            // A best shot: a gold star at the top right.
            if (index.data(BestRole).toBool()) {
                p->setPen(QPen(QColor(0, 0, 0, 120), 1.2));
                p->setBrush(t.bestShotGold);
                p->drawPolygon(starIn(QRectF(target.right() - 30, target.top() + 6, 24, 24)));
            }
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
        const QRect caption(frame.left() + 8, img.bottom() + 4, frame.width() - 16, captionHeight());
        const QString line1 = index.data(Qt::DisplayRole).toString() + QStringLiteral("  ·  ")
                              + index.data(KindsRole).toString()
                              + (rejected ? QObject::tr("  ·  reject") : QString())
                              + (index.data(BestRole).toBool() ? QObject::tr("  ·  ★ best shot") : QString())
                              + (index.data(NamingEndRole).toBool()
                                     ? QObject::tr("  ·  last of its taxon")
                                     : QString());
        const QDateTime taken = index.data(TakenRole).toDateTime();
        const QString line2 = QLocale().toString(taken, QLocale::ShortFormat);
        const QFontMetrics fm(small);
        p->drawText(caption.left(), caption.top() + fm.ascent(), fm.elidedText(line1, Qt::ElideRight, caption.width()));
        p->setPen(t.mutedText);
        p->drawText(caption.left(), caption.top() + fm.height() + fm.ascent(),
                    fm.elidedText(line2, Qt::ElideRight, caption.width()));
        if (showPlaces) {
            QString line3;
            switch (index.data(PlaceStateRole).toInt()) {
            case PlaceKnown:
                line3 = shortPlace(index.data(PlaceRole).toString());
                p->setPen(rejected ? t.mutedText : t.text);
                break;
            case PlaceFinding:
                line3 = QObject::tr("Finding place…");
                break;
            case PlaceUnavailable:
                line3 = QObject::tr("Place unavailable");
                break;
            default:
                break;
            }
            p->drawText(caption.left(), caption.top() + 2 * fm.height() + fm.ascent(),
                        fm.elidedText(line3, Qt::ElideRight, caption.width()));
        }
        p->restore();
    }

private:
    // "GEO" at the picture's bottom-left (⤢ has the right), styled like the
    // library grid's badges.
    static void drawGeoPill(QPainter *p, const ThemeColors &t, const QRect &picture, const QFont &base)
    {
        QFont font = base;
        font.setPointSize(7);
        font.setBold(true);
        const QFontMetrics fm(font);
        const QString text = QStringLiteral("GEO");
        constexpr int padH = 6, pillH = 16;
        const QRect pill(picture.left() + 5, picture.bottom() - pillH - 4, fm.horizontalAdvance(text) + 2 * padH, pillH);
        p->save();
        p->setFont(font);
        p->setPen(Qt::NoPen);
        p->setBrush(t.badgeBackground);
        p->drawRoundedRect(pill, pillH / 2.0, pillH / 2.0);
        p->setPen(t.badgeText);
        p->drawText(pill, Qt::AlignCenter, text);
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
    // Elided to fit (full path in its tooltip): a label sized to a long path
    // would make the window's minimum width wider than the screen, and the
    // window manager then can't maximise it.
    m_folderText = tr("No folder open");
    m_folderLabel = new QLabel(m_folderText, this);
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_folderLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_folderLabel->setMinimumWidth(80);
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
    m_placesButton = new QToolButton(this);
    m_placesButton->setText(tr("Places"));
    m_placesButton->setCheckable(true);
    m_placesButton->setToolTip(tr("Show where each photo with GPS was taken — the town or locality, "
                                  "looked up on OpenStreetMap for the photos in view. Photos with "
                                  "GPS are marked GEO."));
    connect(m_placesButton, &QToolButton::toggled, this, &OrganiseView::setShowPlaces);

    m_counts = new QLabel(this);
    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setTextVisible(true);
    m_scanProgress->setFormat(tr("Finding photos…"));
    m_scanProgress->setRange(0, 0);
    m_scanProgress->hide();
    m_commitButton = new QPushButton(tr("Organise into Folders…"), this);
    m_commitButton->setToolTip(tr("Move each named run of photos into a folder of its name inside "
                                  "this folder, and rejects into \"Rejects\". File names are kept; "
                                  "it can be undone."));
    connect(m_commitButton, &QPushButton::clicked, this, &OrganiseView::commit);
    m_undoButton = new QPushButton(tr("Undo Organise…"), this);
    connect(m_undoButton, &QPushButton::clicked, this, &OrganiseView::undoLast);

    auto *bar = new QHBoxLayout;
    bar->addWidget(open);
    bar->addWidget(m_folderLabel, 1);
    bar->addWidget(m_counts);
    bar->addSpacing(12);
    bar->addWidget(m_filmstripButton);
    bar->addWidget(m_gridButton);
    bar->addWidget(m_placesButton);
    bar->addSpacing(12);
    bar->addWidget(m_undoButton);
    bar->addWidget(m_commitButton);

    m_hint = new QLabel(tr("Click a photo to focus and preview it. N names it · R marks it as a "
                           "reject · B marks it as a best shot · G sets its location on a map · E marks it as the last photo of its taxon · V, a double-click or ⤢ shows it full size (there A is 1:1 and R rejects) · "
                           "right-click for actions. The photos after a named one go with it, "
                           "up to the next name or end. The wheel scrolls · F11 full screen."),
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
        const QPoint at = m_list->viewport()->mapFromGlobal(QCursor::pos());
        if (m_delegate->zoomRect(m_list->visualRect(index)).contains(at))
            showFullSize(index.row());
    });
    // A double-click opens the viewer, as V does (the first click of it has
    // already selected the photo; on the corner button it's already open).
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_list, &QListView::doubleClicked, this, [this](const QModelIndex &index) {
        const QPoint at = m_list->viewport()->mapFromGlobal(QCursor::pos());
        if (!m_delegate->zoomRect(m_list->visualRect(index)).contains(at))
            showFullSize(index.row());
    });
    connect(m_list, &QListView::customContextMenuRequested, this, &OrganiseView::contextMenuAt);

    // Under the filmstrip: the selected photo, large enough to tell what it is.
    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumHeight(120);
    // Ignored both ways: the photo is scaled to whatever space there is, and
    // must never ask for its own size (that would grow the window each time).
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_previewCaption = new QLabel(this);
    m_previewCaption->setAlignment(Qt::AlignCenter);
    m_previewCaption->setWordWrap(true);   // wraps rather than widening the window
    m_previewCaption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { updatePreview(); });
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this,
            [this](const QString &key, int px) {
                if (px == thumb::ThumbnailCache::kPreviewPx && key == m_previewKey)
                    updatePreview();
            });

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(bar);
    layout->addWidget(m_scanProgress);
    layout->addWidget(m_hint);
    layout->addWidget(m_list);
    layout->addWidget(m_preview, 1);
    layout->addWidget(m_previewCaption);

    // Places are looked up for the photos in view once it settles.
    m_placeScan = new QTimer(this);
    m_placeScan->setSingleShot(true);
    m_placeScan->setInterval(200);
    connect(m_placeScan, &QTimer::timeout, this, &OrganiseView::lookUpNextPlace);
    connect(m_list->horizontalScrollBar(), &QScrollBar::valueChanged, m_placeScan, qOverload<>(&QTimer::start));
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, m_placeScan, qOverload<>(&QTimer::start));
    // Nearing the end of the photos loaded: the next page.
    for (QScrollBar *bar : {m_list->horizontalScrollBar(), m_list->verticalScrollBar()}) {
        connect(bar, &QScrollBar::valueChanged, this, &OrganiseView::maybeLoadMore);
        connect(bar, &QScrollBar::rangeChanged, this, &OrganiseView::maybeLoadMore);
    }
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &OrganiseView::maybeLoadMore);

    setFilmstrip(true);
    m_placesButton->setChecked(m_app.settings().organiseShowPlaces());
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
    ++m_loadGeneration;
    m_commitWhenLoaded = false;
    m_app.settings().setOrganiseFolder(folder);
    m_folderText = QDir::toNativeSeparators(folder);
    m_folderLabel->setToolTip(m_folderText);
    elideFolderLabel();
    m_counts->setText(tr("Reading the photos…"));
    m_scanProgress->setRange(0, 0);
    m_scanProgress->setFormat(tr("Finding photos…"));
    m_scanProgress->show();
    m_commitButton->setEnabled(false);
    m_undoButton->setEnabled(false);
    // The shots are listed (quick), then the first page of them read.
    struct FirstPage
    {
        QList<organise::Item> read;
        QList<organise::Item> pending;
    };
    auto *watcher = new QFutureWatcher<FirstPage>(this);
    connect(watcher, &QFutureWatcher<FirstPage>::progressRangeChanged, this,
            [this](int minimum, int maximum) {
                m_scanProgress->setRange(minimum, maximum);
                m_scanProgress->setFormat(maximum == 0 ? tr("Finding photos…")
                                                       : tr("%v / %m photos"));
            });
    connect(watcher, &QFutureWatcher<FirstPage>::progressValueChanged,
            m_scanProgress, &QProgressBar::setValue);
    connect(watcher, &QFutureWatcher<FirstPage>::finished, this, [this, watcher, folder] {
        watcher->deleteLater();
        m_loading = false;
        m_scanProgress->hide();
        FirstPage page = watcher->result();
        m_session.setItems(folder, std::move(page.read), std::move(page.pending));
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
        m_placeScan->start();
        maybeLoadMore();
    });
    watcher->setFuture(QtConcurrent::run([folder](QPromise<FirstPage> &promise) {
        promise.setProgressRange(0, 0);
        FirstPage page;
        page.pending = organise::OrganiseSession::listFolder(folder);
        const int first = organise::OrganiseSession::firstPageSize(folder, page.pending);
        promise.setProgressRange(0, first);
        page.read = page.pending.mid(0, first);
        page.pending.remove(0, first);
        for (int i = 0; i < page.read.size(); ++i) {
            organise::OrganiseSession::readMetadata(page.read[i]);
            promise.setProgressValue(i + 1);
        }
        organise::OrganiseSession::sortByTaken(page.read);
        promise.addResult(std::move(page));
    }));
}

void OrganiseView::maybeLoadMore()
{
    if (m_loading || m_loadingMore || m_session.pendingCount() == 0)
        return;
    // Within a few screens of the end, or the focus on one of the last 50
    // photos: early enough that the page is usually there before you are.
    const QScrollBar *bar = m_filmstrip ? m_list->horizontalScrollBar() : m_list->verticalScrollBar();
    const bool nearEnd = bar->maximum() - bar->value() <= 3 * bar->pageStep();
    if (nearEnd || m_list->currentIndex().row() >= m_session.count() - 50)
        loadMore();
}

void OrganiseView::loadMore(bool all)
{
    if (m_loadingMore || m_session.pendingCount() == 0)
        return;
    m_loadingMore = true;
    const int generation = m_loadGeneration;
    QList<organise::Item> batch =
        m_session.pending().mid(0, all ? m_session.pendingCount() : organise::OrganiseSession::kPageSize);
    m_scanProgress->setRange(0, int(batch.size()));
    m_scanProgress->setValue(0);
    m_scanProgress->setFormat(all ? tr("Reading the rest of the photos before organising… %v / %m")
                                  : tr("Loading more photos… %v / %m"));
    m_scanProgress->show();
    auto *watcher = new QFutureWatcher<QList<organise::Item>>(this);
    connect(watcher, &QFutureWatcher<QList<organise::Item>>::progressValueChanged,
            m_scanProgress, &QProgressBar::setValue);
    connect(watcher, &QFutureWatcher<QList<organise::Item>>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_loadGeneration) {
            m_loadingMore = false;
            return;   // another folder (or this one again) has been opened since
        }
        m_scanProgress->hide();
        const QList<organise::Item> read = watcher->result();
        const int from = m_session.count();
        if (!read.isEmpty()) {
            m_model->appendRows(from, from + int(read.size()) - 1, [&] { m_session.appendItems(read); });
            m_model->runChanged(from - 1);   // the last run carries on into them
        }
        m_loadingMore = false;
        updateCounts();
        if (m_commitWhenLoaded) {
            if (m_session.pendingCount() > 0) {
                loadMore(true);
            } else {
                m_commitWhenLoaded = false;
                commit();
            }
            return;
        }
        m_placeScan->start();
        maybeLoadMore();
    });
    watcher->setFuture(QtConcurrent::run([batch](QPromise<QList<organise::Item>> &promise) mutable {
        promise.setProgressRange(0, int(batch.size()));
        for (int i = 0; i < batch.size(); ++i) {
            organise::OrganiseSession::readMetadata(batch[i]);
            promise.setProgressValue(i + 1);
        }
        promise.addResult(std::move(batch));
    }));
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
    m_placeScan->start();
}

void OrganiseView::setShowPlaces(bool on)
{
    m_placesButton->setChecked(on);
    m_app.settings().setOrganiseShowPlaces(on);
    m_model->showPlaces = on;
    m_delegate->showPlaces = on;
    updateCellSize();
    m_model->allChanged();
    updatePreview();
    if (on)
        m_placeScan->start();
}

QList<int> OrganiseView::visibleRows() const
{
    QList<int> rows;
    const QRect view = m_list->viewport()->rect();
    for (int i = 0; i < m_session.count(); ++i) {
        if (m_list->visualRect(m_model->index(i)).intersects(view))
            rows << i;
    }
    // The selected photo first, then outwards from it.
    const int current = m_list->currentIndex().row();
    if (current >= 0)
        std::stable_sort(rows.begin(), rows.end(),
                         [current](int a, int b) { return std::abs(a - current) < std::abs(b - current); });
    return rows;
}

void OrganiseView::lookUpNextPlace()
{
    if (!m_model->showPlaces || m_placeInFlight || !isVisible())
        return;
    geo::LocalityStore store(m_app.database().connectionName());
    // From what's in view now: anything scrolled away since isn't wanted.
    for (int row : visibleRows()) {
        const organise::Item &item = m_session.items().at(row);
        const QString key = placeKey(item);
        if (key.isEmpty() || m_model->places.contains(key) || m_model->unavailable.contains(key))
            continue;
        // Known from the library's photos: no need to ask.
        if (const std::optional<QString> cached = store.lookup(*item.latitude, *item.longitude)) {
            m_model->places.insert(key, *cached);
            m_model->placeChanged(key);
            if (row == m_list->currentIndex().row())
                updatePreview();
            continue;
        }
        // One at a time (Nominatim allows a request a second); then the next.
        m_placeInFlight = true;
        const double lat = *item.latitude, lon = *item.longitude;
        QPointer<OrganiseView> self(this);
        m_app.geocoder().reverseGeocode(lat, lon, [self, key, lat, lon](net::Outcome<QString> out) {
            if (!self)
                return;
            self->m_placeInFlight = false;
            if (out.ok()) {
                self->m_model->places.insert(key, out.value);
                // Kept in the catalogue too, when it may be changed, so the
                // library's map and filters know the place later.
                if (self->m_app.catalogueEditable())
                    geo::LocalityStore(self->m_app.database().connectionName()).upsertLocality(lat, lon, out.value);
            } else {
                self->m_model->unavailable.insert(key);
            }
            self->m_model->placeChanged(key);
            self->updatePreview();
            self->lookUpNextPlace();
        });
        return;
    }
}

void OrganiseView::updateCellSize()
{
    QSize cell;
    if (m_filmstrip) {
        // A band of 3:2 frames, about a third of the view's height.
        const int h = std::clamp(height() * 3 / 10, 200, 320);
        const int imageH = h - OrganiseDelegate::kBand - m_delegate->captionHeight() - 2 * OrganiseDelegate::kMargin - 12;
        cell = QSize(imageH * 3 / 2 + 2 * OrganiseDelegate::kMargin + 12, h);
        m_list->setFixedHeight(h + m_list->horizontalScrollBar()->sizeHint().height() + 2 * m_list->frameWidth() + 2);
    } else {
        cell = QSize(260, 240 + (m_delegate->showPlaces ? OrganiseDelegate::kCaptionLine : 0));
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
    elideFolderLabel();
    updateCellSize();
    updatePreview();
    m_placeScan->start();
}

void OrganiseView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_placeScan->start();   // places not looked up while away
}

void OrganiseView::elideFolderLabel()
{
    m_folderLabel->setText(m_folderLabel->fontMetrics().elidedText(
        m_folderText, Qt::ElideMiddle, std::max(40, m_folderLabel->width())));
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
    const QString place = m_model->showPlaces ? m_model->places.value(placeKey(item)) : QString();
    m_previewCaption->setText(
        QStringLiteral("<b>%1</b> · %2 · %3%4%5%6")
            .arg(item.baseName.toHtmlEscaped(), item.kinds.join(QLatin1Char('+')),
                 QLocale().toString(item.taken, QLocale::LongFormat).toHtmlEscaped(),
                 place.isEmpty() ? QString() : QStringLiteral(" · ") + place.toHtmlEscaped(),
                 run.isEmpty() ? tr(" · not named yet") : QStringLiteral(" · <b>%1</b>").arg(run.toHtmlEscaped()),
                 m_session.isRejected(row) ? tr(" · <span style='color:#c33'>reject</span>")
                 : m_session.isBest(row)   ? tr(" · ★ best shot")
                                           : QString()));
}

void OrganiseView::updateCounts()
{
    const bool open = !m_session.folder().isEmpty();
    // Say plainly what Undo covers: a whole organise, every folder of it.
    const organise::OrganiseMover::UndoPreview undo =
        open ? organise::OrganiseMover::previewUndo(m_session.folder()) : organise::OrganiseMover::UndoPreview{};
    m_undoButton->setEnabled(!undo.folders.isEmpty());
    m_undoButton->setToolTip(
        undo.folders.isEmpty()
            ? tr("Nothing to undo: no Organise into Folders has been done here.")
            : tr("Puts back everything the organise of %1 moved: %Ln photo(s) from %2 folder(s) — "
                 "not just one taxon. Shows the list before doing anything.",
                 nullptr, undo.shots)
                  .arg(QLocale().toString(undo.when, QLocale::ShortFormat))
                  .arg(undo.folders.size()));
    if (!open || m_session.count() == 0) {
        m_counts->setText(open ? tr("No photos in this folder") : QString());
        m_commitButton->setEnabled(false);
        return;
    }
    const organise::OrganisePlan plan = m_session.plan();
    const int total = m_session.count() + m_session.pendingCount();
    m_counts->setText((m_session.pendingCount() > 0
                           ? tr("%1 of %Ln photo(s) loaded — more as you scroll", nullptr, total)
                                 .arg(QLocale().toString(m_session.count()))
                           : tr("%Ln photo(s)", nullptr, m_session.count()))
                      + QStringLiteral(" · ")
                      + tr("%Ln taxa", nullptr, int(plan.folders.size())) + QStringLiteral(" · ")
                      + tr("%Ln reject(s)", nullptr, int(plan.rejects.size())) + QStringLiteral(" · ")
                      + tr("%Ln unnamed", nullptr, plan.unnamed)
                      + (m_session.bestShots().isEmpty()
                             ? QString()
                             : QStringLiteral(" · ") + tr("%Ln best", nullptr, int(m_session.bestShots().size()))));
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

void OrganiseView::toggleRejected(int row)
{
    const QModelIndexList rows = row >= 0 && row < m_session.count()
                                     ? QModelIndexList{m_model->index(row)}
                                     : m_list->selectionModel()->selectedIndexes();
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

void OrganiseView::toggleNamingEnd(int row)
{
    if (row < 0 || row >= m_session.count())
        return;
    const QString taxon = m_session.runName(row);
    if (taxon.isEmpty() && !m_session.isNamingEndAt(row)) {
        emit statusMessage(tr("This photo isn't named yet, so it can't be the last of a taxon."), 6000);
        return;
    }
    m_session.setNamingEndAt(row, !m_session.isNamingEndAt(row));
    const QString photo = m_session.items().at(row).baseName;
    emit statusMessage(m_session.isNamingEndAt(row)
                           ? tr("%1 is the last photo of \u201c%2\u201d — the photos after it are "
                                "unnamed until you name the next taxon.").arg(photo, taxon)
                           : tr("%1 is no longer the last photo of \u201c%2\u201d — the name "
                                "carries on to the next named photo.").arg(photo, taxon),
                       8000);
    changed(row);
}

void OrganiseView::toggleBest()
{
    const QModelIndexList rows = m_list->selectionModel()->selectedIndexes();
    if (rows.isEmpty())
        return;
    // All best shots already: unmark; otherwise mark them all.
    bool all = true;
    for (const QModelIndex &i : rows)
        all = all && m_session.isBest(i.row());
    int first = m_session.count();
    for (const QModelIndex &i : rows) {
        m_session.setBest(i.row(), !all);
        first = std::min(first, i.row());
    }
    emit statusMessage(all ? tr("No longer a best shot.", nullptr, int(rows.size()))
                           : tr("%n best shot(s) — rated 5 stars when organised, so the library "
                                "takes them as Best Shots when you add them.", nullptr, int(rows.size())),
                       6000);
    changed(first);
}

ImageViewer::Item OrganiseView::viewerItem(int row) const
{
    const organise::Item &it = m_session.items().at(row);
    ImageViewer::Item v;
    v.path = it.previewPath;
    v.title = it.baseName;
    const QString run = m_session.runName(row);
    const QString place = m_model->showPlaces ? m_model->places.value(placeKey(it)) : QString();
    v.caption = it.baseName + QStringLiteral(" · ") + it.kinds.join(QLatin1Char('+'))
                + QStringLiteral(" · ") + QLocale().toString(it.taken, QLocale::ShortFormat)
                + (place.isEmpty() ? QString() : QStringLiteral(" · ") + place)
                + (run.isEmpty() ? QString() : QStringLiteral("\n") + run)
                + (m_session.isBest(row) ? tr(" · ★ best shot") : QString());
    v.canReject = true;
    v.rejected = m_session.isRejected(row);
    return v;
}

void OrganiseView::setLocationOnMap()
{
    QList<int> rows;
    for (const QModelIndex &i : m_list->selectionModel()->selectedIndexes())
        rows << i.row();
    std::sort(rows.begin(), rows.end());
    if (rows.isEmpty())
        return;
    QList<GeotagDialog::Photo> photos;
    QSet<int> chosen;
    QDateTime from, to;
    for (int r : std::as_const(rows)) {
        const organise::Item &it = m_session.items().at(r);
        GeotagDialog::Photo p;
        p.path = it.previewPath;
        p.label = it.baseName;
        p.taken = it.taken;
        p.latitude = it.latitude;
        p.longitude = it.longitude;
        p.isRaw = std::any_of(it.files.cbegin(), it.files.cend(), [](const QString &f) {
            return scan::FileScanner::rawExtensions().contains(QFileInfo(f).suffix().toLower());
        });
        photos << p;
        chosen.insert(r);
        from = from.isValid() ? std::min(from, it.taken) : it.taken;
        to = to.isValid() ? std::max(to, it.taken) : it.taken;
    }
    // Clues: the other photos with a location taken within a few hours.
    QList<GeotagDialog::Clue> clues;
    for (int i = 0; i < m_session.count(); ++i) {
        const organise::Item &it = m_session.items().at(i);
        if (chosen.contains(i) || !it.hasGps())
            continue;
        if (from.isValid() && (it.taken < from.addSecs(-6 * 3600) || it.taken > to.addSecs(6 * 3600)))
            continue;
        clues << GeotagDialog::Clue{*it.latitude, *it.longitude, it.baseName, it.taken};
    }

    GeotagDialog dialog(m_app, photos, clues, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList failures;
    int set = 0;
    for (int r : std::as_const(rows)) {
        if (m_session.items().at(r).hasGps() && !dialog.replaceExisting())
            continue;
        const geo::GeotagWriter::Result result =
            m_session.setLocation(r, dialog.latitude(), dialog.longitude(), dialog.writeIntoRaw());
        failures << result.failures;
        ++set;
        m_model->runChanged(r);
    }
    QApplication::restoreOverrideCursor();
    if (!failures.isEmpty())
        QMessageBox::warning(this, tr("Set Location on Map"),
                             tr("The location couldn't be written into %n file(s):\n\n%1", nullptr,
                                int(failures.size()))
                                 .arg(failures.mid(0, 10).join(QLatin1Char('\n'))));
    emit statusMessage(tr("Set the location of %n photo(s).", nullptr, set), 6000);
    updatePreview();
    m_placeScan->start();
}

void OrganiseView::showFullSize(int row)
{
    if (row < 0 || row >= m_session.count())
        return;
    QVector<ImageViewer::Item> items;
    for (int i = 0; i < m_session.count(); ++i)
        items << viewerItem(i);
    if (!m_viewer) {
        m_viewer = new ImageViewer(window());
        // R in the viewer: reject it here too, and keep the filmstrip on it.
        connect(m_viewer, &ImageViewer::rejectToggleRequested, this, [this](int i, bool reject) {
            if (i < 0 || i >= m_session.count())
                return;
            m_session.setRejected(i, reject);
            changed(i);
            m_viewer->updateItem(i, viewerItem(i));
            const QModelIndex at = m_model->index(i);
            m_list->setCurrentIndex(at);
            m_list->scrollTo(at);
        });
    }
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
        m_list->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    else
        m_list->setCurrentIndex(index);
    const int row = index.row();
    QMenu menu(this);
    menu.addAction(m_session.nameAt(row).isEmpty() ? tr("Name This Photo…") : tr("Rename…"), this,
                   [this, row] { nameAt(row); });
    // The names given so far this session, most recent first: one click (or
    // its number) names this photo.
    const QStringList recent = m_session.recentNames().mid(0, 15);
    if (!recent.isEmpty()) {
        QMenu *nameAs = menu.addMenu(tr("Name As"));
        const QString current = m_session.runName(row);
        for (int i = 0; i < recent.size(); ++i) {
            const QString &name = recent.at(i);
            // "&1 Scarlet Robin": the number picks it; a name's own '&' stays one.
            const QString label = (i < 9 ? QStringLiteral("&%1  ").arg(i + 1) : QStringLiteral("    "))
                                  + QString(name).replace(QLatin1Char('&'), QStringLiteral("&&"));
            QAction *a = nameAs->addAction(label, this, [this, row, name] {
                m_session.setName(row, name);
                changed(row);
            });
            a->setCheckable(true);
            a->setChecked(name == current);
        }
    }
    if (!m_session.nameAt(row).isEmpty())
        menu.addAction(tr("Clear Name"), this, [this, row] {
            m_session.setName(row, QString());
            changed(row);
        });
    menu.addAction(m_session.isRejected(row) ? tr("Not a Reject") : tr("Reject"), this,
                   [this, row] { toggleRejected(row); });
    QAction *best = menu.addAction(tr("Best Shot"), this, [this] { toggleBest(); });
    best->setCheckable(true);
    best->setChecked(m_session.isBest(row));
    best->setShortcut(Qt::Key_B);
    best->setToolTip(tr("Rated 5 stars when organised, so the library takes it as a Best Shot "
                        "of its species when you add it."));
    // Named for the taxon it closes, so it's clear the mark goes on its LAST
    // photo (not on the first photo of the next one).
    const QString taxon = m_session.runName(row);
    QAction *last = menu.addAction(
        m_session.isNamingEndAt(row)
            ? tr("Not the Last Photo of \u201c%1\u201d").arg(taxon)
            : taxon.isEmpty() ? tr("Mark as Last Photo of Its Taxon")
                              : tr("Mark as Last Photo of \u201c%1\u201d").arg(taxon),
        this, [this, row] { toggleNamingEnd(row); });
    last->setEnabled(!taxon.isEmpty() || m_session.isNamingEndAt(row));
    last->setShortcut(Qt::Key_E);
    last->setToolTip(tr("This photo keeps the name; the photos after it are left unnamed until "
                        "you name the next taxon."));
    QAction *locate = menu.addAction(tr("Set Location on Map…"), this, [this] { setLocationOnMap(); });
    locate->setShortcut(Qt::Key_G);
    locate->setToolTip(tr("Put a pin on a map where the selected photos were taken, or move it: "
                          "written into the JPEG, and an .xmp sidecar beside a RAW."));
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
        case Qt::Key_N:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            nameAt(row);
            return true;
        case Qt::Key_R:
            if (row >= 0)
                toggleRejected(row);
            return true;
        case Qt::Key_E:
            toggleNamingEnd(row);
            return true;
        case Qt::Key_B:
            toggleBest();
            return true;
        case Qt::Key_G:
            setLocationOnMap();
            return true;
        case Qt::Key_Delete:
            toggleRejected();
            return true;
        case Qt::Key_V:
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
    // The last name carries on into photos not loaded yet: read them all first.
    if (m_session.pendingCount() > 0 || m_loadingMore) {
        m_commitWhenLoaded = true;
        loadMore(true);
        return;
    }
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
    const int bestCount = int(m_session.bestShots().size());
    const QString unnamedNote =
        (plan.unnamed > 0 ? tr("%Ln photo(s) before the first name stay where they are. ", nullptr, plan.unnamed)
                          : QString())
        + (bestCount > 0 ? tr("%Ln best shot(s) are rated 5 stars (in the JPEG, or an .xmp sidecar "
                              "beside a RAW) so the library takes them as Best Shots. ",
                              nullptr, bestCount)
                         : QString());
    box.setInformativeText(unnamedNote
                           + tr("File names are kept, RAW and JPEG and sidecar files move together, "
                                "nothing is overwritten, and Undo Organise puts all of it back."));
    box.setDetailedText(lines.join(QLatin1Char('\n')));

    // Renaming too: "Scarlet Robin - Anakie, Victoria, 2025-10-18".
    QList<int> named;   // the photos going into named folders
    for (const organise::FolderMove &f : plan.folders)
        named += f.items;
    const QString keptInfo = box.informativeText();
    auto *rename = new QCheckBox(&box);
    if (!plan.folders.isEmpty()) {
        const organise::FolderMove &first = plan.folders.first();
        const organise::Item &it = m_session.items().at(first.items.first());
        const QString known = shortPlace(m_model->places.value(placeKey(it)));
        const QString example = organise::OrganiseSession::renamedBase(
            first.folderName, it.hasGps() && known.isEmpty() ? tr("Town, State") : known, it.taken);
        rename->setText(tr("Also rename the photos, e.g. \u201c%1\u201d").arg(example));
        rename->setToolTip(tr("Named for their folder, where they were taken (from their GPS) and "
                              "the date. RAW, JPEG and sidecar files keep their extensions; Undo "
                              "Organise puts the old names back."));
        rename->setChecked(m_app.settings().organiseRenameOnOrganise());
        box.setCheckBox(rename);
        const int toFind = int(placesToFind(named).size());
        const auto explain = [&box, keptInfo, unnamedNote, toFind, this](bool on) {
            if (!on) {
                box.setInformativeText(keptInfo);
                return;
            }
            QString text = unnamedNote
                           + tr("The photos in named folders are renamed (rejects keep their names); "
                                "RAW and JPEG and sidecar files move together, nothing is "
                                "overwritten, and Undo Organise puts all of it back, old names too.");
            if (toFind > 0)
                text += QStringLiteral("\n\n")
                        + tr("Where %Ln spot(s) are will be looked up on OpenStreetMap first "
                             "(about a second each; needs the internet).",
                             nullptr, toFind);
            box.setInformativeText(text);
        };
        connect(rename, &QCheckBox::toggled, &box, explain);
        explain(rename->isChecked());
    }
    QPushButton *go = box.addButton(tr("Organise"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(go);
    box.exec();
    if (box.clickedButton() != go)
        return;
    const bool renaming = !plan.folders.isEmpty() && rename->isChecked();
    if (!plan.folders.isEmpty())
        m_app.settings().setOrganiseRenameOnOrganise(renaming);
    if (renaming) {
        // The places first; then on, unless cancelled.
        findPlacesThen(named, [this, plan] { executePlan(plan, true); });
        return;
    }
    executePlan(plan, false);
}

QSet<QString> OrganiseView::placesToFind(const QList<int> &rows) const
{
    // Not yet known here or from the library's photos.
    geo::LocalityStore store(m_app.database().connectionName());
    QSet<QString> keys;
    for (int row : rows) {
        const organise::Item &item = m_session.items().at(row);
        const QString key = placeKey(item);
        if (!key.isEmpty() && !keys.contains(key) && !m_model->places.contains(key)
            && !m_model->unavailable.contains(key) && !store.lookup(*item.latitude, *item.longitude))
            keys.insert(key);
    }
    return keys;
}

void OrganiseView::findPlacesThen(const QList<int> &rows, std::function<void()> then)
{
    // Known from the library's photos: no need to ask.
    geo::LocalityStore store(m_app.database().connectionName());
    QList<QPair<double, double>> pending;
    QSet<QString> queued;
    for (int row : rows) {
        const organise::Item &item = m_session.items().at(row);
        const QString key = placeKey(item);
        if (key.isEmpty() || queued.contains(key) || m_model->places.contains(key)
            || m_model->unavailable.contains(key))
            continue;
        queued.insert(key);
        if (const std::optional<QString> cached = store.lookup(*item.latitude, *item.longitude)) {
            m_model->places.insert(key, *cached);
            m_model->placeChanged(key);
        } else {
            pending.append({*item.latitude, *item.longitude});
        }
    }
    if (pending.isEmpty()) {
        then();
        return;
    }

    auto *progress = new QProgressDialog(tr("Finding where the photos were taken…"), tr("Cancel"), 0,
                                         int(pending.size()), this);
    progress->setWindowTitle(tr("Organise into Folders"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    // One at a time (Nominatim allows a request a second), each answer
    // starting the next; Cancel (seen when the current answer comes back)
    // stops the organise with nothing moved.
    auto step = std::make_shared<std::function<void(int)>>();
    QPointer<OrganiseView> self(this);
    QPointer<QProgressDialog> dialog(progress);
    *step = [self, dialog, pending, then, step](int at) {
        if (!self || !dialog)
            return;
        if (dialog->wasCanceled()) {
            dialog->deleteLater();
            emit self->statusMessage(tr("Organise cancelled — nothing was moved."), 6000);
            return;
        }
        if (at >= pending.size()) {
            dialog->deleteLater();
            then();
            return;
        }
        dialog->setLabelText(tr("Finding where the photos were taken: %1 of %2…")
                                 .arg(at + 1)
                                 .arg(pending.size()));
        const double lat = pending.at(at).first, lon = pending.at(at).second;
        self->m_app.geocoder().reverseGeocode(lat, lon, [self, dialog, step, at, lat, lon](net::Outcome<QString> out) {
            if (!self)
                return;
            const QString key = placeKey(lat, lon);
            if (out.ok()) {
                self->m_model->places.insert(key, out.value);
                if (self->m_app.catalogueEditable())
                    geo::LocalityStore(self->m_app.database().connectionName()).upsertLocality(lat, lon, out.value);
            } else {
                self->m_model->unavailable.insert(key);
            }
            self->m_model->placeChanged(key);
            if (dialog)
                dialog->setValue(at + 1);
            (*step)(at + 1);
        });
    };
    (*step)(0);
}

void OrganiseView::executePlan(const organise::OrganisePlan &plan, bool rename)
{
    // Best shots first: rated 5 stars, so the library takes them as Best
    // Shots. A new sidecar goes with its photo.
    QHash<int, QString> sidecars;
    QStringList ratingFailures;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (int i : m_session.bestShots()) {
        QString created, error;
        if (!organise::OrganiseSession::rateFiveStars(m_session.items().at(i), &created, &error))
            ratingFailures << QStringLiteral("%1: %2").arg(m_session.items().at(i).baseName, error);
        if (!created.isEmpty())
            sidecars.insert(i, created);
        m_session.refreshFileStats(i);   // still recognised after an undo
    }
    QApplication::restoreOverrideCursor();
    if (!m_session.bestShots().isEmpty())
        m_session.saveProgress();
    if (!ratingFailures.isEmpty())
        QMessageBox::warning(this, tr("Organise into Folders"),
                             tr("%Ln best shot(s) couldn't be rated, so the library won't know them "
                                "as best shots (star them there instead):\n\n%1",
                                nullptr, int(ratingFailures.size()))
                                 .arg(ratingFailures.mid(0, 10).join(QLatin1Char('\n'))));

    QList<organise::ShotMove> moves;
    int placeless = 0;   // renamed without a place: GPS, but it couldn't be found
    for (const organise::FolderMove &f : plan.folders) {
        for (int i : f.items) {
            const organise::Item &it = m_session.items().at(i);
            organise::ShotMove move{it.baseName, it.files, f.folderName, {}};
            if (sidecars.contains(i))
                move.files << sidecars.value(i);
            if (rename) {
                const QString place = shortPlace(m_model->places.value(placeKey(it)));
                if (it.hasGps() && m_model->unavailable.contains(placeKey(it)))
                    ++placeless;
                move.newBaseName = organise::OrganiseSession::renamedBase(f.folderName, place, it.taken);
            }
            moves.append(move);
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
    emit statusMessage(
        (rename ? tr("Organised and renamed %Ln photo(s).", nullptr, result.moved)
                : tr("Organised %Ln photo(s) into folders.", nullptr, result.moved))
            + (placeless > 0 ? QStringLiteral(" ")
                                   + tr("%Ln couldn't have their place found, so their names have "
                                        "just the date.", nullptr, placeless)
                             : QString()),
        10000);
    emit organised(folder);
    openFolder(folder);
}

void OrganiseView::undoLast()
{
    const QString folder = m_session.folder();
    const organise::OrganiseMover::UndoPreview preview = organise::OrganiseMover::previewUndo(folder);
    if (folder.isEmpty() || preview.folders.isEmpty()) {
        QMessageBox::information(this, tr("Undo Organise"),
                                 tr("There's no Organise into Folders to undo in this folder."));
        return;
    }
    // Exactly what goes back: every folder of that organise.
    QString list = QStringLiteral("<ul>");
    for (const auto &f : preview.folders)
        list += QStringLiteral("<li><b>%1</b> — %2</li>")
                    .arg(f.name.toHtmlEscaped(), tr("%Ln photo(s)", nullptr, f.shots));
    list += QStringLiteral("</ul>");
    QString text = tr("<p>This undoes the whole <b>Organise into Folders</b> of %1 — every folder "
                      "it made, not just the last taxon named:</p>%2"
                      "<p>%Ln photo(s) go back into <i>%3</i> under their original file names, "
                      "keeping their names and rejects, and the folders are removed once empty.</p>",
                      nullptr, preview.shots)
                       .arg(QLocale().toString(preview.when, QLocale::LongFormat).toHtmlEscaped(), list,
                            QDir::toNativeSeparators(folder).toHtmlEscaped());
    if (preview.missing > 0)
        text += tr("<p><b>%Ln file(s)</b> have been moved or renamed since, and will be left where "
                   "they are.</p>",
                   nullptr, preview.missing);
    if (preview.earlier > 0)
        text += tr("<p>An earlier organise (%Ln in all) can be undone after this one.</p>", nullptr,
                   preview.earlier);
    QMessageBox box(QMessageBox::Question, tr("Undo Organise"), text, QMessageBox::NoButton, this);
    box.setTextFormat(Qt::RichText);
    QPushButton *go = box.addButton(tr("Put Them All Back"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;
    const organise::OrganiseMover::Result result = organise::OrganiseMover::undo(folder);
    if (!result.failures.isEmpty())
        QMessageBox::warning(this, tr("Undo Organise"),
                             tr("%Ln file(s) couldn't be moved back and were left where they "
                                "are:\n\n%1",
                                nullptr, int(result.failures.size()))
                                 .arg(result.failures.mid(0, 10).join(QLatin1Char('\n'))));
    emit statusMessage(tr("Moved %Ln photo(s) back.", nullptr, result.moved), 8000);
    emit organised(folder);
    openFolder(folder);
}

} // namespace pl
