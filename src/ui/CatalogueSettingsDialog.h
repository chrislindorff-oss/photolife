#pragma once

#include "settings/Settings.h"

#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;

namespace pl {

// Adds or edits one Server catalogue profile (see Settings::CatalogueProfile):
// its display name, the Postgres connection, your name there and this
// machine's photo library folder for it.
class CatalogueSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CatalogueSettingsDialog(const Settings::CatalogueProfile &profile,
                                     QWidget *parent = nullptr);
    Settings::CatalogueProfile profile() const;

private:
    void updateOkState();
    void testConnection();
    void browseForLibraryRoot();

    Settings::CatalogueProfile m_profile;
    QLineEdit *m_name;
    QLineEdit *m_pgHost;
    QSpinBox *m_pgPort;
    QLineEdit *m_pgDbName;
    QLineEdit *m_pgUser;
    QLineEdit *m_pgPassword;
    QComboBox *m_pgSslMode;
    QLineEdit *m_userName;
    QLineEdit *m_libraryRoot;
    QDialogButtonBox *m_buttons;
};

} // namespace pl
