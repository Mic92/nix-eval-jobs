#include <cstddef>
#include <exception>
#include <mutex>
#include <nix/store/store-api.hh>
#include <nix/util/ref.hh>
#include <nix/util/signals.hh>
#include <utility>
#include <variant>

#include "cache-status-queue.hh"
#include "drv.hh"
#include "response.hh"

CacheStatusQueue::CacheStatusQueue(nix::ref<nix::Store> store, size_t nrThreads,
                                   Sink sink)
    : store(std::move(store)), sink(std::move(sink)) {
    threads.reserve(nrThreads);
    for (size_t i = 0; i < nrThreads; i++) {
        threads.emplace_back([this]() -> void { run(); });
    }
}

void CacheStatusQueue::closeAndJoin() {
    {
        const std::scoped_lock lock(mutex);
        closed = true;
    }
    wakeup.notify_all();
    for (auto &thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

CacheStatusQueue::~CacheStatusQueue() {
    {
        // The destructor runs on error paths (exception unwinding)
        // where draining the backlog would only delay shutdown.
        const std::scoped_lock lock(mutex);
        queue.clear();
    }
    closeAndJoin();
}

void CacheStatusQueue::push(Response response) {
    {
        const std::scoped_lock lock(mutex);
        queue.push_back(std::move(response));
    }
    wakeup.notify_one();
}

void CacheStatusQueue::finish() {
    closeAndJoin();
    const std::scoped_lock lock(mutex);
    if (exc) {
        std::rethrow_exception(exc);
    }
}

void CacheStatusQueue::run() {
    std::unique_lock<std::mutex> lock(mutex);
    while (true) {
        wakeup.wait(lock, [this]() -> bool {
            return !queue.empty() || closed || exc != nullptr;
        });
        // Empty queue here implies closed. Also stop on a sibling's
        // failure: finish() rethrows it anyway, so remaining lookups
        // would be wasted work.
        if (exc != nullptr || queue.empty()) {
            return;
        }
        auto response = std::move(queue.front());
        queue.pop_front();

        lock.unlock();
        try {
            nix::checkInterrupt();
            if (auto *job = std::get_if<Response::Job>(&response.payload)) {
                auto &drv = job->drv;
                auto derivation = store->readDerivation(drv.drvPath);
                drv.cacheStatus = queryCacheStatus(
                    *store, drv.outputs, drv.neededBuilds,
                    drv.neededSubstitutes, drv.unknownPaths, derivation);
            }
            sink(std::move(response));
        } catch (...) {
            lock.lock();
            if (exc == nullptr) {
                exc = std::current_exception();
            }
            wakeup.notify_all();
            return;
        }
        lock.lock();
    }
}
