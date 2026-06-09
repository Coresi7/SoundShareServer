#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// Single-producer single-consumer ring buffer for int16 PCM samples.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacitySamples)
        : m_buffer(capacitySamples), m_capacity(capacitySamples) {}

    size_t Capacity() const { return m_capacity; }

    size_t Available() const {
        size_t w = m_writePos.load(std::memory_order_acquire);
        size_t r = m_readPos.load(std::memory_order_acquire);
        if (w >= r) return w - r;
        return m_capacity - r + w;
    }

    size_t FreeSpace() const { return m_capacity - Available() - 1; }

    // Producer: push samples; drops oldest data on overflow.
    void Push(const int16_t* data, size_t count) {
        if (count == 0) return;

        size_t w = m_writePos.load(std::memory_order_relaxed);
        size_t r = m_readPos.load(std::memory_order_acquire);
        size_t avail = (w >= r) ? (w - r) : (m_capacity - r + w);
        size_t free = m_capacity - avail - 1;

        if (count > free) {
            size_t drop = count - free;
            m_readPos.store((r + drop) % m_capacity, std::memory_order_release);
        }

        w = m_writePos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; i++) {
            m_buffer[w] = data[i];
            w = (w + 1) % m_capacity;
        }
        m_writePos.store(w, std::memory_order_release);
    }

    // Consumer: read up to maxCount samples; returns actual count read.
    size_t Pop(int16_t* out, size_t maxCount) {
        size_t r = m_readPos.load(std::memory_order_relaxed);
        size_t w = m_writePos.load(std::memory_order_acquire);
        size_t avail = (w >= r) ? (w - r) : (m_capacity - r + w);
        size_t toRead = (maxCount < avail) ? maxCount : avail;

        for (size_t i = 0; i < toRead; i++) {
            out[i] = m_buffer[r];
            r = (r + 1) % m_capacity;
        }
        m_readPos.store(r, std::memory_order_release);
        return toRead;
    }

    void Clear() {
        m_readPos.store(0, std::memory_order_relaxed);
        m_writePos.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<int16_t> m_buffer;
    size_t m_capacity;
    std::atomic<size_t> m_writePos{0};
    std::atomic<size_t> m_readPos{0};
};
