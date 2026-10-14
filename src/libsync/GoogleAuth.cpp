#include "libsync/GoogleAuth.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

namespace pl::libsync {

namespace {

QString randomToken(int length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString out;
    out.reserve(length);
    for (int i = 0; i < length; ++i)
        out += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(int(sizeof(alphabet) - 1))]);
    return out;
}

QByteArray page(const QString &title, const QString &text)
{
    const QByteArray body = QStringLiteral(
                                "<!doctype html><html><head><meta charset=\"utf-8\"><title>PhotoLife</title>"
                                "</head><body style=\"font-family:sans-serif;margin:3em\"><h2>%1</h2><p>%2</p>"
                                "</body></html>")
                                .arg(title.toHtmlEscaped(), text.toHtmlEscaped())
                                .toUtf8();
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
           "Content-Length: "
           + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

QString googleError(const QByteArray &body, const QString &fallback)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (o.value(QStringLiteral("error")).isString()) {
        const QString description = o.value(QStringLiteral("error_description")).toString();
        return description.isEmpty() ? o.value(QStringLiteral("error")).toString() : description;
    }
    const QString message = o.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
    return message.isEmpty() ? fallback : message;
}

} // namespace

GoogleAuth::GoogleAuth(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<pl::libsync::GoogleAuth::Result>();
    m_openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
}

GoogleAuth::~GoogleAuth() = default;

QString GoogleAuth::pkceChallenge(const QString &verifier)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool GoogleAuth::start(const DriveConfig &config)
{
    if (!m_done)
        return false;
    m_config = config;
    delete m_server;
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0))
        return false;
    connect(m_server, &QTcpServer::newConnection, this, &GoogleAuth::onConnection);
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, [this] {
            finish({false, {}, {}, tr("No answer from the browser — sign-in timed out.")});
        });
    }

    m_done = false;
    m_verifier = randomToken(64);
    m_state = randomToken(24);
    m_redirectUri = QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort());

    QUrl url(config.authUrl);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), config.clientId.trimmed());
    query.addQueryItem(QStringLiteral("redirect_uri"), m_redirectUri);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("scope"), QString::fromLatin1(DriveConfig::kScope));
    query.addQueryItem(QStringLiteral("code_challenge"), pkceChallenge(m_verifier));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
    query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));   // always a refresh token
    query.addQueryItem(QStringLiteral("state"), m_state);
    url.setQuery(query);

    m_timer->start(m_timeoutMs);
    if (!m_openUrl(url)) {
        finish({false, {}, {}, tr("Couldn't open the browser to sign in.")});
        return false;
    }
    return true;
}

void GoogleAuth::cancel()
{
    if (!m_done)
        finish({false, {}, {}, tr("Cancelled")});
}

void GoogleAuth::onConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onRequest(socket); });
    }
}

void GoogleAuth::onRequest(QTcpSocket *socket)
{
    if (!socket->canReadLine())
        return;
    const QList<QByteArray> requestLine = socket->readLine().trimmed().split(' ');
    socket->readAll();
    if (requestLine.size() < 2 || requestLine.at(0) != "GET") {
        socket->write("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        socket->disconnectFromHost();
        return;
    }
    const QUrl target(QString::fromLatin1(requestLine.at(1)));
    const QUrlQuery query(target);
    if (target.path() != QLatin1String("/") || m_done) {
        socket->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        socket->disconnectFromHost();
        return;
    }

    const QString error = query.queryItemValue(QStringLiteral("error"));
    const QString code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    const bool stateOk = query.queryItemValue(QStringLiteral("state")) == m_state;
    if (!error.isEmpty() || code.isEmpty() || !stateOk) {
        socket->write(page(tr("Not signed in"), tr("You can close this tab and try again in PhotoLife.")));
        socket->disconnectFromHost();
        finish({false, {}, {},
                !error.isEmpty() ? tr("Google said: %1").arg(error)
                                 : tr("The sign-in didn't come back as expected.")});
        return;
    }
    socket->write(page(tr("Signed in to PhotoLife"), tr("You can close this tab and go back to PhotoLife.")));
    socket->disconnectFromHost();
    m_server->close();
    exchange(code);
}

void GoogleAuth::exchange(const QString &code)
{
    auto field = [](const char *name, const QString &value) {
        return QByteArray(name) + '=' + QUrl::toPercentEncoding(value);
    };
    const QByteArray form = field("code", code) + '&'
                            + field("client_id", m_config.clientId.trimmed()) + '&'
                            + field("client_secret", m_config.clientSecret.trimmed()) + '&'
                            + field("redirect_uri", m_redirectUri) + '&'
                            + field("grant_type", QStringLiteral("authorization_code")) + '&'
                            + field("code_verifier", m_verifier);
    QNetworkRequest request{QUrl(m_config.tokenUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply = m_nam->post(request, form);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        const QString refresh = o.value(QStringLiteral("refresh_token")).toString();
        const QString access = o.value(QStringLiteral("access_token")).toString();
        if (refresh.isEmpty() || access.isEmpty()) {
            finish({false, {}, {},
                    tr("Google didn't give PhotoLife access: %1")
                        .arg(googleError(body, reply->errorString()))});
            return;
        }
        fetchAccount(access, refresh);
    });
}

void GoogleAuth::fetchAccount(const QString &accessToken, const QString &refreshToken)
{
    QNetworkRequest request(QUrl(m_config.apiBase + QStringLiteral("/drive/v3/about?fields=user(emailAddress)")));
    request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, refreshToken] {
        reply->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString email =
            o.value(QStringLiteral("user")).toObject().value(QStringLiteral("emailAddress")).toString();
        finish({true, refreshToken, email, {}});   // the account is only for display
    });
}

void GoogleAuth::finish(Result result)
{
    if (m_done)
        return;
    m_done = true;
    if (m_timer)
        m_timer->stop();
    if (m_server)
        m_server->close();
    emit finished(result);
}

} // namespace pl::libsync
