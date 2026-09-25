#pragma once

#include "db/CatalogueDescriptor.h"
#include "match/MatchEngine.h"

#include <QObject>

#include <atomic>
#include <memory>

class QThread;

namespace pl::match {

// Runs MatchEngine::matchAll (or, given a group, matchGroup) on a background
// thread, reporting progress on the thread it was created on. One run at a time.
class MatchService : public QObject
{
    Q_OBJECT

public:
    explicit MatchService(CatalogueDescriptor descriptor, QObject *parent = nullptr);
    ~MatchService() override;

    bool isRunning() const { return m_running; }

public slots:
    // Empty `groupTaxonIds` = the whole library (matchAll); otherwise
    // matchGroup() against those local taxon ids.
    void start(const QSet<qint64> &groupTaxonIds = {});
    // MatchEngine::matchCaptures() on just these captures.
    void startForCaptures(const QList<qint64> &captureIds);
    void cancel();

signals:
    void started();
    void progress(int done, int total);
    void finished(pl::match::MatchEngine::Stats stats);

private:
    class Worker;
    void launch(QSet<qint64> groupTaxonIds, QList<qint64> captureIds);
    void onFinished(pl::match::MatchEngine::Stats stats);

    CatalogueDescriptor m_descriptor;
    QThread *m_thread = nullptr;
    Worker *m_worker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_running = false;
};

} // namespace pl::match
