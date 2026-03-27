#include "embedding/embedding_worker.h"
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace codetldr {

EmbeddingStats::Snapshot EmbeddingStats::snapshot() const {
    // Read write_index first (acquire), then read array entries
    uint64_t idx = write_index.load(std::memory_order_acquire);
    size_t filled = static_cast<size_t>(std::min(idx, static_cast<uint64_t>(kWindowSize)));

    Snapshot s;
    s.chunks_processed = chunks_processed.load(std::memory_order_relaxed);
    s.queue_depth      = queue_depth.load(std::memory_order_relaxed);
    s.sample_count     = filled;
    s.has_data         = (filled > 0);

    if (filled == 0) return s;

    // Copy filled entries into a local sorted array for percentile computation
    std::array<uint64_t, kWindowSize> samples{};
    for (size_t i = 0; i < filled; ++i) {
        samples[i] = latency_ns[i];
    }
    std::sort(samples.begin(), samples.begin() + filled);

    s.p50_ms = samples[filled * 50 / 100] / 1e6;
    s.p95_ms = samples[filled * 95 / 100] / 1e6;
    s.p99_ms = samples[std::min(filled - 1, filled * 99 / 100)] / 1e6;

    uint64_t total_ns = total_duration_ns.load(std::memory_order_relaxed);
    uint64_t total_chunks = s.chunks_processed;
    if (total_ns > 0 && total_chunks > 0) {
        s.avg_ms = static_cast<double>(total_ns) / total_chunks / 1e6;
        s.throughput_chunks_per_sec = total_chunks * 1e9 / static_cast<double>(total_ns);
    }
    return s;
}

} // namespace codetldr
