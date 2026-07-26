#pragma once
#include "common.h"
#include <atomic>

// Single-producer / single-consumer lock-free ring of interleaved float frames.
// Producer = decode thread. Consumer = WASAPI render thread.
class RingBuffer {
public:
    void init(size_t frames, int channels) {
        ch = channels;
        capFrames = frames + 1;              // one slot always empty => full/empty are distinguishable
        buf.assign(capFrames * (size_t)ch, 0.0f);
        rd.store(0, std::memory_order_relaxed);
        wr.store(0, std::memory_order_relaxed);
    }

    size_t readAvail() const {
        size_t r = rd.load(std::memory_order_acquire);
        size_t w = wr.load(std::memory_order_acquire);
        return w >= r ? w - r : capFrames - r + w;
    }

    size_t writeAvail() const { return capFrames - 1 - readAvail(); }

    size_t capacity() const { return capFrames - 1; }

    // producer only
    size_t write(const float* src, size_t frames) {
        size_t avail = writeAvail();
        if (frames > avail) frames = avail;
        size_t w = wr.load(std::memory_order_relaxed);
        size_t first = capFrames - w;
        if (first > frames) first = frames;
        memcpy(&buf[w * ch], src, first * ch * sizeof(float));
        if (frames > first)
            memcpy(&buf[0], src + first * ch, (frames - first) * ch * sizeof(float));
        wr.store((w + frames) % capFrames, std::memory_order_release);
        return frames;
    }

    // consumer only
    size_t read(float* dst, size_t frames) {
        size_t avail = readAvail();
        if (frames > avail) frames = avail;
        size_t r = rd.load(std::memory_order_relaxed);
        size_t first = capFrames - r;
        if (first > frames) first = frames;
        memcpy(dst, &buf[r * ch], first * ch * sizeof(float));
        if (frames > first)
            memcpy(dst + first * ch, &buf[0], (frames - first) * ch * sizeof(float));
        rd.store((r + frames) % capFrames, std::memory_order_release);
        return frames;
    }

    // consumer only: throw away everything currently queued
    void dropAll() {
        size_t w = wr.load(std::memory_order_acquire);
        rd.store(w, std::memory_order_release);
    }

    // safe only when the consumer is provably idle (stream stopped)
    void hardReset() {
        rd.store(0, std::memory_order_release);
        wr.store(0, std::memory_order_release);
    }

private:
    std::vector<float> buf;
    size_t capFrames = 0;
    int ch = 2;
    std::atomic<size_t> rd{ 0 }, wr{ 0 };
};
