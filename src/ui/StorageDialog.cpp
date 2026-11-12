#include "ui/StorageDialog.h"

#include "app/Application.h"
#include "app/StorageStats.h"
#include "db/CatalogueDescriptor.h"
#include "net/PhotoCache.h"
#include "net/TileCache.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace pl {
namespace {
enum RowIndex { CatalogueRow, ThumbnailRow, ReferenceRow, StreetRow, AerialRow };
}

StorageDialog::StorageDialog(Application &app, QWidget *parent) : QDialog(parent), m_app(app)
{
    setWindowTitle(tr("Storage Usage"));
    const bool sqlite = m_app.settings().catalogueDescriptor().backend == CatalogueDescriptor::Backend::Sqlite;
    m_rows = {
        {tr("Catalogue database"), QString()},
        {tr("Photo thumbnails"), m_app.thumbnails().cacheDir()},
        {tr("Reference photos (iNaturalist)"), m_app.photoCache().cacheDir()},
        {tr("Map tiles: street map"), m_app.tileCache().cacheDir()},
        {tr("Map tiles: aerial photos"), m_app.satelliteTileCache().cacheDir()},
    };
    auto *grid = new QGridLayout;
    grid->setColumnStretch(0, 1);
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &row = m_rows[i];
        row.size = new QLabel(QStringLiteral("…"), this);
        row.size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(new QLabel(row.name, this), i, 0);
        grid->addWidget(row.size, i, 1);
        if (i == ThumbnailRow) {
            row.button = new QPushButton(tr("Remove Unused…"), this);
            row.button->setToolTip(tr("Remove the thumbnails of photos that are no longer in any catalogue on "
                                      "this computer. Thumbnails of photos you still have are kept."));
            // Without a catalogue file to read there's no telling which are in use.
            row.button->setEnabled(sqlite);
            connect(row.button, &QPushButton::clicked, this, &StorageDialog::removeUnusedThumbnails);
        } else if (i != CatalogueRow) {
            row.button = new QPushButton(tr("Clear…"), this);
            row.button->setToolTip(tr("Remove these from this computer. They are downloaded again as they're needed."));
            connect(row.button, &QPushButton::clicked, this, [this, i] { clearCache(i); });
        }
        if (row.button) {
            row.button->setAutoDefault(false);
            grid->addWidget(row.button, i, 2);
        }
    }
    m_total = new QLabel(QStringLiteral("…"), this);
    m_total->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(new QLabel(tr("<b>Total</b>"), this), int(m_rows.size()), 0);
    grid->addWidget(m_total, int(m_rows.size()), 1);

    m_tileLimit = new QComboBox(this);
    for (int mb : {250, 500, 1000, 2000})
        m_tileLimit->addItem(QLocale().formattedDataSize(qint64(mb) * 1024 * 1024, 0), mb);
    m_tileLimit->addItem(tr("No limit"), 0);
    const int at = m_tileLimit->findData(m_app.settings().tileCacheLimitMb());
    m_tileLimit->setCurrentIndex(at >= 0 ? at : 1);
    m_tileLimit->setToolTip(tr("When a set of map tiles passes this, the oldest are removed the next time "
                               "PhotoLife starts. They're downloaded again if you look there."));
    connect(m_tileLimit, &QComboBox::activated, this,
            [this] { m_app.settings().setTileCacheLimitMb(m_tileLimit->currentData().toInt()); });
    auto *limit = new QHBoxLayout;
    limit->addWidget(new QLabel(tr("Keep each set of map tiles under:"), this));
    limit->addWidget(m_tileLimit);
    limit->addStretch(1);

    m_note = new QLabel(tr("Your photos are never touched here: only what PhotoLife made or downloaded "
                           "itself, and can make or download again."),
                        this);
    m_note->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(grid);
    layout->addSpacing(6);
    layout->addLayout(limit);
    layout->addWidget(m_note);
    layout->addWidget(buttons);
    setMinimumWidth(520);
    measure();
}

void StorageDialog::setBusy(bool busy)
{
    for (const Row &row : std::as_const(m_rows)) {
        if (row.button)
            row.button->setDisabled(busy);
    }
    if (!busy) {
        const bool sqlite = m_app.settings().catalogueDescriptor().backend == CatalogueDescriptor::Backend::Sqlite;
        m_rows[ThumbnailRow].button->setEnabled(sqlite);
    }
}

