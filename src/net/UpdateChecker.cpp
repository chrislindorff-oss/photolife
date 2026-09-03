#include "net/UpdateChecker.h"

#include "net/HttpClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

namespace pl::net {
namespace {

// The leading "x.y.z" of a version string as integers.
QList<int> numericParts(const QString &version)
{
    QList<int> parts;
    static const QRegularExpression head(
        QStringLiteral(R"(^\s*v?(\d+(?:\.\d+)*)(?![0-9A-Za-z]))"));
    const auto m = head.match(version);
    if (!m.hasMatch())
        return parts;
    for (const QString &p : m.captured(1).split(QLatin1Char('.')))
        parts << p.toInt();
    return parts;
}

// True when `version` has a dev suffix ("0.1.0-5-g1a2b3c", "1.0.0-rc1").
bool hasDevSuffix(const QString &version)
{
    static const QRegularExpression suffix(QStringLiteral(R"(^\s*v?\d+(?:\.\d+)*[-+])"));
    return suffix.match(version).hasMatch();
}

} // namespace

UpdateChecker::UpdateChecker(HttpClient &http, QObject *parent)
    : QObject(parent), m_http(http)
{
}

bool UpdateChecker::isNewer(const QString &candidate, const QString &current)
{
    const QList<int> a = numericParts(candidate);
    const QList<int> b = numericParts(current);
    if (a.isEmpty())
        return false;          // no sensible release number to offer
    if (b.isEmpty())
        return true;           // current is a bare hash — any release is newer

    for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
        const int av = i < a.size() ? a.at(i) : 0;
        const int bv = i < b.size() ? b.at(i) : 0;
        if (av != bv)
            return av > bv;
    }
    // Same release numbers: a plain release beats a local dev build of it.
    return hasDevSuffix(current) && !hasDevSuffix(candidate);
}

void UpdateChecker::check()
{
    if (m_repo.isEmpty()) {
        emit checkFailed(tr("no update repository configured"));
        return;
    }

    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(m_repo));
    m_http.get(url, [this](HttpResponse resp) {
        if (!resp.error.isEmpty()) {
            emit checkFailed(resp.error);
            return;
        }
        if (resp.status == 404) {
            emit checkFailed(tr("no releases published yet"));
            return;
        }
        if (!resp.ok()) {
            emit checkFailed(tr("update check failed (HTTP %1)").arg(resp.status));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(resp.body).object();
        const QString tag = obj.value(QStringLiteral("tag_name")).toString();
        const QString htmlUrl = obj.value(QStringLiteral("html_url")).toString();
        if (tag.isEmpty()) {
            emit checkFailed(tr("could not read the latest release"));
            return;
        }

        if (isNewer(tag, m_current))
            emit updateAvailable(tag, htmlUrl);
        else
            emit upToDate(m_current);
    });
}

} // namespace pl::net
