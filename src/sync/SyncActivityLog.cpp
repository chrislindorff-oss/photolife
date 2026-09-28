#include "sync/SyncActivityLog.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace pl::sync {

bool SyncActivityLog::append(const SyncActivityEntry &entry) const
{
    QFile f(m_path);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("at"), entry.at.toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("action"), entry.action);
    o.insert(QStringLiteral("ok"), entry.ok);
    if (!entry.detail.isEmpty())
        o.insert(QStringLiteral("detail"), entry.detail);
    if (!entry.error.isEmpty())
        o.insert(QStringLiteral("error"), entry.error);
    if (entry.durationMs > 0)
        o.insert(QStringLiteral("ms"), double(entry.durationMs));
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
    return true;
}

QList<SyncActivityEntry> SyncActivityLog::read(int limit) const
{
    QList<SyncActivityEntry> entries;
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return entries;
    while (!f.atEnd()) {
        const QJsonObject o = QJsonDocument::fromJson(f.readLine()).object();
        if (o.isEmpty())
            continue;   // a damaged line: skip it, keep the rest
        SyncActivityEntry e;
        e.at = QDateTime::fromString(o.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs);
        e.action = o.value(QStringLiteral("action")).toString();
        e.ok = o.value(QStringLiteral("ok")).toBool(true);
        e.detail = o.value(QStringLiteral("detail")).toString();
        e.error = o.value(QStringLiteral("error")).toString();
        e.durationMs = qint64(o.value(QStringLiteral("ms")).toDouble());
        entries.append(e);
    }
    std::reverse(entries.begin(), entries.end());
    if (entries.size() > limit)
        entries.resize(limit);
    return entries;
}

} // namespace pl::sync
