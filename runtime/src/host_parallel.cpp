#include "host_parallel.h"
#include "host_cpu.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace {
thread_local unsigned participation_depth;

struct Batch {
    Batch *next = nullptr;
    PsxParallelRange function;
    void *context;
    uint32_t count, grain, claimed = 0, completed = 0;
};

class ParallelPool {
    std::mutex mutex;
    std::condition_variable work, done;
    std::vector<std::thread> threads;
    Batch *batches = nullptr;
    unsigned callers = 0, running = 0, cpus = 1;
    bool stopping = false;

    Batch *runnable(Batch *preferred = nullptr) {
        if (preferred && preferred->claimed < preferred->count) return preferred;
        for (Batch *b = batches; b; b = b->next)
            if (b->claimed < b->count) return b;
        return nullptr;
    }

    void execute(std::unique_lock<std::mutex>& lock, Batch *batch) {
        const uint32_t begin = batch->claimed;
        const uint32_t end = begin + std::min(batch->grain, batch->count - begin);
        batch->claimed = end;
        lock.unlock();
        ++participation_depth;
        batch->function(begin, end, batch->context);
        --participation_depth;
        lock.lock();
        batch->completed += end - begin;
        if (batch->completed == batch->count) done.notify_all();
    }

    void worker() {
#if defined(__linux__)
        (void)pthread_setname_np(pthread_self(), "psx-jobs");
#endif
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            work.wait(lock, [&] {
                return stopping || (running + callers < cpus && runnable());
            });
            if (stopping) return;
            ++running;
            execute(lock, runnable());
            --running;
            work.notify_one();
        }
    }

public:
    ~ParallelPool() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            work.notify_all();
        }
        for (auto& thread : threads) thread.join();
    }

    void run(uint32_t count, uint32_t grain, PsxParallelRange function, void *context, unsigned available) {
        Batch batch{nullptr, function, context, count, grain};
        std::unique_lock<std::mutex> lock(mutex);
        const bool external = participation_depth == 0;
        if (external) ++callers;
        ++participation_depth;
        cpus = available;
        const uint32_t chunks = 1u + (count - 1u) / grain;
        const unsigned wanted = std::min(cpus - 1u, chunks - 1u);
        try {
            threads.reserve(wanted);
            while (threads.size() < wanted) threads.emplace_back([this] { worker(); });
        } catch (const std::exception&) {
            /* Existing workers and the caller still complete the whole batch. */
        }
        batch.next = batches;
        batches = &batch;
        work.notify_all();
        done.notify_all();
        while (batch.completed != batch.count) {
            if (Batch *next = runnable(&batch)) execute(lock, next);
            else done.wait(lock);
        }
        Batch **link = &batches;
        while (*link != &batch) link = &(*link)->next;
        *link = batch.next;
        --participation_depth;
        if (external) --callers;
        work.notify_all();
    }
};
} // namespace

extern "C" void psx_parallel_for(uint32_t count, uint32_t grain,
                                  PsxParallelRange function, void *context) {
    if (!count) return;
    if (!grain) grain = 1;
    static const bool disabled = [] {
        const char *value = std::getenv("PSX_HOST_PARALLEL");
        return value && value[0] == '0';
    }();
    if (disabled || count <= grain) {
        function(0, count, context);
        return;
    }
    static const unsigned fallback = std::thread::hardware_concurrency();
    const unsigned available = psx_host_worker_count(fallback, UINT32_MAX);
    if (available == 1u) {
        function(0, count, context);
        return;
    }
    static ParallelPool pool;
    pool.run(count, grain, function, context, available);
}
