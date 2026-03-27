#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>

namespace codetldr {

/// Lock-free rolling latency statistics for the embedding pipeline.
/// Written on the hot embedding path (atomic ops only), read on the status path.
/// Single writer (worker thread) — no concurrent write contention.
struct EmbeddingStats {
    static constexpr size_t kWindowSize = 100;

    // Ring buffer: kWindowSize latency samples in nanoseconds
    std::array<uint64_t, kWindowSize> latency_ns{};
    std::atomic<uint64_t> write_index{0};       // monotonically increasing write cursor
    std::atomic<uint64_t> chunks_processed{0};  // total chunks embedded since start
    std::atomic<uint64_t> total_duration_ns{0}; // total inference time (for throughput)
    std::atomic<uint64_t> queue_depth{0};       // snapshot of pending_ids_ size

    struct Snapshot {
        double p50_ms = 0.0;
        double p95_ms = 0.0;
        double p99_ms = 0.0;
        double avg_ms = 0.0;
        double throughput_chunks_per_sec = 0.0;
        uint64_t chunks_processed = 0;
        uint64_t queue_depth = 0;
        size_t   sample_count = 0;  // how many samples in the window (0..100)
        bool     has_data = false;  // true once at least 1 sample recorded
    };

    /// Compute a percentile snapshot from the ring buffer.
    /// Caller-side read; no locks needed (single-writer atomic ring buffer).
    Snapshot snapshot() const;
};

/// EmbeddingWorker: background worker that processes embedding jobs.
/// Phase 15+ will add the full implementation; this header provides the
/// EmbeddingStats ring buffer and get_stats() accessor used by the
/// observability layer (Phase 22).
class EmbeddingWorker {
public:
    EmbeddingWorker() = default;
    ~EmbeddingWorker() = default;

    // Non-copyable, non-movable
    EmbeddingWorker(const EmbeddingWorker&) = delete;
    EmbeddingWorker& operator=(const EmbeddingWorker&) = delete;

    /// Access the embedding stats ring buffer (read-only from caller).
    const EmbeddingStats& stats() const noexcept { return stats_; }

    /// Mutable access for the worker thread to record samples.
    EmbeddingStats& mutable_stats() noexcept { return stats_; }

private:
    EmbeddingStats stats_;
};

} // namespace codetldr
