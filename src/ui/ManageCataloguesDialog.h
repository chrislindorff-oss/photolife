#pragma once

#include "settings/Settings.h"

#include <QDialog>

class QPushButton;
class QTreeWidget;

namespace pl {

class Settings;

// "Manage Catalogues": the Local catalogue and every Server catalogue, with
// what each holds, and Open / Add Server / Edit / Remove. Opening another
// catalogue is MainWindow's job (it restarts PhotoLife): see openRequested().
class ManageCataloguesDialog : public QDialog
{
    Q_OBJECT

public:
    ManageCataloguesDialog(Settings &settings, QWidget *parent = nullptr);

    // "23 reference trees · 12,513 photos · 48 MB", or that there's no copy yet.
    static QString describeCatalogueFile(const QString &path);

signals:
    void openRequested(const QString &profileId);
    // The open catalogue's server settings were changed and saved; `before`
    // is how they were. MainWindow puts them to use straight away.
    void activeServerEdited(const pl::Settings::CatalogueProfile &before);

private:
    void reload();
    QString selectedId() const;
    void addServer();
    void editServer();
    void removeServer();
    void updateButtons();

    Settings &m_settings;
    QTreeWidget *m_list;
    QPushButton *m_open;
    QPushButton *m_edit;
    QPushButton *m_remove;
};

} // namespace pl
