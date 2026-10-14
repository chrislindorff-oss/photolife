#pragma once

#include "libsync/DriveConfig.h"

#include <QObject>
#include <QPointer>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;
class QTimer;

namespace pl::libsync {

// "Sign in with Google" for a desktop app: Google's sign-in page opens in the
// browser, which comes back to a one-off listener on 127.0.0.1 with a code;
// that code (with its PKCE verifier) is swapped for a refresh token, which is
// what GoogleDriveStore needs from then on. Asynchronous, for the GUI thread.
class GoogleAuth : public QObject
{
    Q_OBJECT

public:
    struct Result
    {
        bool ok = false;
        QString refreshToken;
        QString account;   // the signed-in Google account's email
        QString error;
    };

    explicit GoogleAuth(QObject *parent = nullptr);
    ~GoogleAuth() override;

    // How the sign-in page is shown (default: the system browser).
    void setOpenUrl(std::function<bool(const QUrl &)> open) { m_openUrl = std::move(open); }
    // Gives up after this long without an answer (default 5 minutes).
    void setTimeout(int ms) { m_timeoutMs = ms; }

    // Starts signing in; finished() reports it. False if it can't start.
    bool start(const DriveConfig &config);
    void cancel();

    // PKCE: the S256 challenge for a verifier (base64url, no padding).
    static QString pkceChallenge(const QString &verifier);

signals:
    void finished(const pl::libsync::GoogleAuth::Result &result);

private:
    void onConnection();
    void onRequest(QTcpSocket *socket);
    void exchange(const QString &code);
    void fetchAccount(const QString &accessToken, const QString &refreshToken);
    void finish(Result result);

    DriveConfig m_config;
    std::function<bool(const QUrl &)> m_openUrl;
    int m_timeoutMs = 5 * 60 * 1000;
    QTcpServer *m_server = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_timer = nullptr;
    QString m_verifier;
    QString m_state;
    QString m_redirectUri;
    bool m_done = true;
};

} // namespace pl::libsync

Q_DECLARE_METATYPE(pl::libsync::GoogleAuth::Result)
