#include "ui/CatalogueSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVBoxLayout>

namespace pl {

CatalogueSettingsDialog::CatalogueSettingsDialog(const Settings::CatalogueProfile &profile,
                                                 QWidget *parent)
    : QDialog(parent), m_profile(profile)
{
    setWindowTitle(profile.id.isEmpty() ? tr("Add Server Catalogue") : tr("Edit Server Catalogue"));
    setModal(true);

    m_name = new QLineEdit(profile.name, this);
    m_name->setPlaceholderText(tr("e.g. Nature Photo Club"));
    m_pgHost = new QLineEdit(profile.pg.pgHost, this);
    m_pgHost->setObjectName(QStringLiteral("pgHost"));
    m_pgHost->setPlaceholderText(tr("e.g. ep-cool-lab-12345.ap-southeast-2.aws.neon.tech"));
    m_pgPort = new QSpinBox(this);
    m_pgPort->setRange(1, 65535);
    m_pgPort->setValue(profile.pg.pgPort > 0 ? profile.pg.pgPort : 5432);
    m_pgDbName = new QLineEdit(profile.pg.pgDbName, this);
    m_pgUser = new QLineEdit(profile.pg.pgUser, this);
    m_pgPassword = new QLineEdit(profile.pg.pgPassword, this);
    m_pgPassword->setEchoMode(QLineEdit::Password);
    m_pgSslMode = new QComboBox(this);
    for (const QString &mode : {QStringLiteral("disable"), QStringLiteral("prefer"),
                                QStringLiteral("require"), QStringLiteral("verify-full")})
        m_pgSslMode->addItem(mode);
    const int ssl = m_pgSslMode->findText(profile.pg.pgSslMode);
    m_pgSslMode->setCurrentIndex(ssl >= 0 ? ssl : m_pgSslMode->findText(QStringLiteral("require")));

    QString defaultUser = qEnvironmentVariable("USER");
    if (defaultUser.isEmpty())
        defaultUser = qEnvironmentVariable("USERNAME");
    m_userName = new QLineEdit(profile.userName.isEmpty() ? defaultUser : profile.userName, this);
    m_userName->setToolTip(tr("Shown to your collaborators while you're editing this catalogue."));
    m_libraryRoot = new QLineEdit(profile.libraryRoot, this);
    m_libraryRoot->setToolTip(
        tr("Where this catalogue's photos are on this computer -- e.g. the shared Google Drive "
           "or OpenDrive folder. Photo locations are shared relative to it, so each "
           "collaborator can keep it wherever suits them."));
    auto *browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &CatalogueSettingsDialog::browseForLibraryRoot);
    auto *rootRow = new QHBoxLayout;
    rootRow->addWidget(m_libraryRoot, 1);
    rootRow->addWidget(browse);

    auto *form = new QFormLayout;
    form->addRow(tr("&Name:"), m_name);
    form->addRow(tr("&Host:"), m_pgHost);
    form->addRow(tr("&Port:"), m_pgPort);
    form->addRow(tr("&Database:"), m_pgDbName);
    form->addRow(tr("&User:"), m_pgUser);
    form->addRow(tr("Pass&word:"), m_pgPassword);
    form->addRow(tr("&SSL mode:"), m_pgSslMode);
    form->addRow(tr("Your na&me:"), m_userName);
    form->addRow(tr("Photo &library:"), rootRow);

    auto *test = new QPushButton(tr("Test Connection"), this);
    connect(test, &QPushButton::clicked, this, &CatalogueSettingsDialog::testConnection);
    auto *note = new QLabel(
        tr("A server catalogue is separate from your Local catalogue: it has its own reference "
           "trees and photos, and a working copy on this computer that you sync with the server. "
           "The password is stored in your OS settings, not encrypted."),
        this);
    note->setWordWrap(true);
    note->setEnabled(false);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(test);
    layout->addWidget(note);
    layout->addWidget(m_buttons);

    for (QLineEdit *e : {m_name, m_pgHost, m_pgDbName, m_pgUser, m_userName, m_libraryRoot})
        connect(e, &QLineEdit::textChanged, this, &CatalogueSettingsDialog::updateOkState);
    updateOkState();
}

void CatalogueSettingsDialog::browseForLibraryRoot()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Photo Library"),
                                                          m_libraryRoot->text());
    if (!dir.isEmpty())
        m_libraryRoot->setText(dir);
}

void CatalogueSettingsDialog::updateOkState()
{
    bool ok = true;
    for (QLineEdit *e : {m_pgHost, m_pgDbName, m_pgUser, m_userName, m_libraryRoot})
        ok = ok && !e->text().trimmed().isEmpty();
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
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
        db.setConnectOptions(QStringLiteral("sslmode=%1;connect_timeout=15").arg(m_pgSslMode->currentText()));
        if (db.open()) {
            db.close();
            QMessageBox::information(this, tr("Test Connection"), tr("Connected successfully."));
        } else {
            QMessageBox::warning(this, tr("Test Connection"),
                                 tr("Could not connect:\n%1").arg(db.lastError().text()));
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
}

Settings::CatalogueProfile CatalogueSettingsDialog::profile() const
{
    Settings::CatalogueProfile p = m_profile;
    p.server = true;
    p.name = m_name->text().trimmed().isEmpty() ? m_pgDbName->text().trimmed() : m_name->text().trimmed();
    p.pg.backend = CatalogueDescriptor::Backend::Postgres;
    p.pg.pgHost = m_pgHost->text().trimmed();
    p.pg.pgPort = m_pgPort->value();
    p.pg.pgDbName = m_pgDbName->text().trimmed();
    p.pg.pgUser = m_pgUser->text().trimmed();
    p.pg.pgPassword = m_pgPassword->text();
    p.pg.pgSslMode = m_pgSslMode->currentText();
    p.userName = m_userName->text().trimmed();
    p.libraryRoot = m_libraryRoot->text().trimmed();
    return p;
}

} // namespace pl
