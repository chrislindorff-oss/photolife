#include "ui/CatalogueSettingsDialog.h"

#include "libsync/GoogleAuth.h"
#include "libsync/GoogleDriveStore.h"
#include "libsync/PhotoSyncService.h"
#include "libsync/S3Store.h"

#include <QApplication>
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
        tr("Where this catalogue's photos are on this computer. With photo storage below, "
           "PhotoLife keeps a copy of the shared photos here; otherwise it's the folder you "
           "share some other way (e.g. a synced Google Drive folder). Photo locations are "
           "shared relative to it, so each collaborator can keep it wherever suits them."));
    auto *browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &CatalogueSettingsDialog::browseForLibraryRoot);
    auto *rootRow = new QHBoxLayout;
    rootRow->addWidget(m_libraryRoot, 1);
    rootRow->addWidget(browse);

    // Photo storage: how the photos themselves get to each collaborator.
    m_photoStore = new QComboBox(this);
    m_photoStore->setObjectName(QStringLiteral("photoStore"));
    m_photoStore->addItem(tr("Shared some other way (e.g. a synced cloud folder)"),
                          QString::fromLatin1(Settings::kPhotoStoreNone));
    m_photoStore->addItem(tr("PhotoLife syncs them — S3-compatible storage"),
                          QString::fromLatin1(Settings::kPhotoStoreS3));
    m_photoStore->addItem(tr("PhotoLife syncs them — a shared Google Drive folder"),
                          QString::fromLatin1(Settings::kPhotoStoreDrive));
    const int store = m_photoStore->findData(profile.photoStore);
    m_photoStore->setCurrentIndex(store >= 0 ? store : 0);

    // Photos deleted from the library folder here: online too, or not.
    m_whenDeletedHere = new QComboBox(this);
    m_whenDeletedHere->setObjectName(QStringLiteral("whenDeletedHere"));
    m_whenDeletedHere->addItem(tr("Ask me after each sync"),
                               QString::fromLatin1(libsync::PhotoSyncService::kDeletedHereAsk));
    m_whenDeletedHere->addItem(tr("Remove them online too"),
                               QString::fromLatin1(libsync::PhotoSyncService::kDeletedHereRemove));
    m_whenDeletedHere->addItem(tr("Keep them online"),
                               QString::fromLatin1(libsync::PhotoSyncService::kDeletedHereKeep));
    const int deleted = m_whenDeletedHere->findData(profile.photosWhenDeletedHere);
    m_whenDeletedHere->setCurrentIndex(deleted >= 0 ? deleted : 0);
    m_whenDeletedHere->setToolTip(
        tr("When you delete photos from the library folder on this computer, whether a sync also "
           "takes them out of the photo storage — and so off everyone else's computers. Removed "
           "photos go to Google Drive's bin (kept 30 days), or under trash/ in S3 storage. "
           "PhotoLife never deletes photos on this computer, and won't remove anything online "
           "when so many photos vanish at once that it looks like a disconnected drive."));

    m_s3Endpoint = new QLineEdit(profile.s3.endpoint, this);
    m_s3Endpoint->setObjectName(QStringLiteral("s3Endpoint"));
    m_s3Endpoint->setPlaceholderText(tr("e.g. https://<account id>.r2.cloudflarestorage.com"));
    m_s3Region = new QLineEdit(profile.s3.region.isEmpty() ? QStringLiteral("auto") : profile.s3.region, this);
    m_s3Region->setToolTip(tr("\"auto\" for Cloudflare R2; e.g. us-west-004 for Backblaze B2."));
    m_s3Bucket = new QLineEdit(profile.s3.bucket, this);
    m_s3Bucket->setObjectName(QStringLiteral("s3Bucket"));
    m_s3Prefix = new QLineEdit(profile.s3.prefix, this);
    m_s3Prefix->setPlaceholderText(tr("optional, e.g. nature-club"));
    m_s3Prefix->setToolTip(tr("A folder within the bucket, if it holds other things too."));
    m_s3AccessKey = new QLineEdit(profile.s3.accessKey, this);
    m_s3AccessKey->setObjectName(QStringLiteral("s3AccessKey"));
    m_s3Secret = new QLineEdit(profile.s3.secretKey, this);
    m_s3Secret->setObjectName(QStringLiteral("s3Secret"));
    m_s3Secret->setEchoMode(QLineEdit::Password);
    auto *testPhotos = new QPushButton(tr("Test Photo Storage"), this);
    connect(testPhotos, &QPushButton::clicked, this, &CatalogueSettingsDialog::testPhotoStore);

    m_s3Fields = new QWidget(this);
    auto *s3Form = new QFormLayout(m_s3Fields);
    s3Form->setContentsMargins(0, 0, 0, 0);
    s3Form->addRow(tr("&Endpoint:"), m_s3Endpoint);
    s3Form->addRow(tr("&Region:"), m_s3Region);
    s3Form->addRow(tr("&Bucket:"), m_s3Bucket);
    s3Form->addRow(tr("&Folder in bucket:"), m_s3Prefix);
    s3Form->addRow(tr("&Access key ID:"), m_s3AccessKey);
    s3Form->addRow(tr("Secret access &key:"), m_s3Secret);
    s3Form->addRow(QString(), testPhotos);
    // Google Drive.
    m_driveRefreshToken = profile.drive.refreshToken;
    m_driveAccount = profile.drive.account;
    m_driveClientId = new QLineEdit(profile.drive.clientId, this);
    m_driveClientId->setObjectName(QStringLiteral("driveClientId"));
    m_driveClientId->setPlaceholderText(tr("…apps.googleusercontent.com"));
    m_driveClientId->setToolTip(tr("A \"Desktop app\" OAuth client from Google Cloud Console — "
                                   "see Help ▸ Sharing the photos. Everyone can use the same one."));
    m_driveClientSecret = new QLineEdit(profile.drive.clientSecret, this);
    m_driveClientSecret->setObjectName(QStringLiteral("driveClientSecret"));
    m_driveClientSecret->setEchoMode(QLineEdit::Password);
    m_driveFolder = new QLineEdit(profile.drive.folderId, this);
    m_driveFolder->setObjectName(QStringLiteral("driveFolder"));
    m_driveFolder->setPlaceholderText(tr("the shared folder's link, or its ID"));
    m_driveAccountLabel = new QLabel(this);
    m_driveSignIn = new QPushButton(this);
    connect(m_driveSignIn, &QPushButton::clicked, this, &CatalogueSettingsDialog::signInWithGoogle);
    auto *signInRow = new QHBoxLayout;
    signInRow->addWidget(m_driveAccountLabel, 1);
    signInRow->addWidget(m_driveSignIn);
    auto *testDrive = new QPushButton(tr("Test Photo Storage"), this);
    connect(testDrive, &QPushButton::clicked, this, &CatalogueSettingsDialog::testPhotoStore);
    auto *driveNote = new QLabel(
        tr("Each person signs in with their own Google account, which needs access to the "
           "shared folder. Until Google has reviewed your OAuth client, it's limited to the "
           "test users you list in Cloud Console, and sign-ins last 7 days — unless your "
           "group is in one Google Workspace and the client is \"Internal\"."),
        this);
    driveNote->setWordWrap(true);
    driveNote->setEnabled(false);
    // A sign-in belongs to one client: a new client needs a new sign-in.
    connect(m_driveClientId, &QLineEdit::textEdited, this, [this] {
        m_driveRefreshToken.clear();
        m_driveAccount.clear();
        updateDriveAccount();
    });

    m_driveFields = new QWidget(this);
    auto *driveForm = new QFormLayout(m_driveFields);
    driveForm->setContentsMargins(0, 0, 0, 0);
    driveForm->addRow(tr("OAuth &client ID:"), m_driveClientId);
    driveForm->addRow(tr("Client s&ecret:"), m_driveClientSecret);
    driveForm->addRow(tr("Shared &folder:"), m_driveFolder);
    driveForm->addRow(tr("Google account:"), signInRow);
    driveForm->addRow(QString(), testDrive);
    driveForm->addRow(driveNote);
    updateDriveAccount();

    connect(m_photoStore, &QComboBox::currentIndexChanged, this,
            &CatalogueSettingsDialog::updatePhotoStoreFields);

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
    form->addRow(tr("Photo s&torage:"), m_photoStore);
    form->addRow(m_s3Fields);
    form->addRow(m_driveFields);
    form->addRow(tr("Photos deleted &here:"), m_whenDeletedHere);

    auto *test = new QPushButton(tr("Test Connection"), this);
    connect(test, &QPushButton::clicked, this, &CatalogueSettingsDialog::testConnection);
    auto *note = new QLabel(
        tr("A server catalogue is separate from your Local catalogue: it has its own reference "
           "trees and photos, and a working copy on this computer that you sync with the server. "
           "The passwords and keys are stored in your OS settings, not encrypted."),
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

    for (QLineEdit *e : {m_name, m_pgHost, m_pgDbName, m_pgUser, m_userName, m_libraryRoot,
                         m_s3Endpoint, m_s3Bucket, m_s3AccessKey, m_s3Secret,
                         m_driveClientId, m_driveFolder})
        connect(e, &QLineEdit::textChanged, this, &CatalogueSettingsDialog::updateOkState);
    updatePhotoStoreFields();
}

