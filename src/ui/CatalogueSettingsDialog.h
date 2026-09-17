#pragma once

#include "db/CatalogueDescriptor.h"

#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;
class QStackedWidget;

namespace pl {

// Lets the user pick which catalogue to open next: a local SQLite file, or a
// shared Postgres database (e.g. Cloud SQL) other collaborators point their
// own PhotoLife instances at. Edits Settings' stored CatalogueDescriptor;
// the change takes effect on the next app start (nothing here re-points an
// already-open Database/ScanService/MatchService at the new descriptor).
class CatalogueSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CatalogueSettingsDialog(const CatalogueDescriptor &current, QWidget *parent = nullptr);

    CatalogueDescriptor descriptor() const;

private:
    void updateOkState();
    void testConnection();
    void browseForSqliteFile();

    QComboBox *m_backend;
    QStackedWidget *m_pages;

    QLineEdit *m_sqlitePath;

    QLineEdit *m_pgHost;
    QSpinBox *m_pgPort;
    QLineEdit *m_pgDbName;
    QLineEdit *m_pgUser;
    QLineEdit *m_pgPassword;
    QComboBox *m_pgSslMode;

    QDialogButtonBox *m_buttons;
};

} // namespace pl
