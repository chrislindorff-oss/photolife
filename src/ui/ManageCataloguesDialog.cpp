#include "ui/ManageCataloguesDialog.h"

#include "settings/Settings.h"
#include "ui/CatalogueSettingsDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace pl {

QString ManageCataloguesDialog::describeCatalogueFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return tr("No copy on this computer yet");
    QString counts;
    const QString name = QStringLiteral("photolife-describe-") + QUuid::createUuid().toString();
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT (SELECT COUNT(*) FROM project), (SELECT COUNT(*) FROM capture)"))
                && q.next())
                counts = tr("%n reference tree(s)", nullptr, q.value(0).toInt()) + QStringLiteral(" · ")
                         + tr("%1 photos").arg(QLocale().toString(q.value(1).toInt())) + QStringLiteral(" · ");
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return counts + tr("%1 MB").arg(QString::number(info.size() / 1048576.0, 'f', 1));
}

ManageCataloguesDialog::ManageCataloguesDialog(Settings &settings, QWidget *parent)
    : QDialog(parent), m_settings(settings)
{
    setWindowTitle(tr("Manage Catalogues"));
    resize(760, 380);

    auto *intro = new QLabel(
        tr("Each catalogue is completely separate: its own reference trees, photos and folders. "
           "Your Local catalogue is your own library on this computer; a Server catalogue is "
           "shared with collaborators and synced on demand. Opening another catalogue restarts "
           "PhotoLife."),
        this);
    intro->setWordWrap(true);

    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({tr("Catalogue"), tr("Where"), tr("On this computer")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(true);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &ManageCataloguesDialog::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (m_open->isEnabled())
            m_open->click();
    });

    m_open = new QPushButton(tr("Open"), this);
    auto *add = new QPushButton(tr("Add Server…"), this);
    m_edit = new QPushButton(tr("Edit…"), this);
    m_remove = new QPushButton(tr("Remove…"), this);
    auto *close = new QPushButton(tr("Close"), this);
    connect(m_open, &QPushButton::clicked, this, [this] {
        const QString id = selectedId();
        if (!id.isEmpty())
            emit openRequested(id);
    });
    connect(add, &QPushButton::clicked, this, &ManageCataloguesDialog::addServer);
    connect(m_edit, &QPushButton::clicked, this, &ManageCataloguesDialog::editServer);
    connect(m_remove, &QPushButton::clicked, this, &ManageCataloguesDialog::removeServer);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_open);
    buttons->addStretch();
    buttons->addWidget(add);
    buttons->addWidget(m_edit);
    buttons->addWidget(m_remove);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_list, 1);
    layout->addLayout(buttons);
    reload();
}

void ManageCataloguesDialog::reload()
{
    const QString previous = selectedId();
    const QString active = m_settings.activeProfile().id;
    m_list->clear();
    for (const Settings::CatalogueProfile &p : m_settings.profiles()) {
        auto *item = new QTreeWidgetItem(m_list);
        const bool isActive = p.id == active;
        item->setText(0, (p.server ? tr("Server: %1").arg(p.name) : p.name)
                             + (isActive ? tr("  (open now)") : QString()));
        item->setText(1, p.server ? tr("%1 on %2 · photos in %3")
                                        .arg(p.pg.pgDbName, p.pg.pgHost,
                                             p.libraryRoot.isEmpty() ? tr("(not set)")
                                                                     : QDir::toNativeSeparators(p.libraryRoot))
                                  : QDir::toNativeSeparators(m_settings.profileDatabasePath(p.id)));
        item->setText(2, describeCatalogueFile(m_settings.profileDatabasePath(p.id)));
        item->setData(0, Qt::UserRole, p.id);
        if (isActive) {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
        if (p.id == previous || (previous.isEmpty() && isActive))
            m_list->setCurrentItem(item);
    }
    updateButtons();
}

QString ManageCataloguesDialog::selectedId() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void ManageCataloguesDialog::updateButtons()
{
    const QString id = selectedId();
    const bool server = !id.isEmpty() && id != QLatin1String(Settings::kLocalProfileId);
    const bool active = id == m_settings.activeProfile().id;
    m_open->setEnabled(!id.isEmpty() && !active);
    m_edit->setEnabled(server);
    m_remove->setEnabled(server && !active);
    m_remove->setToolTip(active ? tr("Open another catalogue first.") : QString());
}

void ManageCataloguesDialog::addServer()
{
    CatalogueSettingsDialog dialog(Settings::CatalogueProfile{}, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString id = m_settings.saveServerProfile(dialog.profile());
    reload();
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        if (m_list->topLevelItem(i)->data(0, Qt::UserRole).toString() == id)
            m_list->setCurrentItem(m_list->topLevelItem(i));
    }
    if (QMessageBox::question(this, tr("Add Server"),
                              tr("Open \"%1\" now? PhotoLife restarts, then offers to download its "
                                 "catalogue (or to start it).")
                                  .arg(dialog.profile().name))
        == QMessageBox::Yes)
        emit openRequested(id);
}

void ManageCataloguesDialog::editServer()
{
    const auto p = m_settings.profile(selectedId());
    if (!p || !p->server)
        return;
    const bool active = p->id == m_settings.activeProfile().id;
    CatalogueSettingsDialog dialog(*p, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const Settings::CatalogueProfile edited = dialog.profile();
    if (active && m_settings.syncEditing()
        && (edited.pg.pgHost != p->pg.pgHost || edited.pg.pgDbName != p->pg.pgDbName)) {
        QMessageBox::warning(this, tr("Edit Server"),
                             tr("Finish Editing before pointing this catalogue at another server."));
        return;
    }
    m_settings.saveServerProfile(edited);
    reload();
    if (active)
        emit activeServerEdited(*p);
}

void ManageCataloguesDialog::removeServer()
{
    const auto p = m_settings.profile(selectedId());
    if (!p || !p->server || p->id == m_settings.activeProfile().id)
        return;
    QMessageBox box(QMessageBox::Warning, tr("Remove Server Catalogue"),
                    tr("Remove \"%1\" from this computer?").arg(p->name), QMessageBox::NoButton, this);
    box.setInformativeText(tr("The shared catalogue on the server isn't affected, and you can add "
                              "it again later."));
    auto *deleteCopy = new QCheckBox(tr("Also delete this computer's working copy and its backups"), &box);
    box.setCheckBox(deleteCopy);
    QPushButton *remove = box.addButton(tr("Remove"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != remove)
        return;
    const QString folder = QFileInfo(m_settings.profileDatabasePath(p->id)).absolutePath();
    m_settings.removeServerProfile(p->id);
    // Only ever a server's own folder (data/servers/<id>), never the Local one.
    if (deleteCopy->isChecked() && QFileInfo(folder).dir().dirName() == QLatin1String("servers"))
        QDir(folder).removeRecursively();
    reload();
}

} // namespace pl
