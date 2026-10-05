#pragma once
#include <vector>
#include <atomic>
#include <algorithm>

class RingBuffer {
    std::vector<float> buffer;
    std::atomic<size_t> write_pos{ 0 };
    std::atomic<size_t> read_pos{ 0 };
    size_t mask;
    
    // Size must be a power of 2 for this mask logic to work
public:
    RingBuffer(size_t size) {
        // Find next power of 2
        size_t powerOfTwo = 1;
        while (powerOfTwo < size) powerOfTwo *= 2;
        
        buffer.resize(powerOfTwo, 0.0f);
        mask = powerOfTwo - 1;
    }
    
    void Write(const float* data, size_t count) {
        size_t w = write_pos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            buffer[w & mask] = data[i];
            w++;
        }
        write_pos.store(w, std::memory_order_release);
    }
    
    size_t Read(float* data, size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        
        size_t available = w - r; // Because w and r just grow, unsigned math handles wrapping
        size_t to_read = (count < available) ? count : available;
        
        for (size_t i = 0; i < to_read; ++i) {
            data[i] = buffer[r & mask];
            r++;
        }
        read_pos.store(r, std::memory_order_release);
        
        // Fill the rest with zeros if underflow
        for (size_t i = to_read; i < count; ++i) {
            data[i] = 0.0f;
        }
        
        return to_read;
    }
    
    size_t Peek(float* data, size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        size_t available = w - r;
        size_t to_read = (count < available) ? count : available;
        
        for (size_t i = 0; i < to_read; ++i) {
            data[i] = buffer[(r + i) & mask];
        }
        
        for (size_t i = to_read; i < count; ++i) {
            data[i] = 0.0f;
        }
        return to_read;
    }
    
    void Advance(size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        r += count;
        read_pos.store(r, std::memory_order_release);
    }
    
    size_t GetAvailableRead() const {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        return w - r;
    }
    
    void Clear() {
        write_pos.store(0, std::memory_order_relaxed);
        read_pos.store(0, std::memory_order_relaxed);
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    }
};
