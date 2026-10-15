#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace scopes
{
// Three frame buffers without locks (SPECIFICATION §2.3): the renderer fills its back buffer
// and publishes it, the writer takes the newest published one. Neither ever waits.
class TripleBuffer
{
public:
    void allocate(std::size_t bytes)
    {
        for (auto& buffer : buffers_)
        {
            buffer.assign(bytes, 0);
        }
        back_ = 0;
        middle_.store(1);
        front_ = 2;
    }
    // Renderer side.
    [[nodiscard]] std::uint8_t* back()
    {
        return buffers_[back_].data();
    }
    void publish()
    {
        back_ = middle_.exchange(back_ | kFresh) & kIndex;
    }
    // Writer side: the newest published buffer (the same one again when nothing new came).
    [[nodiscard]] std::uint8_t const* front()
    {
        if ((middle_.load() & kFresh) != 0)
        {
            front_ = middle_.exchange(front_) & kIndex;
        }
        return buffers_[front_].data();
    }
    [[nodiscard]] std::size_t size() const
    {
        return buffers_[0].size();
    }

private:
    static constexpr unsigned kFresh = 4;
    static constexpr unsigned kIndex = 3;
    std::vector<std::uint8_t> buffers_[3];
    unsigned back_ = 0;
    std::atomic<unsigned> middle_{1};
    unsigned front_ = 2;
};

// The same exchange for any object: the producer fills back() and publishes it; the consumer
// takes the newest published one with take(), which is nullptr when nothing new came.
template <typename T>
class TripleSlot
{
public:
    [[nodiscard]] T& back()
    {
        return slots_[back_];
    }
    void publish()
    {
        back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndex;
    }
    [[nodiscard]] T* take()
    {
        if ((middle_.load(std::memory_order_acquire) & kFresh) == 0)
        {
            return nullptr;
        }
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        return &slots_[front_];
    }

private:
    static constexpr unsigned kFresh = 4;
    static constexpr unsigned kIndex = 3;
    T slots_[3];
    unsigned back_ = 0;
    std::atomic<unsigned> middle_{1};
    unsigned front_ = 2;
};
} // namespace scopes
