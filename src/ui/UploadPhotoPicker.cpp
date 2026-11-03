#include "ui/UploadPhotoPicker.h"

#include "app/Application.h"
#include "db/Database.h"
#include "scan/FileScanner.h"
#include "thumb/ThumbnailCache.h"

#include <QCheckBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace pl {

namespace {

constexpr int kIconPx = 128;

// `text` taken literally, as part of a LIKE pattern.
QString escapedForLike(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return escaped;
}

// `text` as a LIKE pattern matching it anywhere, its own % and _ taken literally.
QString containing(const QString &text)
{
    return QLatin1Char('%') + escapedForLike(text.toLower()) + QLatin1Char('%');
}

// Everything under folder `path`, as a LIKE pattern.
QString under(const QString &path)
{
    return escapedForLike(path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/')) + QLatin1Char('%');
}

// A full path typed or chosen, rather than part of a name.
bool isFullPath(const QString &text)
{
    return !text.isEmpty() && QDir::isAbsolutePath(text);
}

// The files the catalogue has under `root`.
QSet<QString> catalogued(const QString &connectionName, const QString &root)
{
    QSet<QString> known;
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT path FROM rendition WHERE path LIKE ? ESCAPE '\\'"));
    q.addBindValue(under(root));
    if (q.exec())
        while (q.next())
            known.insert(q.value(0).toString());
    return known;
}

// How many photo files under `root` aren't in `known`, skipping what a
// library scan skips. Names only, no catalogue: any thread.
int countNotIn(const QString &root, bool subfolders, const QSet<QString> &known)
{
    int n = 0;
    QStringList dirs{root};
    while (!dirs.isEmpty()) {
        const QDir dir(dirs.takeFirst());
        for (const QFileInfo &info : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (info.isDir()) {
                if (subfolders && !info.isSymLink() && !scan::FileScanner::isIgnoredDir(info.fileName()))
                    dirs << info.filePath();
                continue;
            }
            const QString ext = info.suffix().toLower();
            if (scan::FileScanner::isIgnoredFile(info.fileName())
                || (!scan::FileScanner::imageExtensions().contains(ext)
                    && !scan::FileScanner::rawExtensions().contains(ext)))
                continue;
            n += known.contains(info.filePath()) ? 0 : 1;
        }
    }
    return n;
}

} // namespace

QList<UploadPhotoPicker::Found> UploadPhotoPicker::find(const QString &connectionName, const Filter &filter)
{
    QStringList clauses;
    QVariantList binds;
    if (filter.from.isValid()) {
        clauses << QStringLiteral("substr(c.captured_on, 1, 10) >= ?");
        binds << filter.from.toString(Qt::ISODate);
    }
    if (filter.to.isValid()) {
        clauses << QStringLiteral("substr(c.captured_on, 1, 10) <= ?");
        binds << filter.to.toString(Qt::ISODate);
    }
    const QString folder = filter.folder.trimmed();
    if (isFullPath(folder)) {
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(folder));
        if (filter.subfolders) {
            clauses << QStringLiteral("(f.path = ? OR f.path LIKE ? ESCAPE '\\')");
            binds << path << under(path);
        } else {
            clauses << QStringLiteral("f.path = ?");
            binds << path;
        }
    } else if (!folder.isEmpty()) {
        clauses << QStringLiteral("LOWER(f.path) LIKE ? ESCAPE '\\'");
        binds << containing(folder);
    }
    if (!filter.species.trimmed().isEmpty()) {
        clauses << QStringLiteral("(LOWER(t.name) LIKE ? ESCAPE '\\' OR LOWER(t.common_name) LIKE ? ESCAPE '\\')");
        binds << containing(filter.species.trimmed()) << containing(filter.species.trimmed());
    }
    if (filter.notOnInat)
        clauses << QStringLiteral("c.inat_observation_id IS NULL");

    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
                  "SELECT c.id, "
                  "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id ORDER BY (r.kind = 'raw'), r.id LIMIT 1) AS path, "
                  "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
                  "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
                  "  t.name, t.common_name, c.inat_observation_id "
                  "FROM capture c JOIN folder f ON f.id = c.folder_id "
                  "LEFT JOIN capture_match m ON m.id = (SELECT id FROM capture_match WHERE capture_id = c.id "
                  "  AND status <> 'rejected' AND taxon_id IS NOT NULL "
                  "  ORDER BY (status = 'confirmed') DESC, (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
                  "LEFT JOIN taxon t ON t.id = m.taxon_id")
              + (clauses.isEmpty() ? QString() : QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND ")))
              + QStringLiteral(" ORDER BY (c.captured_on IS NULL), c.captured_on DESC, c.id DESC LIMIT %1").arg(filter.limit));
    for (const QVariant &b : std::as_const(binds))
        q.addBindValue(b);
    QList<Found> out;
    if (!q.exec())
        return out;
    while (q.next()) {
        Found f;
        f.captureId = q.value(0).toInt();
        f.path = q.value(1).toString();
        if (f.path.isEmpty())
            continue;   // no file to send
        f.contentHash = q.value(2).toString();
        f.label = QFileInfo(f.path).fileName();
        f.species = q.value(4).toString().isEmpty() ? q.value(3).toString() : q.value(4).toString();
        f.onInat = !q.value(5).isNull();
        out << f;
    }
    return out;
}

