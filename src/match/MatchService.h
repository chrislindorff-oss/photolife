#pragma once

#include "match/MatchEngine.h"

#include <QObject>

#include <atomic>
#include <memory>

class QThread;

namespace pl::match {

// Runs MatchEngine::matchAll on a background thread, reporting progress on the
// thread it was created on. One run at a time.
class MatchService : public QObject
{
    Q_OBJECT

public:
    explicit MatchService(QString databasePath, QObject *parent = nullptr);
    ~MatchService() override;

    bool isRunning() const { return m_running; }

public slots:
    void start();
    void cancel();

signals:
    void started();
    void progress(int done, int total);
    void finished(pl::match::MatchEngine::Stats stats);

private:
    class Worker;
    void onFinished(pl::match::MatchEngine::Stats stats);

    QString m_databasePath;
    QThread *m_thread = nullptr;
    Worker *m_worker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_running = false;
};

} // namespace pl::match
