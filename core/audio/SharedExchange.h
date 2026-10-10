#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace holotoner {

/**
 * Hands immutable data from a control thread to the audio thread without
 * locking or destroying data on the audio thread.
 *
 * The control thread owns publication and garbage collection. The audio
 * thread calls acquire() once per block and may use the returned pointer for
 * that block.
 */
template <typename T>
class SharedExchange {
public:
    void publish(std::shared_ptr<const T> data)
    {
        std::lock_guard lock(ownedMutex_);
        owned_.push_back(std::move(data));
        current_.store(owned_.back().get(), std::memory_order_release);
    }

    std::shared_ptr<const T> currentShared() const
    {
        std::lock_guard lock(ownedMutex_);
        const T* current = current_.load(std::memory_order_acquire);
        for (const auto& item : owned_)
            if (item.get() == current)
                return item;
        return nullptr;
    }

    /** Audio thread. Lock-free and allocation-free. */
    const T* acquire()
    {
        for (;;) {
            const T* data = current_.load(std::memory_order_acquire);
            inUse_.store(data, std::memory_order_release);
            if (current_.load(std::memory_order_acquire) == data)
                return data;
        }
    }

    /** Control thread. Retired objects are destroyed here, never in acquire(). */
    void collectGarbage()
    {
        std::lock_guard lock(ownedMutex_);
        const T* current = current_.load(std::memory_order_acquire);
        const T* inUse = inUse_.load(std::memory_order_acquire);
        std::array<const T*, kHoldSlots> held {};
        for (std::size_t i = 0; i < kHoldSlots; ++i)
            held[i] = held_[i].load(std::memory_order_acquire);
        std::erase_if(owned_, [&](const auto& item) {
            return item.get() != current && item.get() != inUse
                && std::find(held.begin(), held.end(), item.get()) == held.end();
        });
    }

    /**
     * Audio thread. Keeps a previously acquired object alive across blocks.
     * Slots are available for future crossfades and other overlapping reads.
     */
    void hold(std::size_t slot, const T* data)
    {
        if (slot < kHoldSlots)
            held_[slot].store(data, std::memory_order_release);
    }

    static constexpr std::size_t kHoldSlots = 2;

private:
    std::atomic<const T*> current_ {nullptr};
    std::atomic<const T*> inUse_ {nullptr};
    std::array<std::atomic<const T*>, kHoldSlots> held_ {};
    mutable std::mutex ownedMutex_;
    std::vector<std::shared_ptr<const T>> owned_;
};

} // namespace holotoner
