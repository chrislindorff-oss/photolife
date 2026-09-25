#pragma once

#include "collection/ExportEngine.h"
#include "db/CatalogueDescriptor.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>

class QThread;

namespace pl::collection {

// Runs ExportEngine::run on a background thread, reporting progress on the
// thread it was created on. One export at a time.
class ExportService : public QObject
{
    Q_OBJECT

public:
    explicit ExportService(CatalogueDescriptor descriptor, QObject *parent = nullptr);
    ~ExportService() override;

    bool isRunning() const { return m_running; }

public slots:
    void start(int projectId, const QString &destRoot, pl::collection::ExportOptions options);
    void cancel();

signals:
    void started();
    void progress(int done, int total);
    void finished(pl::collection::ExportSummary summary);

private:
    class Worker;
    void onFinished(pl::collection::ExportSummary summary);

    CatalogueDescriptor m_descriptor;
    QThread *m_thread = nullptr;
    Worker *m_worker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_running = false;
};

} // namespace pl::collection
