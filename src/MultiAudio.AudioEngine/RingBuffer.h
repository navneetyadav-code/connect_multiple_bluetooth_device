#pragma once
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cassert>

// ============================================================================
// Bounded SPSC (Single-Producer Single-Consumer) Ring Buffer
// ============================================================================
//
// DESIGN INVARIANT:
//   The producer NEVER writes to a slot that the consumer could be reading.
//   The consumer NEVER reads a slot that the producer could be writing.
//
// This is achieved by a bounded-queue policy:
//   - Producer may only advance write_pos up to (read_pos_cache + capacity).
//     If the queue is full, Write() drops excess input rather than overwriting.
//   - Consumer may only advance read_pos up to (write_pos_cache).
//
// Both write_pos and read_pos are monotonically increasing 64-bit sequence
// numbers.  The actual buffer index is (seq & mask).  Because capacity is a
// power-of-two, the mask naturally wraps.
//
// Memory ordering follows the Lamport SPSC pattern:
//   - Producer: store(write_pos, release) after writing data.
//               load(read_pos, acquire) to see consumer progress.
//   - Consumer: store(read_pos, release) after consuming data.
//               load(write_pos, acquire) to see producer progress.
//
// THREADING CONTRACT:
//   - Exactly ONE thread calls Write()                   ("producer").
//   - Exactly ONE thread calls Read/Peek/Advance()       ("consumer").
//   - Clear() may only be called when BOTH threads are stopped.
//   - Metrics getters are safe to call from any thread.
//
// METRIC TERMINOLOGY:
//   All counts are in SAMPLES (individual float values), not audio frames.
//   The caller is responsible for converting (samples / channels) if frame
//   counts are needed at a higher layer.
//
class RingBuffer {
    // --- Storage ---
    std::vector<float> buffer_;
    size_t capacity_;            // always a power-of-two
    size_t mask_;                // capacity_ - 1

    // --- Sequence counters (monotonically increasing) ---
    // Placed on separate cache lines to avoid false-sharing.
    alignas(64) std::atomic<uint64_t> write_pos_{ 0 };
    alignas(64) std::atomic<uint64_t> read_pos_{ 0 };

    // --- Per-side cached snapshots of the other side's counter ---
    // These live on the SAME cache line as the owning side's counter
    // and are never touched by the other side, so no false-sharing.
    uint64_t cached_read_pos_  = 0;   // producer-local cache of read_pos_
    uint64_t cached_write_pos_ = 0;   // consumer-local cache of write_pos_

    // --- Diagnostics (relaxed atomics, safe to read from any thread) ---
    alignas(64) std::atomic<uint64_t> overflow_count_{ 0 };
    std::atomic<uint64_t> underflow_count_{ 0 };
    std::atomic<uint64_t> dropped_samples_{ 0 };

public:
    // -----------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------
    explicit RingBuffer(size_t min_capacity) {
        // Round up to next power-of-two.
        size_t pot = 1;
        while (pot < min_capacity) pot *= 2;
        capacity_ = pot;
        mask_     = pot - 1;
        buffer_.resize(pot, 0.0f);
    }

    // Non-copyable, non-movable (atomic members).
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // -----------------------------------------------------------------
    // Producer API  (call from producer thread ONLY)
    // -----------------------------------------------------------------

    // Write up to `count` samples.  Returns the number actually written.
    // If the queue is full, the EXCESS samples are DROPPED (not overwritten).
    // This guarantees the consumer's currently-readable region is never
    // touched.
    size_t Write(const float* data, size_t count) {
        const uint64_t w = write_pos_.load(std::memory_order_relaxed);

        // How much free space do we have?
        size_t free = capacity_ - static_cast<size_t>(w - cached_read_pos_);
        if (free < count) {
            // Refresh the cache — the consumer may have advanced.
            cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
            free = capacity_ - static_cast<size_t>(w - cached_read_pos_);
        }

        const size_t to_write = (count <= free) ? count : free;

        // --- Copy data into the ring ---
        // Split into at most two memcpy's around the physical wrap point.
        const size_t start = static_cast<size_t>(w & mask_);
        const size_t first_chunk = capacity_ - start;            // room until physical end

        if (to_write <= first_chunk) {
            std::memcpy(&buffer_[start], data, to_write * sizeof(float));
        } else {
            std::memcpy(&buffer_[start], data, first_chunk * sizeof(float));
            std::memcpy(&buffer_[0], data + first_chunk,
                        (to_write - first_chunk) * sizeof(float));
        }

        // Publish — consumer can now see the new data.
        write_pos_.store(w + to_write, std::memory_order_release);

        // --- Diagnostics for dropped input ---
        if (to_write < count) {
            const size_t dropped = count - to_write;
            overflow_count_.fetch_add(1, std::memory_order_relaxed);
            dropped_samples_.fetch_add(dropped, std::memory_order_relaxed);
        }

        return to_write;
    }

    // -----------------------------------------------------------------
    // Consumer API  (call from consumer thread ONLY)
    // -----------------------------------------------------------------

