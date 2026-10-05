#pragma once
#include <vector>
#include <atomic>

// Lock-free Single-Producer Single-Consumer ring buffer.
// Policy: when full, DROP OLDEST data (overwrite) to preserve timing.
// This is the correct policy for live audio mirroring.
class RingBuffer {
    std::vector<float> buffer;
    std::atomic<size_t> write_pos{ 0 };
    std::atomic<size_t> read_pos{ 0 };
    size_t capacity;  // number of floats
    size_t mask;      // capacity - 1 (power-of-two)

    // Observable health counters (atomic so render/capture threads can read them freely)
    std::atomic<uint64_t> overflow_count{ 0 };
    std::atomic<uint64_t> underflow_count{ 0 };
    std::atomic<uint64_t> dropped_frames{ 0 };

public:
    explicit RingBuffer(size_t size) {
        // Round up to the next power of two
        size_t powerOfTwo = 1;
        while (powerOfTwo < size) powerOfTwo *= 2;

        capacity = powerOfTwo;
        mask     = powerOfTwo - 1;
        buffer.resize(powerOfTwo, 0.0f);
    }

    // Write samples. If buffer would overflow, oldest samples are dropped
    // to make room, preserving current timing over historical data.
    void Write(const float* data, size_t count) {
        size_t w = write_pos.load(std::memory_order_relaxed);
        size_t r = read_pos.load(std::memory_order_acquire);

        size_t available_space = capacity - (w - r); // unsigned arithmetic handles wrap

        if (count > available_space) {
            // Drop oldest: advance read pointer to make room
            size_t overflow = count - available_space;
            read_pos.fetch_add(overflow, std::memory_order_release);
            overflow_count.fetch_add(1, std::memory_order_relaxed);
            dropped_frames.fetch_add(overflow, std::memory_order_relaxed);
        }

        for (size_t i = 0; i < count; ++i) {
            buffer[(w + i) & mask] = data[i];
        }
        write_pos.store(w + count, std::memory_order_release);
    }

    // Read up to `count` samples. Fills remainder with zeros on underflow.
    // Returns actual samples read.
    size_t Read(float* data, size_t count) {
        size_t r = write_pos.load(std::memory_order_acquire); // re-read w for freshness
        size_t w = r;
        r = read_pos.load(std::memory_order_relaxed);
        w = write_pos.load(std::memory_order_acquire);

        size_t available = w - r;
        size_t to_read   = (count < available) ? count : available;

        for (size_t i = 0; i < to_read; ++i) {
            data[i] = buffer[(r + i) & mask];
        }
        read_pos.store(r + to_read, std::memory_order_release);

        // Zero-fill any gap
        for (size_t i = to_read; i < count; ++i) {
            data[i] = 0.0f;
        }

        if (to_read < count) {
            underflow_count.fetch_add(1, std::memory_order_relaxed);
        }

        return to_read;
    }

    // Peek without consuming. Useful for resampler lookahead.
    size_t Peek(float* data, size_t count) const {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        size_t available = w - r;
        size_t to_peek   = (count < available) ? count : available;

        for (size_t i = 0; i < to_peek; ++i) {
            data[i] = buffer[(r + i) & mask];
        }
        for (size_t i = to_peek; i < count; ++i) {
            data[i] = 0.0f;
        }
        return to_peek;
    }

    // Advance read pointer by `count` samples (use after Peek + processing).
    void Advance(size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        size_t available = w - r;
        // Never advance past write pointer
        size_t safe = (count < available) ? count : available;
        read_pos.store(r + safe, std::memory_order_release);
    }

    size_t GetAvailableRead() const {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        return w - r;
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