int UploadPhotoPicker::countUncatalogued(const QString &connectionName, const QString &folder, bool subfolders)
{
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    if (!QFileInfo(root).isDir())
        return 0;
    return countNotIn(root, subfolders, catalogued(connectionName, root));
}

UploadPhotoPicker::UploadPhotoPicker(Application &app, QWidget *parent)
    : QDialog(parent), m_app(app)
{
    setWindowTitle(tr("Add Photos to Upload"));
    resize(900, 640);

    m_useDates = new QCheckBox(tr("Taken between"), this);
    m_from = new QDateEdit(QDate::currentDate().addMonths(-1), this);
    m_to = new QDateEdit(QDate::currentDate(), this);
    for (QDateEdit *edit : {m_from, m_to}) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("d MMM yyyy"));
        edit->setEnabled(false);
    }
    connect(m_useDates, &QCheckBox::toggled, m_from, &QWidget::setEnabled);
    connect(m_useDates, &QCheckBox::toggled, m_to, &QWidget::setEnabled);
    auto *dates = new QHBoxLayout;
    dates->addWidget(m_useDates);
    dates->addWidget(m_from);
    dates->addWidget(new QLabel(tr("and"), this));
    dates->addWidget(m_to);
    dates->addStretch(1);

    m_folder = new QLineEdit(this);
    m_folder->setPlaceholderText(tr("Any folder — type part of its name, or Browse for one"));
    m_folder->setClearButtonEnabled(true);
    auto *browse = new QPushButton(tr("Browse…"), this);
    browse->setToolTip(tr("Choose a folder on your drive, such as one of new photos."));
    connect(browse, &QPushButton::clicked, this, &UploadPhotoPicker::browse);
    m_subfolders = new QCheckBox(tr("Include subfolders"), this);
    m_subfolders->setChecked(true);
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    folderRow->addWidget(m_subfolders);

    m_newBanner = new QWidget(this);
    m_newText = new QLabel(m_newBanner);
    m_newText->setWordWrap(true);
    m_addFolderButton = new QPushButton(tr("Add Folder to Library"), m_newBanner);
    m_addFolderButton->setToolTip(tr("Scans the folder into the library, as Library → Add Folder to Library does, "
                                     "then matches its new photos to species by their names, as Match Library "
                                     "does. Only library photos can be uploaded, so PhotoLife remembers which were. "
                                     "Your files aren't moved or changed."));
    connect(m_addFolderButton, &QPushButton::clicked, this, &UploadPhotoPicker::addFolderToLibrary);
    auto *bannerLayout = new QHBoxLayout(m_newBanner);
    bannerLayout->setContentsMargins(0, 0, 0, 0);
    bannerLayout->addWidget(m_newText, 1);
    bannerLayout->addWidget(m_addFolderButton);
    m_newBanner->setVisible(false);
    m_species = new QLineEdit(this);
    m_species->setPlaceholderText(tr("Any species — type part of a scientific or common name"));
    m_notOnInat = new QCheckBox(tr("Only photos not yet on iNaturalist"), this);
    m_notOnInat->setChecked(true);

    auto *form = new QFormLayout;
    form->addRow(dates);
    form->addRow(tr("Folder:"), folderRow);
    form->addRow(m_newBanner);
    form->addRow(tr("Species:"), m_species);
    form->addRow(m_notOnInat);

    m_list = new QListWidget(this);
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(kIconPx, kIconPx));
    m_list->setGridSize(QSize(kIconPx + 24, kIconPx + 40));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &UploadPhotoPicker::updateAdd);
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, &UploadPhotoPicker::fillVisibleIcons);
    connect(m_list->verticalScrollBar(), &QScrollBar::rangeChanged, this, &UploadPhotoPicker::fillVisibleIcons);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        if (!m_list->selectedItems().isEmpty())
            accept();
    });

    m_count = new QLabel(this);
    auto *selectAll = new QPushButton(tr("Select All"), this);
    connect(selectAll, &QPushButton::clicked, m_list, &QListWidget::selectAll);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_add = buttons->addButton(tr("Add"), QDialogButtonBox::AcceptRole);
    m_add->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_count, 1);
    bottom->addWidget(selectAll);
    bottom->addWidget(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_list, 1);
    layout->addLayout(bottom);

    // Searched as the filters change, a moment after typing stops.
    m_searchSoon = new QTimer(this);
    m_searchSoon->setSingleShot(true);
    m_searchSoon->setInterval(300);
    connect(m_searchSoon, &QTimer::timeout, this, &UploadPhotoPicker::search);
    auto soon = [this] { m_searchSoon->start(); };
    connect(m_useDates, &QCheckBox::toggled, this, soon);
    connect(m_from, &QDateEdit::dateChanged, this, soon);
    connect(m_to, &QDateEdit::dateChanged, this, soon);
    connect(m_folder, &QLineEdit::textChanged, this, soon);
    connect(m_subfolders, &QCheckBox::toggled, this, soon);
    connect(m_species, &QLineEdit::textChanged, this, soon);
    connect(m_notOnInat, &QCheckBox::toggled, this, soon);

    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, [this](const QString &hash, int px) {
        if (px != thumb::ThumbnailCache::kGridPx)
            return;
        if (QListWidgetItem *item = m_byHash.value(hash)) {
            const QPixmap pixmap = m_app.thumbnails().thumbnail(item->data(Qt::UserRole + 2).toString(),
                                                                item->data(Qt::UserRole + 1).toString(), px);
            if (!pixmap.isNull())
                item->setIcon(QIcon(pixmap));
        }
    });

    search();
}

