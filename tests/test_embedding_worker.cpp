// test_embedding_worker.cpp
// Tests for EmbeddingStats ring buffer correctness (OBS-01, OBS-04).

#include "embedding/embedding_worker.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

using namespace codetldr;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_pass;                                                           \
            std::cout << "PASS: " << msg << "\n";                              \
        } else {                                                                \
            ++g_fail;                                                           \
            std::cerr << "FAIL: " << msg << "  (line " << __LINE__ << ")\n";  \
        }                                                                       \
    } while (0)

#define CHECK_APPROX(val, expected, tol, msg)                                   \
    CHECK(std::fabs((val) - (expected)) < (tol), msg)

// ---------------------------------------------------------------
// Test 1: fresh stats are empty
// ---------------------------------------------------------------
static void test_fresh_stats_empty() {
    EmbeddingStats stats;
    auto snap = stats.snapshot();
    CHECK(snap.has_data == false, "fresh stats: has_data=false");
    CHECK(snap.sample_count == 0, "fresh stats: sample_count=0");
    CHECK(snap.chunks_processed == 0, "fresh stats: chunks_processed=0");
    CHECK(snap.queue_depth == 0, "fresh stats: queue_depth=0");
    CHECK(snap.p50_ms == 0.0, "fresh stats: p50_ms=0");
    CHECK(snap.p95_ms == 0.0, "fresh stats: p95_ms=0");
    CHECK(snap.p99_ms == 0.0, "fresh stats: p99_ms=0");
}

// ---------------------------------------------------------------
// Test 2: ring buffer accumulates 5 samples
// ---------------------------------------------------------------
static void test_ring_buffer_accumulates_samples() {
    EmbeddingStats stats;

    CHECK(stats.snapshot().has_data == false, "ring buffer: initially empty");
    CHECK(stats.snapshot().sample_count == 0, "ring buffer: sample_count=0 initially");

    // Write 5 samples: 10ms, 20ms, 30ms, 40ms, 50ms
    uint64_t samples_ns[] = {10000000, 20000000, 30000000, 40000000, 50000000};
    for (uint64_t ns : samples_ns) {
        uint64_t slot = stats.write_index.fetch_add(1) % EmbeddingStats::kWindowSize;
        stats.latency_ns[slot] = ns;
        stats.chunks_processed.fetch_add(1);
        stats.total_duration_ns.fetch_add(ns);
    }

    auto snap = stats.snapshot();
    CHECK(snap.has_data == true, "ring buffer: has_data=true after writes");
    CHECK(snap.sample_count == 5, "ring buffer: sample_count=5");
    CHECK(snap.chunks_processed == 5, "ring buffer: chunks_processed=5");
    // p50 of [10,20,30,40,50] ms: sorted index at 50% of 5 = index 2 = 30ms
    CHECK_APPROX(snap.p50_ms, 30.0, 0.001, "ring buffer: p50=30ms");
    // p99 of 5 samples: min(4, 5*99/100) = min(4, 4) = 4 => 50ms
    CHECK_APPROX(snap.p99_ms, 50.0, 0.001, "ring buffer: p99=50ms");
    // avg = (10+20+30+40+50)/5 = 30ms
    CHECK_APPROX(snap.avg_ms, 30.0, 0.001, "ring buffer: avg=30ms");
    // throughput = chunks * 1e9 / total_ns = 5 * 1e9 / 150_000_000 ~= 33.3 chunks/sec
    CHECK(snap.throughput_chunks_per_sec > 0.0, "ring buffer: throughput > 0");
}

// ---------------------------------------------------------------
// Test 3: ring buffer wraps at kWindowSize
// ---------------------------------------------------------------
static void test_ring_buffer_wraps() {
    EmbeddingStats stats;

    // Write 101 samples — should wrap, sample_count stays at 100
    for (size_t i = 0; i < EmbeddingStats::kWindowSize + 1; ++i) {
        uint64_t slot = stats.write_index.fetch_add(1) % EmbeddingStats::kWindowSize;
        stats.latency_ns[slot] = static_cast<uint64_t>(i + 1) * 1000000; // i+1 ms
    }

    auto snap = stats.snapshot();
    // write_index = 101 > kWindowSize=100, so filled = min(101, 100) = 100
    CHECK(snap.sample_count == 100, "ring buffer wrap: sample_count=100 after 101 writes");
    CHECK(snap.has_data == true, "ring buffer wrap: has_data=true");
}

// ---------------------------------------------------------------
// Test 4: OBS-04 EXECUTION_PROVIDER_FALLBACK threshold
// ---------------------------------------------------------------
static void test_execution_provider_fallback_threshold() {
    EmbeddingStats stats;

    // Write 10 samples all at 25ms — this should trigger the OBS-04 check (p50 > 20ms)
    for (int i = 0; i < 10; ++i) {
        uint64_t slot = stats.write_index.fetch_add(1) % EmbeddingStats::kWindowSize;
        stats.latency_ns[slot] = 25000000; // 25ms
        stats.chunks_processed.fetch_add(1);
        stats.total_duration_ns.fetch_add(25000000);
    }

    auto snap = stats.snapshot();
    CHECK(snap.sample_count == 10, "obs04 threshold: sample_count=10");
    CHECK(snap.p50_ms > 20.0, "obs04 threshold: p50_ms > 20ms (CoreML fallback threshold)");
}

// ---------------------------------------------------------------
// Test 5: queue_depth tracking
// ---------------------------------------------------------------
static void test_queue_depth_tracking() {
    EmbeddingStats stats;

    stats.queue_depth.store(42, std::memory_order_relaxed);
    auto snap = stats.snapshot();
    CHECK(snap.queue_depth == 42, "queue depth: stored value retrieved correctly");

    stats.queue_depth.store(0, std::memory_order_relaxed);
    snap = stats.snapshot();
    CHECK(snap.queue_depth == 0, "queue depth: reset to 0 correctly");
}

// ---------------------------------------------------------------
// Test 6: EmbeddingWorker stats accessor
// ---------------------------------------------------------------
static void test_embedding_worker_stats_accessor() {
    EmbeddingWorker worker;

    // Initially empty
    auto snap = worker.stats().snapshot();
    CHECK(snap.has_data == false, "EmbeddingWorker: stats initially empty");

    // Write one sample via mutable_stats
    auto& ms = worker.mutable_stats();
    uint64_t slot = ms.write_index.fetch_add(1) % EmbeddingStats::kWindowSize;
    ms.latency_ns[slot] = 5000000; // 5ms
    ms.chunks_processed.fetch_add(1);
    ms.total_duration_ns.fetch_add(5000000);

    snap = worker.stats().snapshot();
    CHECK(snap.has_data == true, "EmbeddingWorker: has_data=true after write");
    CHECK(snap.sample_count == 1, "EmbeddingWorker: sample_count=1");
    CHECK_APPROX(snap.p50_ms, 5.0, 0.001, "EmbeddingWorker: p50=5ms");
}

int main() {
    test_fresh_stats_empty();
    test_ring_buffer_accumulates_samples();
    test_ring_buffer_wraps();
    test_execution_provider_fallback_threshold();
    test_queue_depth_tracking();
    test_embedding_worker_stats_accessor();

    int total = g_pass + g_fail;
    printf("\ntest_embedding_worker: %d/%d passed\n", g_pass, total);
    return g_fail == 0 ? 0 : 1;
}