void CatalogueSettingsDialog::updatePhotoStoreFields()
{
    const QString store = m_photoStore->currentData().toString();
    m_s3Fields->setVisible(store == QLatin1String(Settings::kPhotoStoreS3));
    m_driveFields->setVisible(store == QLatin1String(Settings::kPhotoStoreDrive));
    m_whenDeletedHere->setEnabled(store != QLatin1String(Settings::kPhotoStoreNone));
    updateOkState();
    adjustSize();
}

libsync::S3Config CatalogueSettingsDialog::s3Config() const
{
    libsync::S3Config c;
    c.endpoint = m_s3Endpoint->text().trimmed();
    c.region = m_s3Region->text().trimmed().isEmpty() ? QStringLiteral("auto")
                                                      : m_s3Region->text().trimmed();
    c.bucket = m_s3Bucket->text().trimmed();
    c.prefix = m_s3Prefix->text().trimmed();
    c.accessKey = m_s3AccessKey->text().trimmed();
    c.secretKey = m_s3Secret->text();
    return c;
}

libsync::DriveConfig CatalogueSettingsDialog::driveConfig() const
{
    libsync::DriveConfig c;
    c.clientId = m_driveClientId->text().trimmed();
    c.clientSecret = m_driveClientSecret->text().trimmed();
    c.folderId = libsync::DriveConfig::folderIdFrom(m_driveFolder->text());
    c.refreshToken = m_driveRefreshToken;
    c.account = m_driveAccount;
    return c;
}

