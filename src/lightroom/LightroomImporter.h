#pragma once

#include "db/CatalogueDescriptor.h"
#include "lightroom/LightroomImportEngine.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>

class QThread;

namespace pl::lightroom {

// Runs LightroomCatalogReader then LightroomImportEngine on a background
// thread, reporting progress on the thread it was created on. One run at a
// time. Structural mirror of match::MatchService.
class LightroomImporter : public QObject
{
    Q_OBJECT

public:
    explicit LightroomImporter(CatalogueDescriptor descriptor, QObject *parent = nullptr);
    ~LightroomImporter() override;

    bool isRunning() const { return m_running; }

public slots:
    void start(const QString &lrcatPath);
    void cancel();

signals:
    void started();
    void progress(int done, int total);
    void finished(pl::lightroom::LightroomImportEngine::Stats stats);

private:
    class Worker;
    void onFinished(pl::lightroom::LightroomImportEngine::Stats stats);

    CatalogueDescriptor m_descriptor;
    QThread *m_thread = nullptr;
    Worker *m_worker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_running = false;
};

} // namespace pl::lightroom
