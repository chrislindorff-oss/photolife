#pragma once

#include <QRegularExpression>
#include <QString>

namespace pl::libsync {

// A shared Google Drive folder holding a photo library, and the Google sign-in
// to reach it. The OAuth client (ID and secret) is a "Desktop app" client the
// group creates once in Google Cloud Console; for an installed app the
// "secret" isn't secret, and the sign-in is protected by PKCE.
struct DriveConfig
{
    QString clientId;
    QString clientSecret;
    QString folderId;        // the shared folder (see folderIdFrom())
    QString refreshToken;    // from signing in
    QString account;         // who signed in, for display

    // Where Google is -- tests point these at a local fake.
    QString apiBase = QStringLiteral("https://www.googleapis.com");
    QString tokenUrl = QStringLiteral("https://oauth2.googleapis.com/token");
    QString authUrl = QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth");

    static constexpr const char *kScope = "https://www.googleapis.com/auth/drive";

    bool canSignIn() const { return !clientId.trimmed().isEmpty(); }
    bool isComplete() const
    {
        return canSignIn() && !folderId.trimmed().isEmpty() && !refreshToken.isEmpty();
    }

    // A folder ID from what someone pasted: the ID itself, or the folder's
    // link (".../drive/folders/<id>?usp=sharing", "...?id=<id>").
    static QString folderIdFrom(const QString &text)
    {
        const QString t = text.trimmed();
        static const QRegularExpression inPath(QStringLiteral("/folders/([A-Za-z0-9_-]+)"));
        static const QRegularExpression inQuery(QStringLiteral("[?&]id=([A-Za-z0-9_-]+)"));
        if (const auto m = inPath.match(t); m.hasMatch())
            return m.captured(1);
        if (const auto m = inQuery.match(t); m.hasMatch())
            return m.captured(1);
        return t;
    }
};

} // namespace pl::libsync
