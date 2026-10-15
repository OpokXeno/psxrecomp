#include "host_parallel.h"
#include "host_cpu.h"
#undef NDEBUG
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

struct Work {
    std::vector<std::atomic<unsigned>> hits;
    std::atomic<unsigned> concurrent{0}, peak{0};
    explicit Work(unsigned count) : hits(count) {
        for (auto& hit : hits) hit = 0;
    }
};

static void touch(uint32_t begin, uint32_t end, void *context) {
    auto& work = *static_cast<Work *>(context);
    const unsigned current = ++work.concurrent;
    unsigned peak = work.peak;
    while (peak < current && !work.peak.compare_exchange_weak(peak, current)) {}
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    for (uint32_t i = begin; i < end; ++i) ++work.hits[i];
    --work.concurrent;
}

static void nested(uint32_t begin, uint32_t end, void *context) {
    auto *work = static_cast<Work *>(context);
    for (uint32_t i = begin; i < end; ++i)
        psx_parallel_for(static_cast<uint32_t>(work[i].hits.size()), 3, touch, &work[i]);
}

int main() {
    Work work(257);
    psx_parallel_for(0, 1, touch, &work);
    psx_parallel_for(257, 7, touch, &work);
    for (auto& hit : work.hits) assert(hit == 1);
    const char *mode = std::getenv("PSX_HOST_PARALLEL");
    if (psx_host_available_cpu_count(1) > 1 && !(mode && mode[0] == '0')) assert(work.peak > 1);
    else assert(work.peak == 1);
    Work children[] = {Work(53), Work(71), Work(103), Work(11)};
    psx_parallel_for(4, 1, nested, children);
    for (auto& child : children)
        for (auto& hit : child.hits) assert(hit == 1);
    Work a(193), b(127);
    std::thread other([&] { psx_parallel_for(127, 5, touch, &b); });
    psx_parallel_for(193, 0, touch, &a);
    other.join();
    for (auto& hit : a.hits) assert(hit == 1);
    for (auto& hit : b.hits) assert(hit == 1);
    printf("shared parallel pool: exact-once, nested and concurrent batches OK (peak %u)\n",
        work.peak.load());
}
