#pragma once

#include "db/CatalogueDescriptor.h"
#include "scan/ScanTypes.h"

#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

class QThread;

namespace pl::scan {

// Runs a catalogue scan (FileScanner walk + CatalogueWriter persist) on a
// background thread and reports progress on the thread it was created on.
// One scan at a time; start() is ignored while a scan is running.
class ScanService : public QObject
{
    Q_OBJECT

public:
    explicit ScanService(CatalogueDescriptor descriptor, QObject *parent = nullptr);
    ~ScanService() override;

    bool isRunning() const { return m_running; }

public slots:
    void start(const QStringList &roots);
    void cancel();

signals:
    void started();
    void progress(pl::scan::ScanProgress progress);
    void finished(pl::scan::ScanSummary summary);

private:
    class Worker;

    void onFinished(pl::scan::ScanSummary summary);

    CatalogueDescriptor m_descriptor;
    QThread *m_thread = nullptr;
    Worker *m_worker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_running = false;
};

} // namespace pl::scan
