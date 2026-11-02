#include "ui/InatTokenGetter.h"

#include "app/Application.h"
#include "inat/InatToken.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QLocale>
#include <QTimer>
#include <QUrl>

namespace pl {

void InatTokenGetter::start(Application &app)
{
    auto *getter = app.findChild<InatTokenGetter *>(QString(), Qt::FindDirectChildrenOnly);
    if (!getter)
        getter = new InatTokenGetter(app);
    getter->m_giveUp->start();
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(inat::kApiTokenPage)));
}

InatTokenGetter::InatTokenGetter(Application &app)
    : QObject(&app), m_app(app)
{
    m_giveUp = new QTimer(this);
    m_giveUp->setSingleShot(true);
    m_giveUp->setInterval(5 * 60 * 1000);
    connect(m_giveUp, &QTimer::timeout, this, &QObject::deleteLater);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &InatTokenGetter::clipboardChanged);
}

void InatTokenGetter::clipboardChanged()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    const QString text = clipboard->text();
    const QString token = inat::extractApiToken(text);
    if (token.isEmpty())
        return;
    m_app.setInatApiToken(token);
    // Not left lying about for whatever is pasted next.
    if (clipboard->text() == text)
        clipboard->clear();
    deleteLater();
}

bool InatTokenGetter::isUsable(const QString &token)
{
    if (token.isEmpty())
        return false;
    const std::optional<QDateTime> expiry = inat::apiTokenExpiry(token);
    return !expiry || *expiry > QDateTime::currentDateTimeUtc();
}

QString InatTokenGetter::status(const QString &token)
{
    if (token.isEmpty())
        return {};
    const std::optional<QDateTime> expiry = inat::apiTokenExpiry(token);
    if (!expiry)
        return tr("Token set");
    if (*expiry <= QDateTime::currentDateTimeUtc())
        return tr("Your token has expired");
    const QDateTime local = expiry->toLocalTime();
    const QString time = QLocale().toString(local.time(), QLocale::ShortFormat);
    const qint64 days = QDate::currentDate().daysTo(local.date());
    return days == 0   ? tr("Signed in until %1 today").arg(time)
           : days == 1 ? tr("Signed in until %1 tomorrow").arg(time)
                       : tr("Signed in until %1").arg(QLocale().toString(local, QLocale::ShortFormat));
}

} // namespace pl
