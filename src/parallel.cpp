#include "parallel.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace par {
namespace {

thread_local bool in_loop = false;

class Pool {
public:
    explicit Pool(unsigned n) {
        for (unsigned i = 1; i < n; ++i) workers_.emplace_back([this] { work(); });
    }

    ~Pool() {
        { std::lock_guard lk(m_); stop_ = true; }
        wake_.notify_all();
        for (auto& t : workers_) t.join();
    }

    // False if another thread is running a loop; the caller then runs serially.
    bool try_run(std::size_t n, void* ctx, void (*fn)(void*, std::size_t)) {
        std::unique_lock owner(owner_, std::try_to_lock);
        if (!owner) return false;
        {
            std::lock_guard lk(m_);
            n_ = n; ctx_ = ctx; fn_ = fn;
            next_.store(0, std::memory_order_relaxed);
            busy_ = workers_.size();
            ++epoch_;
        }
        wake_.notify_all();
        drain();
        std::unique_lock lk(m_);
        done_.wait(lk, [this] { return busy_ == 0; });
        return true;
    }

private:
    void drain() {
        in_loop = true;
        for (std::size_t i; (i = next_.fetch_add(1, std::memory_order_relaxed)) < n_;)
            fn_(ctx_, i);
        in_loop = false;
    }

    void work() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lk(m_);
                wake_.wait(lk, [&] { return stop_ || epoch_ != seen; });
                if (stop_) return;
                seen = epoch_;
            }
            drain();
            {
                std::lock_guard lk(m_);
                --busy_;
            }
            done_.notify_one();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex               owner_;   // one loop at a time
    std::mutex               m_;
    std::condition_variable  wake_, done_;
    std::atomic<std::size_t> next_{0};
    std::size_t              n_{0}, busy_{0};
    void*                    ctx_{nullptr};
    void                   (*fn_)(void*, std::size_t){nullptr};
    std::uint64_t            epoch_{0};
    bool                     stop_{false};
};

unsigned              g_threads = 0;
std::unique_ptr<Pool> g_pool;
std::mutex            g_pool_init;

} // namespace

void set_threads(unsigned n) {
    g_pool.reset();
    g_threads = n;
}

unsigned threads() noexcept {
    if (g_threads == 0) g_threads = std::max(1u, std::thread::hardware_concurrency());
    return g_threads;
}

void run(std::size_t n, void* ctx, void (*fn)(void*, std::size_t)) {
    if (n == 0) return;
    if (threads() == 1 || n == 1 || in_loop) {
        for (std::size_t i = 0; i < n; ++i) fn(ctx, i);
        return;
    }
    {
        std::lock_guard lk(g_pool_init);
        if (!g_pool) g_pool = std::make_unique<Pool>(threads());
    }
    if (!g_pool->try_run(n, ctx, fn))
        for (std::size_t i = 0; i < n; ++i) fn(ctx, i);
}

} // namespace par
