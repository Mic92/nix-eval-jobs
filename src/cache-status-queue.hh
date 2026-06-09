#pragma once
///@file

#include <nix/store/store-api.hh>
#include <nix/util/ref.hh>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "response.hh"

/* A thread pool resolving the cache status (substituter narinfo
   lookups via Store::queryMissing) of evaluated jobs.

   Doing the lookup inline in the eval worker stalls CPU-bound
   evaluation behind HTTP round-trips, so the collector enqueues
   finished jobs here instead. */
class CacheStatusQueue {
  public:
    /* Called with the job after its cache status has been filled in.
       Invoked from pool threads; the sink must do its own locking. */
    using Sink = std::function<void(Response)>;

    CacheStatusQueue(nix::ref<nix::Store> store, size_t nrThreads, Sink sink);
    ~CacheStatusQueue();

    CacheStatusQueue(const CacheStatusQueue &) = delete;
    CacheStatusQueue(CacheStatusQueue &&) = delete;
    auto operator=(const CacheStatusQueue &) -> CacheStatusQueue & = delete;
    auto operator=(CacheStatusQueue &&) -> CacheStatusQueue & = delete;

    void push(Response response);

    /* Drain remaining work, join all threads and rethrow the first
       error raised by a pool thread. */
    void finish();

  private:
    void run();
    void closeAndJoin();

    nix::ref<nix::Store> store;
    Sink sink;

    std::mutex mutex;
    std::condition_variable wakeup;
    std::deque<Response> queue;
    bool closed = false;
    std::exception_ptr exc;

    // Plain std::thread is fine here: unlike the eval threads these
    // never run deeply recursive evaluator code, so the platform
    // default stack size suffices.
    std::vector<std::thread> threads;
};
