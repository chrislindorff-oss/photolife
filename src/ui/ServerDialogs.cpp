#include "ui/ServerDialogs.h"

#include "sync/SyncActivityLog.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {

QString describeTime(const QDateTime &time)
{
    if (!time.isValid())
        return QObject::tr("never");
    const QDateTime local = time.toLocalTime();
    return local.date() == QDate::currentDate()
               ? QLocale().toString(local.time(), QLocale::ShortFormat)
               : QLocale().toString(local, QLocale::ShortFormat);
}

namespace {

QDateTime fromIso(const QString &iso)
{
    QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!t.isValid())
        t = QDateTime::fromString(iso, Qt::ISODate);
    return t;
}

QLabel *valueLabel(QWidget *parent)
{
    auto *l = new QLabel(parent);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

} // namespace

// --- Server Status ------------------------------------------------------------

ServerStatusDialog::ServerStatusDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Server Status"));
    setMinimumWidth(520);

    m_mode = new QLabel(this);
    QFont big = m_mode->font();
    big.setPointSizeF(big.pointSizeF() * 1.25);
    big.setBold(true);
    m_mode->setFont(big);

    auto section = [this](const QString &title, QLabel *&value) {
        auto *box = new QGroupBox(title, this);
        auto *l = new QVBoxLayout(box);
        value = valueLabel(box);
        l->addWidget(value);
        return box;
    };

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_mode);
    layout->addWidget(section(tr("Server"), m_server));
    layout->addWidget(section(tr("Connection"), m_connection));
    layout->addWidget(section(tr("You"), m_you));
    layout->addWidget(section(tr("Edit lock"), m_lock));
    layout->addWidget(section(tr("Versions"), m_versions));

    auto *changesBox = section(tr("Your unsent changes"), m_changes);
    auto *review = new QPushButton(tr("Review Changes…"), changesBox);
    connect(review, &QPushButton::clicked, this, &ServerStatusDialog::reviewChangesRequested);
    changesBox->layout()->addWidget(review);
    layout->addWidget(changesBox);

    layout->addWidget(section(tr("Photo library folder"), m_library));
    auto *photosBox = section(tr("Photos online"), m_photos);
    m_syncPhotos = new QPushButton(tr("Sync Photos"), photosBox);
    connect(m_syncPhotos, &QPushButton::clicked, this, &ServerStatusDialog::syncPhotosRequested);
    photosBox->layout()->addWidget(m_syncPhotos);
    layout->addWidget(photosBox);
    auto *backupsBox = section(tr("Backups"), m_backups);
    auto *restore = new QPushButton(tr("Restore a Backup…"), backupsBox);
    connect(restore, &QPushButton::clicked, this, &ServerStatusDialog::restoreBackupRequested);
    backupsBox->layout()->addWidget(restore);
    layout->addWidget(backupsBox);

    m_startEditing = new QPushButton(tr("Start Editing"), this);
    m_getLatest = new QPushButton(tr("Get Latest"), this);
    m_sync = new QPushButton(tr("Sync"), this);
    m_finish = new QPushButton(tr("Finish Editing"), this);
    auto *test = new QPushButton(tr("Test Connection"), this);
    auto *log = new QPushButton(tr("Activity Log…"), this);
    auto *close = new QPushButton(tr("Close"), this);
    connect(m_startEditing, &QPushButton::clicked, this, &ServerStatusDialog::startEditingRequested);
    connect(m_getLatest, &QPushButton::clicked, this, &ServerStatusDialog::getLatestRequested);
    connect(m_sync, &QPushButton::clicked, this, &ServerStatusDialog::syncRequested);
    connect(m_finish, &QPushButton::clicked, this, &ServerStatusDialog::finishEditingRequested);
    connect(test, &QPushButton::clicked, this, &ServerStatusDialog::testConnectionRequested);
    connect(log, &QPushButton::clicked, this, &ServerStatusDialog::activityLogRequested);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto *buttons = new QHBoxLayout;
    for (QPushButton *b : {m_startEditing, m_getLatest, m_sync, m_finish})
        buttons->addWidget(b);
    buttons->addStretch();
    buttons->addWidget(test);
    buttons->addWidget(log);
    buttons->addWidget(close);
    layout->addLayout(buttons);
}

