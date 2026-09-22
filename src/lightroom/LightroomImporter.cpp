#include "lightroom/LightroomImporter.h"

#include "db/Database.h"
#include "lightroom/LightroomCatalogReader.h"

#include <QThread>

namespace pl::lightroom {

namespace {
const bool kMetaRegistered = [] {
    qRegisterMetaType<pl::lightroom::LightroomImportEngine::Stats>(
        "pl::lightroom::LightroomImportEngine::Stats");
    return true;
}();
} // namespace

class LightroomImporter::Worker : public QObject
{
    Q_OBJECT

public:
    Worker(CatalogueDescriptor descriptor, QString lrcatPath,
           std::shared_ptr<std::atomic_bool> cancel)
        : m_descriptor(std::move(descriptor)), m_lrcatPath(std::move(lrcatPath)),
          m_cancel(std::move(cancel))
    {
    }

    void run()
    {
        LightroomImportEngine::Stats stats;

        LightroomCatalogReader reader;
        const auto photos = reader.read(m_lrcatPath);
        if (!reader.ok()) {
            stats.error = reader.error();
            emit finished(stats);
            return;
        }

        Database db;
        if (!db.open(m_descriptor)) {
            stats.error = db.error().isEmpty() ? QStringLiteral("cannot open catalogue")
                                               : db.error();
            emit finished(stats);
            return;
        }

        LightroomImportEngine engine(db.connectionName());
        stats = engine.import(photos, [this] { return m_cancel->load(); },
                              [this](int done, int total) { emit progress(done, total); });
        emit finished(stats);
    }

signals:
    void progress(int done, int total);
    void finished(pl::lightroom::LightroomImportEngine::Stats stats);

private:
    CatalogueDescriptor m_descriptor;
    QString m_lrcatPath;
    std::shared_ptr<std::atomic_bool> m_cancel;
};

LightroomImporter::LightroomImporter(CatalogueDescriptor descriptor, QObject *parent)
    : QObject(parent), m_descriptor(std::move(descriptor))
{
    Q_UNUSED(kMetaRegistered);
}

LightroomImporter::~LightroomImporter()
{
    cancel();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    delete m_worker;
}

void LightroomImporter::start(const QString &lrcatPath)
{
    if (m_running)
        return;
    m_running = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);

    m_worker = new Worker(m_descriptor, lrcatPath, m_cancel);
    Worker *worker = m_worker;
    m_thread = QThread::create([worker] { worker->run(); });

    connect(worker, &Worker::progress, this, &LightroomImporter::progress);
    connect(worker, &Worker::finished, this, &LightroomImporter::onFinished);
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_worker->deleteLater();
        m_thread = nullptr;
        m_worker = nullptr;
    });

    emit started();
    m_thread->start();
}

void LightroomImporter::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
}

void LightroomImporter::onFinished(LightroomImportEngine::Stats stats)
{
    m_running = false;
    m_cancel.reset();
    emit finished(stats);
}

} // namespace pl::lightroom

#include "LightroomImporter.moc"