void CatalogueSettingsDialog::updateDriveAccount()
{
    const bool signedIn = !m_driveRefreshToken.isEmpty();
    m_driveAccountLabel->setText(!signedIn ? tr("Not signed in")
                                 : m_driveAccount.isEmpty() ? tr("Signed in")
                                                            : tr("Signed in as %1").arg(m_driveAccount));
    m_driveSignIn->setText(signedIn ? tr("Sign In Again…") : tr("Sign In with Google…"));
    m_driveSignIn->setEnabled(true);
    updateOkState();
}

void CatalogueSettingsDialog::signInWithGoogle()
{
    const libsync::DriveConfig config = driveConfig();
    if (!config.canSignIn()) {
        QMessageBox::information(this, tr("Sign In with Google"),
                                 tr("Enter the OAuth client ID (and secret) first."));
        return;
    }
    if (!m_auth) {
        m_auth = new libsync::GoogleAuth(this);
        connect(m_auth, &libsync::GoogleAuth::finished, this,
                [this](const libsync::GoogleAuth::Result &result) {
                    if (result.ok) {
                        m_driveRefreshToken = result.refreshToken;
                        m_driveAccount = result.account;
                    } else {
                        QMessageBox::warning(this, tr("Sign In with Google"),
                                             tr("Not signed in: %1").arg(result.error));
                    }
                    updateDriveAccount();
                });
    }
    if (!m_auth->start(config)) {
        QMessageBox::warning(this, tr("Sign In with Google"),
                             tr("Couldn't start signing in (no browser, or no local port)."));
        return;
    }
    m_driveSignIn->setEnabled(false);
    m_driveAccountLabel->setText(tr("Finish signing in in your browser…"));
}

void CatalogueSettingsDialog::testPhotoStore()
{
    const bool drive = m_photoStore->currentData().toString() == QLatin1String(Settings::kPhotoStoreDrive);
    std::unique_ptr<libsync::LibraryStore> store;
    if (drive)
        store = std::make_unique<libsync::GoogleDriveStore>(driveConfig());
    else
        store = std::make_unique<libsync::S3Store>(s3Config());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = store->testConnection();
    QApplication::restoreOverrideCursor();
    if (ok)
        QMessageBox::information(this, tr("Test Photo Storage"),
                                 drive ? tr("Opened the shared Drive folder successfully.")
                                       : tr("Connected to the bucket successfully."));
    else
        QMessageBox::warning(this, tr("Test Photo Storage"),
                             (drive ? tr("Could not use the Drive folder:\n%1")
                                    : tr("Could not use the bucket:\n%1"))
                                 .arg(store->error()));
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
    if (!m_buttons)
        return;   // still being built
    bool ok = true;
    for (QLineEdit *e : {m_pgHost, m_pgDbName, m_pgUser, m_userName, m_libraryRoot})
        ok = ok && !e->text().trimmed().isEmpty();
    if (m_photoStore->currentData().toString() == QLatin1String(Settings::kPhotoStoreS3))
        ok = ok && s3Config().isComplete();
    if (m_photoStore->currentData().toString() == QLatin1String(Settings::kPhotoStoreDrive))
        ok = ok && driveConfig().isComplete();
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
    p.photoStore = m_photoStore->currentData().toString();
    p.s3 = s3Config();
    p.drive = driveConfig();
    p.photosWhenDeletedHere = m_whenDeletedHere->currentData().toString();
    return p;
}

} // namespace pl
