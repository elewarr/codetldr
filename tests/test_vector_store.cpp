/// test_vector_store.cpp
/// Unit tests for VectorStore (Phase 16: FAISS persistent vector index).
/// Tests use dim=8 synthetic vectors — no real ONNX model needed.
/// Covers: VEC-01 (search/store), VEC-02 (atomic save), VEC-03 (concurrency), VEC-05 (dim mismatch)

#include "embedding/vector_store.h"

#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using codetldr::VectorStore;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr int kDim = 8;

/// Generate a unit basis vector e_i (1 at index i, 0 elsewhere).
static std::vector<float> basis(int i, int dim = kDim) {
    std::vector<float> v(dim, 0.0f);
    v[i % dim] = 1.0f;
    return v;
}

/// Unique temp directory for one test.
static fs::path make_temp_dir(const std::string& suffix) {
    auto p = fs::temp_directory_path() / ("codetldr_vs_test_" + suffix);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// ---------------------------------------------------------------------------
// Test 1: construct empty index
// ---------------------------------------------------------------------------
static void test_construct_empty() {
    auto dir = make_temp_dir("t1");
    auto path = dir / "index.faiss";
    assert(!fs::exists(path));

    auto vs = VectorStore::open(path, kDim);
    assert(vs.ntotal() == 0);
    assert(vs.dim() == kDim);
    std::cout << "PASS: test_construct_empty\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 2: add_batch returns correct ntotal
// ---------------------------------------------------------------------------
static void test_add_batch() {
    auto dir = make_temp_dir("t2");
    auto vs = VectorStore::open(dir / "index.faiss", kDim);

    std::vector<std::pair<int64_t, std::vector<float>>> items = {
        {10, basis(0)},
        {20, basis(1)},
        {30, basis(2)},
        {40, basis(3)},
        {50, basis(4)},
    };
    vs.add_batch(items);
    assert(vs.ntotal() == 5);
    std::cout << "PASS: test_add_batch\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 3: search returns expected top result
// ---------------------------------------------------------------------------
static void test_search_top_result() {
    auto dir = make_temp_dir("t3");
    auto vs = VectorStore::open(dir / "index.faiss", kDim);

    vs.add(100, basis(0));
    vs.add(200, basis(1));
    vs.add(300, basis(2));

    // Query with basis(1) — should match ID 200 first with distance ~0
    auto results = vs.search(basis(1), 1);
    assert(results.size() == 1);
    assert(results[0].first == 200);
    assert(results[0].second < 0.01f);
    std::cout << "PASS: test_search_top_result\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 4: remove by ID
// ---------------------------------------------------------------------------
static void test_remove() {
    auto dir = make_temp_dir("t4");
    auto vs = VectorStore::open(dir / "index.faiss", kDim);

    vs.add(1, basis(0));
    vs.add(2, basis(1));
    vs.add(3, basis(2));
    assert(vs.ntotal() == 3);

    vs.remove({2});
    assert(vs.ntotal() == 2);

    // Search for basis(1) — ID 2 was removed, so top result should be something else
    auto results = vs.search(basis(1), 3);
    for (auto& r : results) {
        assert(r.first != 2);
    }
    std::cout << "PASS: test_remove\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 5: save and reload
// ---------------------------------------------------------------------------
static void test_save_and_reload() {
    auto dir = make_temp_dir("t5");
    auto path = dir / "index.faiss";

    {
        auto vs = VectorStore::open(path, kDim);
        vs.add(100, basis(0));
        vs.add(200, basis(1));
        vs.add(300, basis(2));
        vs.save();
    }
    assert(fs::exists(path));

    // Reload
    auto vs2 = VectorStore::open(path, kDim);
    assert(vs2.ntotal() == 3);

    auto results = vs2.search(basis(1), 1);
    assert(results.size() == 1);
    assert(results[0].first == 200);
    assert(results[0].second < 0.01f);
    std::cout << "PASS: test_save_and_reload\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 6: atomic save — no .tmp file left behind
// ---------------------------------------------------------------------------
static void test_atomic_save() {
    auto dir = make_temp_dir("t6");
    auto path = dir / "index.faiss";
    auto tmp_path = dir / "index.faiss.tmp";

    auto vs = VectorStore::open(path, kDim);
    vs.add(1, basis(0));
    vs.save();

    assert(fs::exists(path));
    assert(!fs::exists(tmp_path));
    std::cout << "PASS: test_atomic_save\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 7: crash recovery — orphan .tmp does not affect load
// ---------------------------------------------------------------------------
static void test_crash_recovery() {
    auto dir = make_temp_dir("t7");
    auto path = dir / "index.faiss";
    auto tmp_path = dir / "index.faiss.tmp";

    // Write a valid index
    {
        auto vs = VectorStore::open(path, kDim);
        vs.add(42, basis(3));
        vs.save();
    }

    // Simulate crash: leave a corrupt .tmp file
    {
        std::ofstream f(tmp_path);
        f << "this is not a valid faiss index";
    }

    // open() should load from `path`, not from `.tmp`
    auto vs2 = VectorStore::open(path, kDim);
    assert(vs2.ntotal() == 1);
    auto results = vs2.search(basis(3), 1);
    assert(results.size() == 1);
    assert(results[0].first == 42);
    std::cout << "PASS: test_crash_recovery\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 8: dimension mismatch triggers auto-rebuild (VEC-05)
// ---------------------------------------------------------------------------
static void test_dimension_mismatch() {
    auto dir = make_temp_dir("t8");
    auto path = dir / "index.faiss";

    // Write with dim=4
    {
        auto vs = VectorStore::open(path, 4);
        vs.add(1, basis(0, 4));
        vs.save();
    }
    assert(fs::file_size(path) > 0);

    // Open with dim=kDim=8 — should auto-rebuild
    auto vs2 = VectorStore::open(path, kDim);
    assert(vs2.ntotal() == 0);
    assert(vs2.dim() == kDim);
    std::cout << "PASS: test_dimension_mismatch\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 9: wrong index type triggers auto-rebuild (VEC-05)
// ---------------------------------------------------------------------------
static void test_wrong_index_type() {
    auto dir = make_temp_dir("t9");
    auto path = dir / "index.faiss";

    // Write a plain IndexFlatL2 (not IndexIDMap2)
    {
        faiss::IndexFlatL2 flat(kDim);
        std::vector<float> v = basis(0);
        flat.add(1, v.data());
        faiss::write_index(&flat, path.c_str());
    }
    assert(fs::exists(path));

    // VectorStore::open should detect wrong type and rebuild
    auto vs = VectorStore::open(path, kDim);
    assert(vs.ntotal() == 0);
    assert(vs.dim() == kDim);
    std::cout << "PASS: test_wrong_index_type\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 10: concurrent add + search — 4 threads, no crash (VEC-03)
// ---------------------------------------------------------------------------
static void test_concurrent() {
    auto dir = make_temp_dir("t10");
    auto path = dir / "index.faiss";

    // Pre-populate so search has something to find
    {
        auto vs_init = VectorStore::open(path, kDim);
        for (int i = 0; i < 10; ++i) {
            vs_init.add(static_cast<int64_t>(i), basis(i % kDim));
        }
        vs_init.save();
    }

    auto vs = VectorStore::open(path, kDim);

    std::atomic<bool> error{false};
    constexpr int kOps = 100;

    // Thread 1 & 2: add vectors
    auto adder = [&](int offset) {
        try {
            for (int i = 0; i < kOps; ++i) {
                vs.add(static_cast<int64_t>(1000 + offset * kOps + i), basis(i % kDim));
            }
        } catch (...) {
            error = true;
        }
    };

    // Thread 3 & 4: search
    auto searcher = [&]() {
        try {
            for (int i = 0; i < kOps; ++i) {
                auto res = vs.search(basis(i % kDim), 3);
                (void)res;
            }
        } catch (...) {
            error = true;
        }
    };

    std::thread t1(adder, 0);
    std::thread t2(adder, 1);
    std::thread t3(searcher);
    std::thread t4(searcher);

    t1.join();
    t2.join();
    t3.join();
    t4.join();

    assert(!error.load());
    std::cout << "PASS: test_concurrent\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 11: search on empty index returns empty vector
// ---------------------------------------------------------------------------
static void test_search_empty() {
    auto dir = make_temp_dir("t11");
    auto vs = VectorStore::open(dir / "index.faiss", kDim);

    auto results = vs.search(basis(0), 5);
    assert(results.empty());
    std::cout << "PASS: test_search_empty\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 12: remove empty ids is a no-op
// ---------------------------------------------------------------------------
static void test_remove_empty() {
    auto dir = make_temp_dir("t12");
    auto vs = VectorStore::open(dir / "index.faiss", kDim);

    vs.add(1, basis(0));
    vs.remove({});
    assert(vs.ntotal() == 1);
    std::cout << "PASS: test_remove_empty\n";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    try {
        test_construct_empty();
        test_add_batch();
        test_search_top_result();
        test_remove();
        test_save_and_reload();
        test_atomic_save();
        test_crash_recovery();
        test_dimension_mismatch();
        test_wrong_index_type();
        test_concurrent();
        test_search_empty();
        test_remove_empty();

        std::cout << "\nAll VectorStore tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: exception: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "FAIL: unknown exception\n";
        return 1;
    }
}
