#pragma once

#include "libsync/LibraryStore.h"

#include <QCoreApplication>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace pl::libsync {

// Runs `job` for items 0..count-1 on up to store.parallelTransfers() worker
// threads, each with its own clone of `store`, and hands each result to
// `done` on the calling thread, in the order they finish -- so `done` can
// keep using things that belong to this thread (the sync state's database).
// Workers stop taking items once `stop` says so. With one worker, or a store
// that can't be cloned, everything runs here with `store` itself.
//
// While it waits the calling thread keeps processing events, so a
// QObject-based server in this thread (as in the tests) still answers.
template <typename Result>
void forEachTransfer(LibraryStore &store, int count, const std::function<bool()> &stop,
                     const std::function<Result(LibraryStore &, int)> &job,
                     const std::function<void(int, Result &&)> &done)
{
    std::vector<std::unique_ptr<LibraryStore>> clones;
    for (int w = 0; w < std::min(store.parallelTransfers(), count); ++w) {
        auto c = store.clone();
        if (!c)
            break;
        clones.push_back(std::move(c));
    }
    if (clones.size() <= 1) {
        for (int i = 0; i < count && !(stop && stop()); ++i)
            done(i, job(store, i));
        return;
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::pair<int, Result>> ready;
    std::atomic<int> next{0};
    int finished = 0;
    std::vector<std::unique_ptr<QThread>> threads;
    const int workers = int(clones.size());
    for (auto &clone : clones) {
        LibraryStore *own = clone.release();
        threads.emplace_back(QThread::create([&, own] {
            std::unique_ptr<LibraryStore> worker(own);   // its network objects live on this thread
            while (!(stop && stop())) {
                const int i = next++;
                if (i >= count)
                    break;
                Result r = job(*worker, i);
                std::lock_guard lock(mutex);
                ready.emplace_back(i, std::move(r));
                wake.notify_one();
            }
            worker.reset();
            std::lock_guard lock(mutex);
            ++finished;
            wake.notify_one();
        }));
        threads.back()->start();
    }

    std::unique_lock lock(mutex);
    for (;;) {
        wake.wait_for(lock, std::chrono::milliseconds(20),
                      [&] { return !ready.empty() || finished == workers; });
        while (!ready.empty()) {
            auto item = std::move(ready.front());
            ready.pop_front();
            lock.unlock();
            done(item.first, std::move(item.second));
            lock.lock();
        }
        if (finished == workers)
            break;
        lock.unlock();
        if (QCoreApplication::instance())
            QCoreApplication::processEvents();
        lock.lock();
    }
    lock.unlock();
    for (auto &t : threads)
        t->wait();
}

} // namespace pl::libsync
