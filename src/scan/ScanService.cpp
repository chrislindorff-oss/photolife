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

// Does the work on the background thread. Opens its own catalogue connection
// there; emits progress/finished, which are delivered to the service's thread.
class ScanService::Worker : public QObject
{
    Q_OBJECT

public:
    Worker(CatalogueDescriptor descriptor, QStringList roots,
           std::shared_ptr<std::atomic_bool> cancel)
        : m_descriptor(std::move(descriptor)), m_roots(std::move(roots)),
          m_cancel(std::move(cancel))
    {
    }

    void run()
    {
        ScanSummary summary;
        auto cancelled = [this] { return m_cancel->load(); };

        Database db;
        if (!db.open(m_descriptor)) {
            summary.error = db.error().isEmpty() ? QStringLiteral("cannot open catalogue")
                                                 : db.error();
            emit finished(summary);
            return;
        }

        FileScanner scanner;
        scanner.setCancelPredicate(cancelled);
        scanner.setProgressCallback([this](const ScanProgress &p) { emit progress(p); });

        const QList<DiscoveredCapture> captures = scanner.scan(m_roots);
        if (scanner.wasCancelled()) {
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

signals:
    void progress(pl::scan::ScanProgress progress);
    void finished(pl::scan::ScanSummary summary);

private:
    CatalogueDescriptor m_descriptor;
    QStringList m_roots;
    std::shared_ptr<std::atomic_bool> m_cancel;
};

ScanService::ScanService(CatalogueDescriptor descriptor, QObject *parent)
    : QObject(parent), m_descriptor(std::move(descriptor))
{
    Q_UNUSED(kMetaTypesRegistered);
}

ScanService::~ScanService()
{
    cancel();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    delete m_worker;
}

void ScanService::start(const QStringList &roots)
{
    if (m_running)
        return;
    m_running = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);

    m_worker = new Worker(m_descriptor, roots, m_cancel);
    Worker *worker = m_worker;
    m_thread = QThread::create([worker] { worker->run(); });

    connect(worker, &Worker::progress, this, &ScanService::progress);
    connect(worker, &Worker::finished, this, &ScanService::onFinished);
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_worker->deleteLater();
        m_thread = nullptr;
        m_worker = nullptr;
    });

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
    m_running = false;
    m_cancel.reset();
    emit finished(summary);
}

} // namespace pl::scan

#include "ScanService.moc"