void UploadPhotoPicker::search()
{
    Filter filter;
    if (m_useDates->isChecked()) {
        filter.from = m_from->date();
        filter.to = m_to->date();
    }
    filter.folder = m_folder->text();
    filter.subfolders = m_subfolders->isChecked();
    filter.species = m_species->text();
    filter.notOnInat = m_notOnInat->isChecked();
    const QList<Found> found = find(m_app.database().connectionName(), filter);

    m_list->clear();
    m_byHash.clear();
    for (const Found &f : found) {
        auto *item = new QListWidgetItem(f.species.isEmpty() ? f.label : f.species, m_list);
        item->setData(Qt::UserRole, f.captureId);
        item->setData(Qt::UserRole + 1, f.path);
        item->setData(Qt::UserRole + 2, f.contentHash);
        item->setToolTip(f.onInat ? tr("%1\nAlready on iNaturalist").arg(f.label) : f.label);
        m_byHash.insert(thumb::ThumbnailCache::keyFor(f.contentHash, f.path), item);
    }
    QTimer::singleShot(0, this, &UploadPhotoPicker::fillVisibleIcons);
    // Just added to the library: its new photos, selected.
    if (!m_beforeScan.isEmpty() || m_addFolderButton->property("scanned").toBool()) {
        for (int i = 0; i < m_list->count(); ++i)
            if (!m_beforeScan.contains(m_list->item(i)->data(Qt::UserRole).toInt()))
                m_list->item(i)->setSelected(true);
        m_beforeScan.clear();
        m_addFolderButton->setProperty("scanned", false);
    }
    countNew();
    m_count->setText(found.size() >= filter.limit
                         ? tr("Showing the newest %1 photos — narrow the search to see others.").arg(found.size())
                         : found.size() == 1 ? tr("1 photo found.")
                                             : tr("%1 photos found.").arg(found.size()));
    updateAdd();
}