void ServerStatusDialog::setView(const ServerView &v)
{
    m_mode->setText(ServerBar::modeText(v));
    m_mode->setStyleSheet(QStringLiteral("color: %1;").arg(ServerBar::modeColor(v).name()));

    m_server->setText(v.server);

    QString connection;
    if (v.running)
        connection = tr("Working: %1").arg(v.runningPhase.isEmpty() ? tr("talking to the server…")
                                                                   : v.runningPhase);
    else if (v.offline)
        connection = tr("Can't reach the server (last tried just now).") + QLatin1Char('\n') + v.lastError;
    else if (v.checked)
        connection = tr("Reachable. Last contact %1.").arg(describeTime(v.lastContact));
    else
        connection = tr("Not checked yet.");
    if (v.lastTest)
        connection += QLatin1Char('\n')
                      + tr("Last test: connect %1 ms, round trip %2 ms.")
                            .arg(v.lastTest->connectMs)
                            .arg(QString::number(v.lastTest->roundTripMs, 'f', 0));
    m_connection->setText(connection);

    QString you = tr("%1 on %2.").arg(v.me.user, v.me.machine) + QLatin1Char('\n');
    if (v.editing) {
        const QDateTime since = v.state && v.state->heldBy(v.me) ? fromIso(v.state->lockedAt) : QDateTime();
        you += since.isValid() ? tr("Editing since %1: your changes go to the server when you Sync.")
                                     .arg(describeTime(since))
                               : tr("Editing: your changes go to the server when you Sync.");
    } else {
        you += tr("Viewing: the catalogue is read-only on this computer. Start Editing to change it.");
    }
    m_you->setText(you);

    if (!v.state)
        m_lock->setText(tr("Unknown until the server answers."));
    else if (!v.state->isLocked())
        m_lock->setText(tr("Free: nobody is editing."));
    else
        m_lock->setText(tr("Held by %1 on %2 since %3.%4")
                            .arg(v.state->heldBy(v.me) ? tr("you") : v.state->lockedBy,
                                 v.state->lockedMachine, describeTime(fromIso(v.state->lockedAt)),
                                 v.state->heldBy(v.me) ? QString()
                                                       : QLatin1Char(' ')
                                                             + tr("You can view, not edit, until "
                                                                  "they Finish Editing.")));

    if (!v.state) {
        m_versions->setText(tr("Your copy: version %1. Server: unknown.").arg(v.localVersion));
    } else {
        QString text = tr("Server: version %1").arg(v.state->version);
        if (!v.state->pushedBy.isEmpty())
            text += tr(", last updated by %1 at %2")
                        .arg(v.state->pushedBy, describeTime(fromIso(v.state->pushedAt)));
        text += QStringLiteral(".\n") + tr("Your copy: version %1 — %2.")
                                            .arg(v.localVersion)
                                            .arg(v.newerAvailable() ? tr("a newer version is on the server")
                                                                    : tr("up to date"));
        if (v.lastSync.isValid())
            text += QLatin1Char('\n') + tr("Your last sync: %1.").arg(describeTime(v.lastSync));
        m_versions->setText(text);
    }

    if (!v.changes)
        m_changes->setText(tr("Not counted yet."));
    else if (v.changes->isEmpty())
        m_changes->setText(tr("None — this computer matches the last sync."));
    else
        m_changes->setText(v.changes->headline()
                           + (v.editing ? QString()
                                        : QStringLiteral("\n")
                                              + tr("You're not editing, so these can't be sent; "
                                                   "they'll be replaced by the next Get Latest.")));

    m_library->setText(v.libraryRoot.isEmpty() ? tr("Not set — set it in Server Settings.")
                                                : QDir::toNativeSeparators(v.libraryRoot));
    if (v.photosEnabled) {
        QString photos = ServerBar::photosText(v);
        if (!v.photosKept.isEmpty())
            photos += QLatin1Char('\n') + tr("Kept on this computer: %1.").arg(v.photosKept);
        if (v.photosLastSync.isValid())
            photos += QLatin1Char('\n') + tr("Last photo sync: %1.").arg(describeTime(v.photosLastSync));
        m_photos->setText(photos);
    } else {
        m_photos->setText(tr("Not synced by PhotoLife — the photo library folder is shared some "
                             "other way. Set up photo storage in Server Settings to have "
                             "PhotoLife sync the photos itself."));
    }
    m_syncPhotos->setVisible(v.photosEnabled);
    m_syncPhotos->setText(v.photosRunning ? tr("Stop Photo Sync") : tr("Sync Photos"));
    m_backups->setText(v.backups > 0 ? tr("%n backup(s) of earlier catalogues kept beside it.",
                                          nullptr, v.backups)
                                     : tr("None yet."));

    const bool idle = !v.running;
    m_startEditing->setVisible(!v.editing);
    m_getLatest->setVisible(!v.editing);
    m_sync->setVisible(v.editing);
    m_finish->setVisible(v.editing);
    for (QPushButton *b : {m_startEditing, m_getLatest, m_sync, m_finish})
        b->setEnabled(idle && !v.offline);
}

// --- Review Changes -----------------------------------------------------------

ReviewChangesDialog::ReviewChangesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Review Changes"));
    resize(760, 540);
    m_headline = new QLabel(this);
    m_headline->setWordWrap(true);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({tr("Change"), tr("Added"), tr("Changed"), tr("Removed")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c <= 3; ++c)
        m_tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(false);

    auto *refresh = new QPushButton(tr("Refresh"), this);
    m_sync = new QPushButton(tr("Sync Now"), this);
    m_discard = new QPushButton(tr("Discard All…"), this);
    auto *close = new QPushButton(tr("Close"), this);
    connect(refresh, &QPushButton::clicked, this, &ReviewChangesDialog::refreshRequested);
    connect(m_sync, &QPushButton::clicked, this, &ReviewChangesDialog::syncRequested);
    connect(m_discard, &QPushButton::clicked, this, &ReviewChangesDialog::discardRequested);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(refresh);
    buttons->addStretch();
    buttons->addWidget(m_discard);
    buttons->addWidget(m_sync);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_headline);
    layout->addWidget(m_tree, 1);
    layout->addLayout(buttons);
}

