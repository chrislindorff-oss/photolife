#pragma once

#include "settings/Settings.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QDialogButtonBox;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace pl::libsync {
class GoogleAuth;
}

namespace pl {

// Adds or edits one Server catalogue profile (see Settings::CatalogueProfile):
// its display name, the Postgres connection, your name there, this machine's
// photo library folder for it, and where the shared photos are kept online
// if PhotoLife syncs them itself.
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
    void updatePhotoStoreFields();
    void testPhotoStore();
    libsync::S3Config s3Config() const;
    libsync::DriveConfig driveConfig() const;
    void signInWithGoogle();
    void updateDriveAccount();

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
    QComboBox *m_photoStore;
    QWidget *m_s3Fields;
    QLineEdit *m_s3Endpoint;
    QLineEdit *m_s3Region;
    QLineEdit *m_s3Bucket;
    QLineEdit *m_s3Prefix;
    QLineEdit *m_s3AccessKey;
    QLineEdit *m_s3Secret;
    QWidget *m_driveFields;
    QLineEdit *m_driveClientId;
    QLineEdit *m_driveClientSecret;
    QLineEdit *m_driveFolder;
    QLabel *m_driveAccountLabel;
    QPushButton *m_driveSignIn;
    QString m_driveRefreshToken;
    QString m_driveAccount;
    libsync::GoogleAuth *m_auth = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace pl
