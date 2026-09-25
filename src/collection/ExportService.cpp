#include "collection/ExportService.h"

#include "db/Database.h"

#include <QThread>

namespace pl::collection {

namespace {
const bool kMetaRegistered = [] {
    qRegisterMetaType<pl::collection::ExportSummary>("pl::collection::ExportSummary");
    return true;
}();
} // namespace

class ExportService::Worker : public QObject
{
    Q_OBJECT

public:
    Worker(CatalogueDescriptor descriptor, int projectId, QString destRoot,
          ExportOptions options, std::shared_ptr<std::atomic_bool> cancel)
        : m_descriptor(std::move(descriptor)), m_projectId(projectId),
          m_destRoot(std::move(destRoot)), m_options(options), m_cancel(std::move(cancel))
    {
    }

    void run()
    {
        ExportSummary summary;

        Database db;
        if (!db.open(m_descriptor)) {
            summary.error = db.error().isEmpty() ? QStringLiteral("cannot open catalogue")
                                                 : db.error();
            emit finished(summary);
            return;
        }

        ExportEngine engine(db.connectionName());
        summary = engine.run(
            m_projectId, m_destRoot, m_options, [this] { return m_cancel->load(); },
            [this](int done, int total) { emit progress(done, total); });
        emit finished(summary);
    }

signals:
    void progress(int done, int total);
    void finished(pl::collection::ExportSummary summary);

private:
    CatalogueDescriptor m_descriptor;
    int m_projectId;
    QString m_destRoot;
    ExportOptions m_options;
    std::shared_ptr<std::atomic_bool> m_cancel;
};

ExportService::ExportService(CatalogueDescriptor descriptor, QObject *parent)
    : QObject(parent), m_descriptor(std::move(descriptor))
{
    Q_UNUSED(kMetaRegistered);
}

ExportService::~ExportService()
{
    cancel();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    delete m_worker;
}

void ExportService::start(int projectId, const QString &destRoot, ExportOptions options)
{
    if (m_running)
        return;
    m_running = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);

    m_worker = new Worker(m_descriptor, projectId, destRoot, options, m_cancel);
    Worker *worker = m_worker;
    m_thread = QThread::create([worker] { worker->run(); });

    connect(worker, &Worker::progress, this, &ExportService::progress);
    connect(worker, &Worker::finished, this, &ExportService::onFinished);
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_worker->deleteLater();
        m_thread = nullptr;
        m_worker = nullptr;
    });

    emit started();
    m_thread->start();
}

void ExportService::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
}

void ExportService::onFinished(ExportSummary summary)
{
    m_running = false;
    m_cancel.reset();
    emit finished(summary);
}

} // namespace pl::collection

#include "ExportService.moc"