void ReviewChangesDialog::setSummary(const std::optional<sync::ChangeSummary> &summary,
                                     bool editing, const QString &error)
{
    m_tree->clear();
    if (!summary) {
        m_headline->setText(error.isEmpty() ? tr("Counting your changes…") : error);
        m_sync->setEnabled(false);
        m_discard->setEnabled(false);
        return;
    }
    m_headline->setText(
        summary->isEmpty()
            ? tr("No changes: this computer matches the last sync.")
            : tr("Since your last sync: %1.").arg(summary->headline())
                  + (editing ? QString()
                             : QStringLiteral("\n")
                                   + tr("You're not editing, so these can't be sent.")));
    auto number = [](int n) { return n > 0 ? QLocale().toString(n) : QString(); };
    auto addKind = [&](QTreeWidgetItem *parentItem, const sync::ChangeKind &k) {
        auto *item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(m_tree);
        item->setText(0, k.title);
        item->setText(1, number(k.added));
        item->setText(2, number(k.changed));
        item->setText(3, number(k.removed));
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        for (const QString &line : k.items) {
            auto *child = new QTreeWidgetItem(item);
            child->setText(0, line);
            child->setToolTip(0, line);   // long lines are cut off in the column
        }
        if (k.moreItems > 0)
            (new QTreeWidgetItem(item))->setText(0, tr("…and %L1 more").arg(k.moreItems));
    };
    QTreeWidgetItem *automatic = nullptr;
    for (const sync::ChangeKind &k : summary->kinds) {
        if (k.automatic && !automatic) {
            automatic = new QTreeWidgetItem(m_tree);
            automatic->setText(0, tr("Automatic updates (%L1)").arg(summary->automaticRows()));
            automatic->setToolTip(0, tr("Derived or background data PhotoLife keeps current by "
                                        "itself: cover photos, localities, place details, "
                                        "downloaded statuses …"));
        }
        addKind(k.automatic ? automatic : nullptr, k);
    }
    // Yours expanded one level; automatic ones collapsed.
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = m_tree->topLevelItem(i);
        if (top != automatic && top->childCount() <= 20)
            top->setExpanded(true);
    }
    m_sync->setEnabled(editing && !summary->isEmpty());
    m_discard->setEnabled(!summary->isEmpty());
}

// --- Activity Log -------------------------------------------------------------

ActivityLogDialog::ActivityLogDialog(QString logPath, QWidget *parent)
    : QDialog(parent), m_path(std::move(logPath))
{
    setWindowTitle(tr("Activity Log"));
    resize(760, 460);
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("When"), tr("Action"), tr("Result"), tr("Details")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setWordWrap(true);

    m_errorsOnly = new QCheckBox(tr("Problems only"), this);
    connect(m_errorsOnly, &QCheckBox::toggled, this, &ActivityLogDialog::reload);
    auto *open = new QPushButton(tr("Open Log File"), this);
    connect(open, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
    });
    auto *refresh = new QPushButton(tr("Refresh"), this);
    connect(refresh, &QPushButton::clicked, this, &ActivityLogDialog::reload);
    auto *close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_errorsOnly);
    buttons->addStretch();
    buttons->addWidget(open);
    buttons->addWidget(refresh);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttons);
    reload();
}

void ActivityLogDialog::reload()
{
    const ThemeColors &theme = themeColors(currentThemeVariant());
    QList<sync::SyncActivityEntry> entries = sync::SyncActivityLog(m_path).read();
    if (m_errorsOnly->isChecked())
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [](const sync::SyncActivityEntry &e) { return e.ok; }),
                      entries.end());
    m_table->setRowCount(int(entries.size()));
    for (int r = 0; r < entries.size(); ++r) {
        const auto &e = entries.at(r);
        auto *result = new QTableWidgetItem(e.ok ? tr("OK") : tr("Failed"));
        result->setForeground(e.ok ? theme.positive : theme.danger);
        QString details = e.ok ? e.detail : e.error;
        if (e.durationMs >= 1000)
            details += tr(" (%1 s)").arg(QString::number(e.durationMs / 1000.0, 'f', 1));
        m_table->setItem(r, 0, new QTableWidgetItem(describeTime(e.at)));
        m_table->setItem(r, 1, new QTableWidgetItem(e.action));
        m_table->setItem(r, 2, result);
        m_table->setItem(r, 3, new QTableWidgetItem(details));
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setStretchLastSection(true);
}

} // namespace pl
