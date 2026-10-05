#pragma once
#include <vector>
#include <atomic>
#include <algorithm>

// True Lock-free Single-Producer Single-Consumer ring buffer.
// The Producer NEVER touches read_pos.
// The Consumer NEVER touches write_pos (only reads it).
class RingBuffer {
    std::vector<float> buffer;
    std::atomic<size_t> write_pos{ 0 };
    std::atomic<size_t> read_pos{ 0 };
    size_t capacity;  
    size_t mask;      

    std::atomic<uint64_t> overflow_count{ 0 };
    std::atomic<uint64_t> underflow_count{ 0 };
    std::atomic<uint64_t> dropped_frames{ 0 };

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

        // Producer blindly writes. It is the Consumer's responsibility 
        // to detect if it has been lapped/overwritten.
        for (size_t i = 0; i < count; ++i) {
            buffer[(w + i) & mask] = data[i];
        }
        
        write_pos.store(w + count, std::memory_order_release);
    }

    size_t Read(float* data, size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);

        // Detect Overrun BEFORE reading
        if (w - r > capacity) {
            size_t dropped = (w - r) - capacity;
            r = w - capacity;
            overflow_count.fetch_add(1, std::memory_order_relaxed);
            dropped_frames.fetch_add(dropped, std::memory_order_relaxed);
        }

        size_t available = w - r;
        size_t to_read   = (count < available) ? count : available;

        for (size_t i = 0; i < to_read; ++i) {
            data[i] = buffer[(r + i) & mask];
        }
        
        // Detect Torn Read AFTER reading (Producer lapped us while copying)
        size_t w2 = write_pos.load(std::memory_order_acquire);
        if (w2 - r > capacity) {
            // Data is corrupt. Output silence instead of screeching audio.
            for (size_t i = 0; i < count; ++i) data[i] = 0.0f;
            
            size_t dropped = (w2 - r) - capacity;
            read_pos.store(w2 - capacity, std::memory_order_release);
            
            overflow_count.fetch_add(1, std::memory_order_relaxed);
            dropped_frames.fetch_add(dropped, std::memory_order_relaxed);
            
            return count; 
        }

        read_pos.store(r + to_read, std::memory_order_release);

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
        
        if (w - r > capacity) {
            r = w - capacity;
        }

        size_t available = w - r;
        size_t to_peek   = (count < available) ? count : available;

        for (size_t i = 0; i < to_peek; ++i) {
            data[i] = buffer[(r + i) & mask];
        }
        
        size_t w2 = write_pos.load(std::memory_order_acquire);
        if (w2 - r > capacity) {
            // Torn peek. Return 0 so caller doesn't use corrupt data.
            return 0;
        }

        return to_peek;
    }

    void Advance(size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        
        if (w - r > capacity) {
            size_t dropped = (w - r) - capacity;
            r = w - capacity;
            overflow_count.fetch_add(1, std::memory_order_relaxed);
            dropped_frames.fetch_add(dropped, std::memory_order_relaxed);
        }
        
        size_t available = w - r;
        size_t safe = (count < available) ? count : available;
        read_pos.store(r + safe, std::memory_order_release);
    }

    size_t GetAvailableRead() const {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        
        if (w - r > capacity) {
            return capacity;
        }
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