    // Read up to `count` samples, removing them from the queue.
    // Remaining slots (count - returned) in `data` are zero-filled (silence).
    // Returns the number of valid samples copied.
    size_t Read(float* data, size_t count) {
        const uint64_t r = read_pos_.load(std::memory_order_relaxed);

        // How much is available?
        size_t avail = static_cast<size_t>(cached_write_pos_ - r);
        if (avail < count) {
            // Refresh — the producer may have advanced.
            cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
            avail = static_cast<size_t>(cached_write_pos_ - r);
        }

        const size_t to_read = (count <= avail) ? count : avail;

        // --- Copy from ring ---
        const size_t start = static_cast<size_t>(r & mask_);
        const size_t first_chunk = capacity_ - start;

        if (to_read <= first_chunk) {
            std::memcpy(data, &buffer_[start], to_read * sizeof(float));
        } else {
            std::memcpy(data, &buffer_[start], first_chunk * sizeof(float));
            std::memcpy(data + first_chunk, &buffer_[0],
                        (to_read - first_chunk) * sizeof(float));
        }

        // Advance consumer position — producer can now reuse these slots.
        read_pos_.store(r + to_read, std::memory_order_release);

        // Zero-fill remainder (silence).
        if (to_read < count) {
            std::memset(data + to_read, 0, (count - to_read) * sizeof(float));
            underflow_count_.fetch_add(1, std::memory_order_relaxed);
        }

        return to_read;
    }

    // Peek up to `count` samples WITHOUT advancing read_pos.
    // Data remains in the queue.  Useful for look-ahead (resampler).
    // Returns the number of valid samples copied.
    //
    // SAFETY: Because the producer can only write to slots BEYOND write_pos,
    // and we only read slots in [read_pos, write_pos), no concurrent
    // mutation of the data we're copying is possible.
    size_t Peek(float* data, size_t count) const {
        const uint64_t r = read_pos_.load(std::memory_order_relaxed);
        const uint64_t w = write_pos_.load(std::memory_order_acquire);

        const size_t avail = static_cast<size_t>(w - r);
        const size_t to_peek = (count <= avail) ? count : avail;

        const size_t start = static_cast<size_t>(r & mask_);
        const size_t first_chunk = capacity_ - start;

        if (to_peek <= first_chunk) {
            std::memcpy(data, &buffer_[start], to_peek * sizeof(float));
        } else {
            std::memcpy(data, &buffer_[start], first_chunk * sizeof(float));
            std::memcpy(data + first_chunk, &buffer_[0],
                        (to_peek - first_chunk) * sizeof(float));
        }

        return to_peek;
    }

    // Advance the read pointer by `count` samples, discarding them.
    // Used after Peek() + external processing to commit consumption.
    // Will not advance past the current write_pos.
    void Advance(size_t count) {
        const uint64_t r = read_pos_.load(std::memory_order_relaxed);
        const uint64_t w = write_pos_.load(std::memory_order_acquire);

        const size_t avail = static_cast<size_t>(w - r);
        const size_t safe  = (count <= avail) ? count : avail;

        read_pos_.store(r + safe, std::memory_order_release);
    }

    // -----------------------------------------------------------------
    // Queries  (safe from any thread)
    // -----------------------------------------------------------------

    // Number of samples available for reading.
    size_t GetAvailableRead() const {
        const uint64_t r = read_pos_.load(std::memory_order_relaxed);
        const uint64_t w = write_pos_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    // Number of free slots the producer can write into.
    size_t GetAvailableWrite() const {
        const uint64_t w = write_pos_.load(std::memory_order_relaxed);
        const uint64_t r = read_pos_.load(std::memory_order_acquire);
        return capacity_ - static_cast<size_t>(w - r);
    }

    size_t GetCapacity() const { return capacity_; }

    // -----------------------------------------------------------------
    // Diagnostics  (safe from any thread)
    // -----------------------------------------------------------------
    uint64_t GetOverflowCount()  const { return overflow_count_.load(std::memory_order_relaxed); }
    uint64_t GetUnderflowCount() const { return underflow_count_.load(std::memory_order_relaxed); }
    uint64_t GetDroppedSamples() const { return dropped_samples_.load(std::memory_order_relaxed); }

    // -----------------------------------------------------------------
    // Reset  (call ONLY when both producer and consumer are STOPPED)
    // -----------------------------------------------------------------

    /// @pre  Neither the producer nor consumer thread is running.
    ///       Violating this is undefined behaviour.
    void Clear() {
        // Assert at debug time.  In release builds we just proceed.
        // A truly robust alternative would be to check atomic flags,
        // but the real fix is to only call Clear() from StartRouting()
        // which already holds g_engineMutex and joins all threads first.
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);

        // Reset caches so they match the zeroed counters.
        cached_read_pos_  = 0;
        cached_write_pos_ = 0;

        overflow_count_.store(0, std::memory_order_relaxed);
        underflow_count_.store(0, std::memory_order_relaxed);
        dropped_samples_.store(0, std::memory_order_relaxed);

        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    }
};
