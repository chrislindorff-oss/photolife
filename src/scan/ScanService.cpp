#include "scan/ScanService.h"

#include "db/Database.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"

#include <QThread>

namespace pl::scan {

namespace {
bool registerMetaTypes()
{
    qRegisterMetaType<pl::scan::ScanProgress>("pl::scan::ScanProgress");
    qRegisterMetaType<pl::scan::ScanSummary>("pl::scan::ScanSummary");
    return true;
}
const bool kMetaTypesRegistered = registerMetaTypes();
} // namespace

// Lives on the background thread. Opens its own catalogue connection there.
class ScanService::Worker : public QObject
{
    Q_OBJECT

public:
    Worker(QString dbPath, QStringList roots, std::shared_ptr<std::atomic_bool> cancel)
        : m_dbPath(std::move(dbPath)), m_roots(std::move(roots)), m_cancel(std::move(cancel))
    {
    }

signals:
    void progress(pl::scan::ScanProgress progress);
    void finished(pl::scan::ScanSummary summary);

public slots:
    void run()
    {
        ScanSummary summary;

        Database db;
        if (!db.open(m_dbPath)) {
            summary.error = db.error();
            emit finished(summary);
            return;
        }

        auto cancelled = [this] { return m_cancel->load(); };

        FileScanner scanner;
        scanner.setProgressCallback([this](const ScanProgress &p) { emit progress(p); });
        scanner.setCancelPredicate(cancelled);

        const QList<DiscoveredCapture> captures = scanner.scan(m_roots);

        if (scanner.wasCancelled() || cancelled()) {
            summary.cancelled = true;
            emit finished(summary);
            return;
        }

        CatalogueWriter writer(db.connectionName());
        writer.setCancelPredicate(cancelled);
        writer.setProgressCallback([this](const ScanProgress &p) { emit progress(p); });
        summary = writer.sync(captures, m_roots);

        emit finished(summary);
    }

private:
    QString m_dbPath;
    QStringList m_roots;
    std::shared_ptr<std::atomic_bool> m_cancel;
};

ScanService::ScanService(QString databasePath, QObject *parent)
    : QObject(parent), m_databasePath(std::move(databasePath))
{
    Q_UNUSED(kMetaTypesRegistered);
}

ScanService::~ScanService()
{
    cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void ScanService::start(const QStringList &roots)
{
    if (m_running)
        return;
    m_running = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);

    m_thread = new QThread(this);
    auto *worker = new Worker(m_databasePath, roots, m_cancel);
    worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, worker, &Worker::run);
    connect(worker, &Worker::progress, this, &ScanService::progress);
    connect(worker, &Worker::finished, this, &ScanService::onFinished);
    connect(worker, &Worker::finished, worker, &QObject::deleteLater);
    connect(worker, &Worker::finished, m_thread, &QThread::quit);

    emit started();
    m_thread->start();
}

void ScanService::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
}

void ScanService::onFinished(ScanSummary summary)
{
    if (m_thread) {
        m_thread->wait();
        m_thread->deleteLater();
        m_thread = nullptr;
    }
    m_running = false;
    m_cancel.reset();
    emit finished(summary);
}

} // namespace pl::scan

#include "ScanService.moc"
