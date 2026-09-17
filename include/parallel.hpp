#pragma once
#include <cstddef>
#include <type_traits>

// A persistent thread pool for data-parallel loops. Threads start on first use
// and stay parked between loops; the calling thread takes work too. Calls made
// from inside a running loop execute serially.
namespace par {

void set_threads(unsigned n);   // 0 = hardware concurrency, 1 = serial
[[nodiscard]] unsigned threads() noexcept;

void run(std::size_t n, void* ctx, void (*fn)(void*, std::size_t));

// fn(i) for every i in [0, n), in unspecified order.
template <class F>
void for_each_index(std::size_t n, F&& fn) {
    using Fn = std::remove_reference_t<F>;
    run(n, const_cast<void*>(static_cast<const void*>(&fn)),
        [](void* c, std::size_t i) { (*static_cast<Fn*>(c))(i); });
}

} // namespace par
