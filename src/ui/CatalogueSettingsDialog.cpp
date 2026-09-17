#include "ui/CatalogueSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace pl {

namespace {
constexpr int kSqlitePageIndex = 0;
constexpr int kPostgresPageIndex = 1;
} // namespace

CatalogueSettingsDialog::CatalogueSettingsDialog(const CatalogueDescriptor &current,
                                                 QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Catalogue Settings"));
    setModal(true);

    m_backend = new QComboBox(this);
    m_backend->setObjectName(QStringLiteral("catalogueBackend"));
    m_backend->addItem(tr("Local (SQLite)"), int(CatalogueDescriptor::Backend::Sqlite));
    m_backend->addItem(tr("Shared (Postgres)"), int(CatalogueDescriptor::Backend::Postgres));

    // --- Local (SQLite) page ---
    auto *sqlitePage = new QWidget(this);
    m_sqlitePath = new QLineEdit(sqlitePage);
    auto *browse = new QPushButton(tr("Browse…"), sqlitePage);
    connect(browse, &QPushButton::clicked, this, &CatalogueSettingsDialog::browseForSqliteFile);
    auto *sqliteRow = new QVBoxLayout(sqlitePage);
    auto *sqliteForm = new QFormLayout;
    auto *sqlitePathRow = new QVBoxLayout;
    sqlitePathRow->addWidget(m_sqlitePath);
    sqlitePathRow->addWidget(browse);
    sqliteForm->addRow(tr("&File:"), sqlitePathRow);
    sqliteRow->addLayout(sqliteForm);
    sqliteRow->addStretch();

    // --- Shared (Postgres) page ---
    auto *pgPage = new QWidget(this);
    m_pgHost = new QLineEdit(pgPage);
    m_pgHost->setObjectName(QStringLiteral("pgHost"));
    m_pgHost->setPlaceholderText(tr("e.g. 10.1.2.3 or my-instance.us-central1.sql.goog"));
    m_pgPort = new QSpinBox(pgPage);
    m_pgPort->setObjectName(QStringLiteral("pgPort"));
    m_pgPort->setRange(1, 65535);
    m_pgPort->setValue(5432);
    m_pgDbName = new QLineEdit(pgPage);
    m_pgDbName->setObjectName(QStringLiteral("pgDbName"));
    m_pgUser = new QLineEdit(pgPage);
    m_pgUser->setObjectName(QStringLiteral("pgUser"));
    m_pgPassword = new QLineEdit(pgPage);
    m_pgPassword->setObjectName(QStringLiteral("pgPassword"));
    m_pgPassword->setEchoMode(QLineEdit::Password);
    m_pgSslMode = new QComboBox(pgPage);
    for (const QString &mode : {QStringLiteral("disable"), QStringLiteral("prefer"),
                                QStringLiteral("require"), QStringLiteral("verify-full")}) {
        m_pgSslMode->addItem(mode);
    }
    auto *testButton = new QPushButton(tr("Test Connection"), pgPage);
    testButton->setObjectName(QStringLiteral("pgTestConnection"));
    connect(testButton, &QPushButton::clicked, this, &CatalogueSettingsDialog::testConnection);

    auto *pgForm = new QFormLayout;
    pgForm->addRow(tr("&Host:"), m_pgHost);
    pgForm->addRow(tr("&Port:"), m_pgPort);
    pgForm->addRow(tr("&Database:"), m_pgDbName);
    pgForm->addRow(tr("&User:"), m_pgUser);
    pgForm->addRow(tr("Pass&word:"), m_pgPassword);
    pgForm->addRow(tr("&SSL mode:"), m_pgSslMode);

    auto *pgWarning = new QLabel(
        tr("Stored locally in your OS settings, not encrypted. Every collaborator "
           "connects directly to this database -- make sure it's reachable and "
           "that this user has appropriate access."),
        pgPage);
    pgWarning->setWordWrap(true);
    pgWarning->setEnabled(false);

    auto *pgLayout = new QVBoxLayout(pgPage);
    pgLayout->addLayout(pgForm);
    pgLayout->addWidget(testButton);
    pgLayout->addWidget(pgWarning);
    pgLayout->addStretch();

    m_pages = new QStackedWidget(this);
    m_pages->insertWidget(kSqlitePageIndex, sqlitePage);
    m_pages->insertWidget(kPostgresPageIndex, pgPage);
    connect(m_backend, &QComboBox::currentIndexChanged, m_pages, &QStackedWidget::setCurrentIndex);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *restartNotice = new QLabel(
        tr("Takes effect the next time PhotoLife starts."), this);
    restartNotice->setEnabled(false);

    auto *layout = new QVBoxLayout(this);
    auto *backendForm = new QFormLayout;
    backendForm->addRow(tr("&Catalogue:"), m_backend);
    layout->addLayout(backendForm);
    layout->addWidget(m_pages);
    layout->addWidget(restartNotice);
    layout->addWidget(m_buttons);

    connect(m_sqlitePath, &QLineEdit::textChanged, this, &CatalogueSettingsDialog::updateOkState);
    connect(m_pgHost, &QLineEdit::textChanged, this, &CatalogueSettingsDialog::updateOkState);
    connect(m_pgDbName, &QLineEdit::textChanged, this, &CatalogueSettingsDialog::updateOkState);
    connect(m_pgUser, &QLineEdit::textChanged, this, &CatalogueSettingsDialog::updateOkState);

    // Prefill from the current descriptor.
    m_sqlitePath->setText(current.sqlitePath);
    m_pgHost->setText(current.pgHost);
    m_pgPort->setValue(current.pgPort > 0 ? current.pgPort : 5432);
    m_pgDbName->setText(current.pgDbName);
    m_pgUser->setText(current.pgUser);
    m_pgPassword->setText(current.pgPassword);
    const int sslIndex = m_pgSslMode->findText(current.pgSslMode);
    m_pgSslMode->setCurrentIndex(sslIndex >= 0 ? sslIndex : m_pgSslMode->findText(QStringLiteral("prefer")));

    const int backendIndex = current.backend == CatalogueDescriptor::Backend::Postgres
                                  ? kPostgresPageIndex
                                  : kSqlitePageIndex;
    m_backend->setCurrentIndex(backendIndex);
    m_pages->setCurrentIndex(backendIndex);

    updateOkState();
}

