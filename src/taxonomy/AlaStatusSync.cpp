#include "taxonomy/AlaStatusSync.h"

#include "net/HttpClient.h"
#include "taxonomy/TaxonomyStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace pl::taxonomy {

const QString AlaStatusSync::kBaseUrl = QStringLiteral("https://api.ala.org.au/specieslist/ws");

namespace {

QJsonDocument parseJson(const net::HttpResponse &resp, QString *error)
{
    if (!resp.error.isEmpty()) {
        *error = resp.error;
        return {};
    }
    if (!resp.ok()) {
        *error = QStringLiteral("HTTP %1").arg(resp.status);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &parseError);
    if (doc.isNull())
        *error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
    return doc;
}

} // namespace

AlaStatusSync::AlaStatusSync(net::HttpClient &http, TaxonomyStore &store, QString cacheDir,
                             QObject *parent)
    : QObject(parent), m_http(http), m_store(store), m_cacheDir(std::move(cacheDir)),
      m_baseUrl(kBaseUrl)
{
}

QString AlaStatusSync::cachePath() const
{
    return m_cacheDir.isEmpty() ? QString()
                                : QDir(m_cacheDir).filePath(QStringLiteral("ala-lists.json"));
}

// The kept copy of the lists, if there is one young enough to use.
bool AlaStatusSync::loadCache()
{
    const QString path = cachePath();
    if (path.isEmpty())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QDateTime fetchedAt =
        QDateTime::fromString(root.value(QStringLiteral("fetchedAt")).toString(), Qt::ISODate);
    if (!fetchedAt.isValid() || fetchedAt.daysTo(QDateTime::currentDateTimeUtc()) > kMaxAgeDays)
        return false;

    QList<List> lists;
    for (const QJsonValue &lv : root.value(QStringLiteral("lists")).toArray()) {
        const QJsonObject l = lv.toObject();
        List list{l.value(QStringLiteral("id")).toString(), l.value(QStringLiteral("name")).toString(),
                  l.value(QStringLiteral("place")).toInteger(), {}};
        for (const QJsonValue &iv : l.value(QStringLiteral("items")).toArray()) {
            const QJsonObject it = iv.toObject();
            QStringList names;
            for (const QJsonValue &n : it.value(QStringLiteral("names")).toArray())
                names << n.toString();
            list.items.append({names, it.value(QStringLiteral("status")).toString()});
        }
        if (!list.id.isEmpty() && list.placeInatId > 0)
            lists.append(list);
    }
    if (lists.isEmpty())
        return false;
    m_lists = lists;
    return true;
}

