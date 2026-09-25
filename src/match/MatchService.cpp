#include "match/MatchService.h"

#include "db/Database.h"

#include <QThread>

namespace pl::match {

namespace {
const bool kMetaRegistered = [] {
    qRegisterMetaType<pl::match::MatchEngine::Stats>("pl::match::MatchEngine::Stats");
    return true;
}();
} // namespace

class MatchService::Worker : public QObject
{
    Q_OBJECT

public:
    Worker(CatalogueDescriptor descriptor, std::shared_ptr<std::atomic_bool> cancel,
           QSet<qint64> group, QList<qint64> captures)
        : m_descriptor(std::move(descriptor)), m_cancel(std::move(cancel)),
          m_group(std::move(group)), m_captures(std::move(captures))
    {
    }

    void run()
    {
        MatchEngine::Stats stats;

        Database db;
        if (!db.open(m_descriptor)) {
            stats.error = db.error().isEmpty() ? QStringLiteral("cannot open catalogue")
                                               : db.error();
            emit finished(stats);
            return;
        }

        MatchEngine engine(db.connectionName());
        const auto cancelled = [this] { return m_cancel->load(); };
        const auto reportProgress = [this](int done, int total) { emit progress(done, total); };
        if (!m_captures.isEmpty())
            stats = engine.matchCaptures(m_captures, cancelled, reportProgress);
        else if (!m_group.isEmpty())
            stats = engine.matchGroup(m_group, cancelled, reportProgress);
        else
            stats = engine.matchAll(cancelled, reportProgress);
        emit finished(stats);
    }

signals:
    void progress(int done, int total);
    void finished(pl::match::MatchEngine::Stats stats);

private:
    CatalogueDescriptor m_descriptor;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QSet<qint64> m_group;
    QList<qint64> m_captures;
};

MatchService::MatchService(CatalogueDescriptor descriptor, QObject *parent)
    : QObject(parent), m_descriptor(std::move(descriptor))
{
    Q_UNUSED(kMetaRegistered);
}

MatchService::~MatchService()
{
    cancel();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    delete m_worker;
}

void MatchService::start(const QSet<qint64> &groupTaxonIds)
{
    launch(groupTaxonIds, {});
}

void MatchService::startForCaptures(const QList<qint64> &captureIds)
{
    launch({}, captureIds);
}

void MatchService::launch(QSet<qint64> groupTaxonIds, QList<qint64> captureIds)
{
    if (m_running)
        return;
    m_running = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);

    m_worker = new Worker(m_descriptor, m_cancel, std::move(groupTaxonIds), std::move(captureIds));
    Worker *worker = m_worker;
    m_thread = QThread::create([worker] { worker->run(); });

    connect(worker, &Worker::progress, this, &MatchService::progress);
    connect(worker, &Worker::finished, this, &MatchService::onFinished);
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_worker->deleteLater();
        m_thread = nullptr;
        m_worker = nullptr;
    });

    emit started();
    m_thread->start();
}

void MatchService::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
}

void MatchService::onFinished(MatchEngine::Stats stats)
{
    m_running = false;
    m_cancel.reset();
    emit finished(stats);
}

} // namespace pl::match

#include "MatchService.moc"