void StorageDialog::measure()
{
    const int generation = ++m_generation;
    QStringList dirs;
    for (const Row &row : std::as_const(m_rows))
        dirs << row.dir;
    const bool sqlite = m_app.settings().catalogueDescriptor().backend == CatalogueDescriptor::Backend::Sqlite;
    const QString dbPath = sqlite ? m_app.settings().databasePath() : QString();
    auto *watcher = new QFutureWatcher<QList<qint64>>(this);
    connect(watcher, &QFutureWatcher<QList<qint64>>::finished, this, [this, watcher, generation, sqlite] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        const QList<qint64> sizes = watcher->result();
        const QLocale locale;
        qint64 total = 0;
        for (int i = 0; i < m_rows.size(); ++i) {
            total += sizes.at(i);
            m_rows[i].size->setText(i == CatalogueRow && !sqlite ? tr("Shared (Postgres)")
                                                                 : locale.formattedDataSize(sizes.at(i)));
        }
        m_total->setText(QStringLiteral("<b>%1</b>").arg(locale.formattedDataSize(total)));
    });
    watcher->setFuture(QtConcurrent::run([dirs, dbPath] {
        QList<qint64> sizes;
        for (const QString &dir : dirs)
            sizes << (dir.isEmpty() ? 0 : app::directoryBytes(dir));
        if (!dbPath.isEmpty()) {
            for (const char *suffix : {"", "-wal", "-shm"})
                sizes[CatalogueRow] += QFileInfo(dbPath + QLatin1String(suffix)).size();
        }
        return sizes;
    }));
}

void StorageDialog::clearCache(int row)
{
    const Row &r = m_rows.at(row);
    if (QMessageBox::question(this, tr("Storage Usage"),
                              tr("Remove everything in “%1” from this computer?\n\nIt is downloaded "
                                 "again as it's needed, which takes a connection and a little time.")
                                  .arg(r.name),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    setBusy(true);
    r.size->setText(tr("clearing…"));
    auto *watcher = new QFutureWatcher<app::Removed>(this);
    connect(watcher, &QFutureWatcher<app::Removed>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const app::Removed removed = watcher->result();
        m_note->setText(tr("Removed %n file(s), %1.", nullptr, removed.files)
                            .arg(QLocale().formattedDataSize(removed.bytes)));
        setBusy(false);
        measure();
    });
    watcher->setFuture(QtConcurrent::run([dir = r.dir] { return app::clearDirectory(dir); }));
}

void StorageDialog::removeUnusedThumbnails()
{
    // Thumbnails are shared by every catalogue on this computer (the Local
    // one and each server's working copy): one in use by any of them stays.
    QStringList catalogues;
    for (const Settings::CatalogueProfile &p : m_app.settings().profiles()) {
        const QString path = m_app.settings().profileDatabasePath(p.id);
        if (!path.isEmpty() && QFileInfo::exists(path))
            catalogues << path;
    }
    if (catalogues.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Storage Usage"),
                              tr("Remove the thumbnails of photos that are no longer in any catalogue on this "
                                 "computer?\n\nThumbnails of photos still in a catalogue are kept, including "
                                 "ones whose photo is online only. Thumbnails made for folders opened in "
                                 "Organise go too, and are made again when you open the folder."),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    setBusy(true);
    m_rows[ThumbnailRow].size->setText(tr("checking…"));
    struct Result
    {
        app::Removed removed;
        QString error;
    };
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const Result result = watcher->result();
        m_note->setText(result.error.isEmpty()
                            ? tr("Removed %n unused thumbnail(s), %1.", nullptr, result.removed.files)
                                  .arg(QLocale().formattedDataSize(result.removed.bytes))
                            : tr("Nothing was removed: %1").arg(result.error));
        setBusy(false);
        measure();
    });
    watcher->setFuture(QtConcurrent::run([catalogues, dir = m_rows.at(ThumbnailRow).dir] {
        Result out;
        QSet<QString> keep;
        for (const QString &path : catalogues) {
            QString error;
            // One that can't be read might be using any of them: stop.
            if (!app::catalogueContentHashes(path, &keep, &error)) {
                out.error = QObject::tr("the catalogue at %1 couldn't be read (%2).").arg(path, error);
                return out;
            }
        }
        out.removed = app::removeThumbnailsExcept(dir, keep);
        return out;
    }));
}

} // namespace pl