void UploadPhotoPicker::fillVisibleIcons()
{
    const QRect visible = m_list->viewport()->rect();
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item->data(Qt::UserRole + 3).toBool() || !m_list->visualItemRect(item).intersects(visible))
            continue;
        item->setData(Qt::UserRole + 3, true);   // asked: ready() brings the rest
        const QPixmap pixmap = m_app.thumbnails().thumbnail(item->data(Qt::UserRole + 2).toString(),
                                                            item->data(Qt::UserRole + 1).toString(),
                                                            thumb::ThumbnailCache::kGridPx);
        if (!pixmap.isNull())
            item->setIcon(QIcon(pixmap));
    }
}

void UploadPhotoPicker::updateAdd()
{
    const int n = int(m_list->selectedItems().size());
    m_add->setEnabled(n > 0);
    m_add->setText(n == 0 ? tr("Add") : n == 1 ? tr("Add 1 Photo") : tr("Add %1 Photos").arg(n));
}

QString UploadPhotoPicker::chosenFolder() const
{
    const QString text = m_folder->text().trimmed();
    return isFullPath(text) ? QDir::cleanPath(QDir::fromNativeSeparators(text)) : QString();
}

void UploadPhotoPicker::browse()
{
    const QString start = chosenFolder().isEmpty() ? QDir::homePath() : chosenFolder();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose a Folder of Photos"), start,
                                                          QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        m_folder->setText(QDir::toNativeSeparators(QDir::cleanPath(dir)));
}

void UploadPhotoPicker::countNew()
{
    const QString folder = chosenFolder();
    const int counting = ++m_counting;
    if (folder.isEmpty()) {
        m_newBanner->setVisible(false);
        return;
    }
    const QString connection = m_app.database().connectionName();
    const bool subfolders = m_subfolders->isChecked();
    // Names only, but a big folder tree takes a moment: off the GUI thread.
    // The catalogue connection belongs to this one, so it's read here.
    const QSet<QString> known = catalogued(connection, folder);
    auto *watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher, counting] {
        const int n = watcher->result();
        watcher->deleteLater();
        if (counting != m_counting)
            return;   // the folder has changed since
        m_newBanner->setVisible(n > 0);
        m_newText->setText(n == 1 ? tr("1 photo in this folder isn't in your library yet, so it can't be added here.")
                                  : tr("%1 photos in this folder aren't in your library yet, so they can't be "
                                       "added here.").arg(n));
        m_addFolderButton->setVisible(bool(m_addFolder));
        m_addFolderButton->setEnabled(true);
        m_addFolderButton->setText(tr("Add Folder to Library"));
    });
    watcher->setFuture(QtConcurrent::run([folder, subfolders, known] {
        return QFileInfo(folder).isDir() ? countNotIn(folder, subfolders, known) : 0;
    }));
}

void UploadPhotoPicker::addFolderToLibrary()
{
    const QString folder = chosenFolder();
    if (folder.isEmpty() || !m_addFolder)
        return;
    m_beforeScan.clear();
    for (int i = 0; i < m_list->count(); ++i)
        m_beforeScan.insert(m_list->item(i)->data(Qt::UserRole).toInt());
    m_addFolderButton->setEnabled(false);
    m_addFolderButton->setText(tr("Adding and Matching…"));
    QPointer<UploadPhotoPicker> self(this);
    m_addFolder(folder, [self] {
        if (!self)
            return;
        self->m_addFolderButton->setProperty("scanned", true);
        self->search();
    });
}

QList<int> UploadPhotoPicker::selectedCaptureIds() const
{
    QList<int> ids;
    for (int i = 0; i < m_list->count(); ++i)   // in the order shown
        if (m_list->item(i)->isSelected())
            ids << m_list->item(i)->data(Qt::UserRole).toInt();
    return ids;
}

} // namespace pl
