#include "inat/PhotoIdentifier.h"

#include "raw/RawPreview.h"
#include "thumb/ThumbnailCache.h"

#include <QBuffer>
#include <QFutureWatcher>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QtConcurrent>

#include <algorithm>

namespace pl::inat {

namespace {

// Which of these iNat taxa are in a reference tree: just them, not the whole
// catalogue (LibraryPresence reads every capture too).
QSet<qint64> inAnyTree(const QString &connectionName, const QList<qint64> &inatIds)
{
    QSet<qint64> out;
    QStringList ids;
    for (qint64 id : inatIds)
        ids << QString::number(id);
    if (ids.isEmpty())
        return out;
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.setForwardOnly(true);
    if (q.exec(QStringLiteral("SELECT DISTINCT t.inat_id FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id "
                              "WHERE t.inat_id IN (%1)")
                   .arg(ids.join(QLatin1Char(','))))) {
        while (q.next())
            out.insert(q.value(0).toLongLong());
    }
    return out;
}

} // namespace

PhotoIdentifier::PhotoIdentifier(net::INatClient &inat, QString connectionName, QObject *parent)
    : QObject(parent), m_inat(inat), m_connectionName(std::move(connectionName))
{
}

QString PhotoIdentifier::cacheKey(const QString &key, const std::optional<QRectF> &crop)
{
    if (!crop)
        return key;
    return QStringLiteral("%1#%2,%3,%4,%5")
        .arg(key)
        .arg(crop->x(), 0, 'f', 4)
        .arg(crop->y(), 0, 'f', 4)
        .arg(crop->width(), 0, 'f', 4)
        .arg(crop->height(), 0, 'f', 4);
}

std::optional<Identification> PhotoIdentifier::known(const QString &key, const std::optional<QRectF> &crop) const
{
    const auto it = m_known.constFind(cacheKey(key, crop));
    if (it == m_known.constEnd())
        return std::nullopt;
    return *it;
}

QByteArray PhotoIdentifier::renderJpeg(const IdentifyRequest &request, int longestEdge, int quality)
{
    // A small part is rendered from a bigger picture, so what's sent still
    // has the detail a whole photo would.
    int longest = longestEdge;
    if (request.crop) {
        const double part = std::max(request.crop->width(), request.crop->height());
        if (part > 0)
            longest = std::min(6000, int(longestEdge / part));
    }
    QImage image = thumb::ThumbnailCache::render(request.path, longest, raw::extractPreview);
    if (image.isNull())
        return {};
    if (request.crop) {
        const QRectF r(request.crop->x() * image.width(), request.crop->y() * image.height(),
                       request.crop->width() * image.width(), request.crop->height() * image.height());
        image = image.copy(r.toAlignedRect().intersected(image.rect()));
        if (image.isNull())
            return {};
    }
    if (std::max(image.width(), image.height()) > longestEdge)
        image = image.scaled(longestEdge, longestEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPG", quality);
    return jpeg;
}

void PhotoIdentifier::identify(const IdentifyRequest &request)
{
    if (const std::optional<Identification> done = known(request.key, request.crop)) {
        emit finished(request.key, request.crop, *done);
        return;
    }
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, request] {
        const QByteArray jpeg = watcher->result();
        watcher->deleteLater();
        if (jpeg.isEmpty()) {
            Identification failed;
            failed.error = tr("The photo couldn't be read.");
            emit finished(request.key, request.crop, failed);
            return;
        }
        score(request, jpeg);
    });
    watcher->setFuture(QtConcurrent::run([request] { return renderJpeg(request); }));
}

void PhotoIdentifier::score(const IdentifyRequest &request, const QByteArray &jpeg)
{
    QPointer<PhotoIdentifier> self(this);
    m_inat.scoreImage(jpeg, request.lonLat, request.date, [self, request](net::Outcome<net::VisionResult> out) {
        if (!self)
            return;
        Identification id;
        id.status = out.status;
        id.error = out.error;
        id.result = out.value;
        if (id.ok()) {
            QList<qint64> ids;
            for (const net::VisionSuggestion &s : std::as_const(id.result.suggestions))
                ids << s.taxon.inatId;
            if (id.result.commonAncestor)
                ids << id.result.commonAncestor->taxon.inatId;
            id.inTrees = inAnyTree(self->m_connectionName, ids);
            // Kept only when it worked: a refusal or an expired token is
            // worth asking again once that's put right.
            self->m_known.insert(cacheKey(request.key, request.crop), id);
        }
        emit self->finished(request.key, request.crop, id);
    });
}

} // namespace pl::inat