void AlaStatusSync::saveCache() const
{
    const QString path = cachePath();
    if (path.isEmpty() || !QDir().mkpath(m_cacheDir))
        return;
    QJsonArray lists;
    for (const List &list : m_lists) {
        QJsonArray items;
        for (const Item &item : list.items)
            items.append(QJsonObject{{QStringLiteral("names"), QJsonArray::fromStringList(item.names)},
                                     {QStringLiteral("status"), item.status}});
        lists.append(QJsonObject{{QStringLiteral("id"), list.id},
                                 {QStringLiteral("name"), list.name},
                                 {QStringLiteral("place"), list.placeInatId},
                                 {QStringLiteral("items"), items}});
    }
    const QJsonObject root{
        {QStringLiteral("fetchedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("lists"), lists}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();   // not fatal if it fails: the next run just downloads again
}

std::optional<qint64> AlaStatusSync::placeForRegion(const QString &region)
{
    // iNaturalist's places for Australia and its states and territories
    // (checked against its places API: each lies within Australia, 6744).
    static const QHash<QString, qint64> places = {
        {QStringLiteral("australia"), 6744},
        {QStringLiteral("australian capital territory"), 12986},
        {QStringLiteral("new south wales"), 6825},
        {QStringLiteral("northern territory"), 9994},
        {QStringLiteral("queensland"), 7308},
        {QStringLiteral("south australia"), 6899},
        {QStringLiteral("tasmania"), 6829},
        {QStringLiteral("victoria"), 7830},
        {QStringLiteral("western australia"), 6827},
    };
    const auto it = places.constFind(region.trimmed().toLower());
    if (it == places.constEnd())
        return std::nullopt;
    return *it;
}

void AlaStatusSync::start(bool forceDownload)
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_lists.clear();
    m_listIndex = 0;
    m_matched = 0;
    m_background = !forceDownload;

    m_downloading = forceDownload || !loadCache();
    if (!m_downloading) {
        // Matched on a later turn, so finished() never fires inside start().
        m_nameIndex = m_store.leafTaxaByFoldedName();
        QTimer::singleShot(0, this, [this] {
            if (!checkCancelled())
                fetchNextList();
        });
        return;
    }

    const auto priority =
        m_background ? net::HttpClient::Priority::Background : net::HttpClient::Priority::Normal;
    QUrl url(m_baseUrl + QStringLiteral("/speciesList"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("isThreatened"), QStringLiteral("eq:true"));
    q.addQueryItem(QStringLiteral("max"), QStringLiteral("100"));
    url.setQuery(q);
    m_http.get(url, [this](net::HttpResponse resp) {
        if (checkCancelled())
            return;
        QString error;
        const QJsonDocument doc = parseJson(resp, &error);
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        // Only authoritative lists for a region PhotoLife maps -- the state
        // and territory lists and EPBC, not user-uploaded or migratory-bird
        // agreement lists.
        for (const QJsonValue &v : doc.object().value(QStringLiteral("lists")).toArray()) {
            const QJsonObject l = v.toObject();
            if (!l.value(QStringLiteral("isAuthoritative")).toBool())
                continue;
            const auto place = placeForRegion(l.value(QStringLiteral("region")).toString());
            if (!place)
                continue;
            m_lists.append({l.value(QStringLiteral("dataResourceUid")).toString(),
                            l.value(QStringLiteral("listName")).toString(), *place});
        }
        if (m_lists.isEmpty()) {
            fail(QStringLiteral("ALA returned no state or national conservation lists"));
            return;
        }
        m_nameIndex = m_store.leafTaxaByFoldedName();
        fetchNextList();
    }, QString(), priority);
}

void AlaStatusSync::fetchNextList()
{
    if (m_listIndex >= m_lists.size()) {
        if (m_downloading)
            saveCache();
        m_running = false;
        m_nameIndex.clear();
        emit finished(true, QString(), int(m_lists.size()), m_matched);
        return;
    }
    emit progress(m_listIndex, int(m_lists.size()), m_lists.at(m_listIndex).name);
    if (m_downloading) {
        m_offset = 0;
        m_lists[m_listIndex].items.clear();
        fetchPage();
        return;
    }
    // From the kept copy: one list per event-loop turn, so the window stays
    // responsive and Cancel is noticed.
    storeList();
}

bool AlaStatusSync::checkCancelled()
{
    if (!m_cancelled)
        return false;
    fail(QStringLiteral("cancelled"));
    return true;
}

void AlaStatusSync::fetchPage()
{
    const List &list = m_lists.at(m_listIndex);
    QUrl url(m_baseUrl + QStringLiteral("/speciesListItems/") + list.id);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("max"), QString::number(kPageSize));
    q.addQueryItem(QStringLiteral("offset"), QString::number(m_offset));   // a multiple of max
    q.addQueryItem(QStringLiteral("includeKVP"), QStringLiteral("true"));
    url.setQuery(q);
    m_http.get(url, [this](net::HttpResponse resp) {
        if (checkCancelled())
            return;
        QString error;
        const QJsonDocument doc = parseJson(resp, &error);
        if (!error.isEmpty()) {
            fail(QStringLiteral("%1: %2").arg(m_lists.at(m_listIndex).name, error));
            return;
        }
        const QJsonArray items = doc.array();
        for (const QJsonValue &v : items) {
            const QJsonObject it = v.toObject();
            QStringList names;
            for (const char *key : {"scientificName", "name"}) {
                const QString n = it.value(QLatin1String(key)).toString().trimmed();
                if (!n.isEmpty() && !names.contains(n))
                    names << n;
            }
            QString status;
            for (const QJsonValue &kv : it.value(QStringLiteral("kvpValues")).toArray()) {
                const QJsonObject pair = kv.toObject();
                if (pair.value(QStringLiteral("key")).toString() == QLatin1String("status")) {
                    status = pair.value(QStringLiteral("value")).toString().trimmed();
                    break;
                }
            }
            if (!names.isEmpty() && !status.isEmpty())
                m_lists[m_listIndex].items.append({names, status});
        }
        if (items.size() == kPageSize) {
            m_offset += kPageSize;
            fetchPage();
            return;
        }
        storeList();
    }, QString(), m_background ? net::HttpClient::Priority::Background
                               : net::HttpClient::Priority::Normal);
}

void AlaStatusSync::storeList()
{
    const List &list = m_lists.at(m_listIndex);
    QList<TaxonomyStore::SourcedFact> rows;
    for (const Item &item : list.items) {
        for (const QString &name : item.names) {
            const auto it = m_nameIndex.constFind(TaxonomyStore::foldListName(name));
            if (it != m_nameIndex.constEnd()) {
                rows.append({*it, list.placeInatId, item.status});
                break;
            }
        }
    }
    const int written =
        m_store.replaceStatusesFromSource(QStringLiteral("ALA %1").arg(list.id), rows);
    if (written < 0) {
        fail(QStringLiteral("could not save %1").arg(list.name));
        return;
    }
    m_matched += written;
    ++m_listIndex;
    if (m_downloading) {
        fetchNextList();
        return;
    }
    QTimer::singleShot(0, this, [this] {
        if (!checkCancelled())
            fetchNextList();
    });
}

void AlaStatusSync::fail(const QString &error)
{
    m_running = false;
    m_nameIndex.clear();
    emit finished(false, error, int(m_lists.size()), m_matched);
}

} // namespace pl::taxonomy
