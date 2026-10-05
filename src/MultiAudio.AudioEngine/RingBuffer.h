#pragma once
#include <vector>
#include <atomic>
#include <algorithm>

// Lock-free Single-Producer Single-Consumer ring buffer with monotonic counters.
class RingBuffer {
    std::vector<float> buffer;
    std::atomic<size_t> write_pos{ 0 };
    std::atomic<size_t> read_pos{ 0 };
    size_t capacity;  
    size_t mask;      

    std::atomic<uint64_t> overflow_count{ 0 };
    std::atomic<uint64_t> underflow_count{ 0 };
    std::atomic<uint64_t> dropped_frames{ 0 };

    void AdvanceReadPosSafe(size_t target_r) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        while (r < target_r && !read_pos.compare_exchange_weak(r, target_r, std::memory_order_release, std::memory_order_relaxed)) {
            // Loop until we successfully advance or another thread advances it past target_r
        }
    }

public:
    explicit RingBuffer(size_t size) {
        size_t powerOfTwo = 1;
        while (powerOfTwo < size) powerOfTwo *= 2;
        capacity = powerOfTwo;
        mask     = powerOfTwo - 1;
        buffer.resize(powerOfTwo, 0.0f);
    }

    void Write(const float* data, size_t count) {
        size_t w = write_pos.load(std::memory_order_relaxed);
        size_t r = read_pos.load(std::memory_order_acquire);

        // Issue #1 Fix: Producer cleanly advances read_pos via CAS if overflow occurs
        if (w + count - r > capacity) {
            size_t overflow = (w + count) - (r + capacity);
            AdvanceReadPosSafe(r + overflow);
            overflow_count.fetch_add(1, std::memory_order_relaxed);
            dropped_frames.fetch_add(overflow, std::memory_order_relaxed);
        }

        for (size_t i = 0; i < count; ++i) {
            buffer[(w + i) & mask] = data[i];
        }
        write_pos.store(w + count, std::memory_order_release);
    }

    size_t Read(float* data, size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);

        size_t available = (w > r) ? (w - r) : 0;
        size_t to_read   = (count < available) ? count : available;

        for (size_t i = 0; i < to_read; ++i) {
            data[i] = buffer[(r + i) & mask];
        }
        
        AdvanceReadPosSafe(r + to_read);

        for (size_t i = to_read; i < count; ++i) {
            data[i] = 0.0f;
        }

        if (to_read < count) {
            underflow_count.fetch_add(1, std::memory_order_relaxed);
        }

        return to_read;
    }

    size_t Peek(float* data, size_t count) const {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        
        size_t available = (w > r) ? (w - r) : 0;
        size_t to_peek   = (count < available) ? count : available;

        for (size_t i = 0; i < to_peek; ++i) {
            data[i] = buffer[(r + i) & mask];
        }
        
        // Don't zero fill peek, let caller handle shortage.
        return to_peek;
    }

    void Advance(size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        size_t available = (w > r) ? (w - r) : 0;
        size_t safe = (count < available) ? count : available;
        AdvanceReadPosSafe(r + safe);
    }

    size_t GetAvailableRead() const {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        return (w > r) ? (w - r) : 0;
    }

    size_t GetCapacity() const { return capacity; }
    uint64_t GetOverflowCount()   const { return overflow_count.load(std::memory_order_relaxed); }
    uint64_t GetUnderflowCount()  const { return underflow_count.load(std::memory_order_relaxed); }
    uint64_t GetDroppedFrames()   const { return dropped_frames.load(std::memory_order_relaxed); }

    void Clear() {
        write_pos.store(0, std::memory_order_relaxed);
        read_pos.store(0, std::memory_order_relaxed);
        overflow_count.store(0, std::memory_order_relaxed);
        underflow_count.store(0, std::memory_order_relaxed);
        dropped_frames.store(0, std::memory_order_relaxed);
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    }
};