void CatalogueSettingsDialog::browseForSqliteFile()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Catalogue File"), m_sqlitePath->text(),
        tr("PhotoLife catalogue (*.db);;All files (*)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty())
        m_sqlitePath->setText(path);
}

void CatalogueSettingsDialog::updateOkState()
{
    const bool sqliteValid = !m_sqlitePath->text().trimmed().isEmpty();
    const bool pgValid = !m_pgHost->text().trimmed().isEmpty()
                        && !m_pgDbName->text().trimmed().isEmpty()
                        && !m_pgUser->text().trimmed().isEmpty();
    const bool valid = m_backend->currentIndex() == kPostgresPageIndex ? pgValid : sqliteValid;
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

void CatalogueSettingsDialog::testConnection()
{
    const QString connectionName = QStringLiteral("photolife-catalogue-settings-test");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), connectionName);
        db.setHostName(m_pgHost->text().trimmed());
        db.setPort(m_pgPort->value());
        db.setDatabaseName(m_pgDbName->text().trimmed());
        db.setUserName(m_pgUser->text().trimmed());
        db.setPassword(m_pgPassword->text());
        db.setConnectOptions(QStringLiteral("sslmode=%1").arg(m_pgSslMode->currentText()));

        if (db.open()) {
            db.close();
            QMessageBox::information(this, tr("Test Connection"),
                                     tr("Connected successfully."));
        } else {
            QMessageBox::warning(this, tr("Test Connection"),
                                 tr("Could not connect:\n%1").arg(db.lastError().text()));
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
}

CatalogueDescriptor CatalogueSettingsDialog::descriptor() const
{
    CatalogueDescriptor d;
    if (m_backend->currentIndex() == kPostgresPageIndex) {
        d.backend = CatalogueDescriptor::Backend::Postgres;
        d.pgHost = m_pgHost->text().trimmed();
        d.pgPort = m_pgPort->value();
        d.pgDbName = m_pgDbName->text().trimmed();
        d.pgUser = m_pgUser->text().trimmed();
        d.pgPassword = m_pgPassword->text();
        d.pgSslMode = m_pgSslMode->currentText();
    } else {
        d.backend = CatalogueDescriptor::Backend::Sqlite;
        d.sqlitePath = m_sqlitePath->text().trimmed();
    }
    return d;
}

} // namespace pl
