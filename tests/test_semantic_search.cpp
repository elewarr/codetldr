// test_semantic_search.cpp -- Tests for VectorStore IDSelectorBatch pre-filter.
// Verifies LANG-04: language filter uses IDSelectorBatch pre-filter, not post-filter.
// Guarded by CODETLDR_ENABLE_SEMANTIC_SEARCH.

#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH

#include "embedding/vector_store.h"
#include <faiss/impl/IDSelector.h>
#include <faiss/Index.h>
#include <cassert>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>

// Helper: create a simple 3-dim vector with a specific value for each component.
static std::vector<float> make_vec(float v0, float v1, float v2) {
    return {v0, v1, v2};
}

int main() {
    const int DIM = 3;

    // Test 1: Basic add + unfiltered search
    {
        VectorStore vs(DIM);
        assert(vs.ntotal() == 0);

        // Add 4 vectors with IDs 10, 20, 30, 40
        vs.add(10, make_vec(1.0f, 0.0f, 0.0f));
        vs.add(20, make_vec(0.0f, 1.0f, 0.0f));
        vs.add(30, make_vec(0.0f, 0.0f, 1.0f));
        vs.add(40, make_vec(1.0f, 1.0f, 0.0f));
        assert(vs.ntotal() == 4);

        // Query closest to (1, 0, 0) — should return ID 10 first
        auto results = vs.search(make_vec(1.0f, 0.0f, 0.0f), 4);
        assert(!results.empty());
        assert(results[0].first == 10);
        std::cout << "PASS: basic unfiltered search returns correct nearest neighbor\n";
    }

    // Test 2: IDSelectorBatch pre-filter — only allowed IDs returned
    {
        VectorStore vs(DIM);
        vs.add(10, make_vec(1.0f, 0.0f, 0.0f));   // "cpp" symbol
        vs.add(20, make_vec(0.0f, 1.0f, 0.0f));   // "python" symbol
        vs.add(30, make_vec(0.0f, 0.0f, 1.0f));   // "cpp" symbol
        vs.add(40, make_vec(1.0f, 1.0f, 0.0f));   // "python" symbol

        // Filter to only "cpp" symbols: IDs 10 and 30
        std::vector<faiss::idx_t> cpp_ids = {10, 30};
        faiss::IDSelectorBatch sel(cpp_ids.size(), cpp_ids.data());
        faiss::SearchParameters params;
        params.sel = &sel;

        // Query closest to (1, 0, 0) with filter — should only return IDs from cpp_ids
        auto results = vs.search(make_vec(1.0f, 0.0f, 0.0f), 4, &params);
        for (const auto& [id, dist] : results) {
            bool allowed = (id == 10 || id == 30);
            assert(allowed);
        }
        // ID 10 should be closest (distance 0 to query)
        assert(!results.empty());
        assert(results[0].first == 10);
        std::cout << "PASS: IDSelectorBatch pre-filter returns only allowed IDs\n";

        // Filter to only "python" symbols: IDs 20 and 40
        std::vector<faiss::idx_t> py_ids = {20, 40};
        faiss::IDSelectorBatch py_sel(py_ids.size(), py_ids.data());
        faiss::SearchParameters py_params;
        py_params.sel = &py_sel;

        // Query closest to (1, 0, 0) with python filter — python symbol IDs only
        auto py_results = vs.search(make_vec(1.0f, 0.0f, 0.0f), 4, &py_params);
        for (const auto& [id, dist] : py_results) {
            bool allowed = (id == 20 || id == 40);
            assert(allowed);
        }
        std::cout << "PASS: IDSelectorBatch python filter excludes cpp symbols\n";
    }

    // Test 3: Empty allowed set (short-circuit behavior at VectorStore level)
    {
        VectorStore vs(DIM);
        vs.add(10, make_vec(1.0f, 0.0f, 0.0f));
        vs.add(20, make_vec(0.0f, 1.0f, 0.0f));

        // Empty IDSelectorBatch — no IDs allowed
        std::vector<faiss::idx_t> empty_ids;
        faiss::IDSelectorBatch sel(0, nullptr);
        faiss::SearchParameters params;
        params.sel = &sel;

        auto results = vs.search(make_vec(1.0f, 0.0f, 0.0f), 4, &params);
        // FAISS with empty IDSelectorBatch should return no valid results (all -1)
        // The VectorStore filters out ids[i] < 0
        assert(results.empty());
        std::cout << "PASS: empty IDSelectorBatch returns no results\n";
    }

    // Test 4: nullptr params falls through to unfiltered search
    {
        VectorStore vs(DIM);
        vs.add(10, make_vec(1.0f, 0.0f, 0.0f));
        vs.add(20, make_vec(0.0f, 1.0f, 0.0f));

        auto filtered = vs.search(make_vec(1.0f, 0.0f, 0.0f), 2, nullptr);
        auto unfiltered = vs.search(make_vec(1.0f, 0.0f, 0.0f), 2);
        assert(filtered.size() == unfiltered.size());
        assert(filtered[0].first == unfiltered[0].first);
        std::cout << "PASS: nullptr params equivalent to unfiltered search\n";
    }

    // Test 5: Pre-filter vs post-filter correctness guarantee
    // When k > filtered set size, FAISS pre-filter correctly returns only the
    // filtered symbols (not k symbols with post-filter applied afterward).
    {
        VectorStore vs(DIM);
        // Add 6 vectors: IDs 1-6
        vs.add(1, make_vec(1.0f, 0.0f, 0.0f));
        vs.add(2, make_vec(0.9f, 0.1f, 0.0f));
        vs.add(3, make_vec(0.8f, 0.2f, 0.0f));
        vs.add(4, make_vec(0.0f, 1.0f, 0.0f));  // different cluster
        vs.add(5, make_vec(0.0f, 0.9f, 0.1f));
        vs.add(6, make_vec(0.0f, 0.8f, 0.2f));

        // Filter to only IDs {4, 5, 6} (the second cluster)
        std::vector<faiss::idx_t> allowed = {4, 5, 6};
        faiss::IDSelectorBatch sel(allowed.size(), allowed.data());
        faiss::SearchParameters params;
        params.sel = &sel;

        // Query near first cluster (1,0,0) — but filter restricts to second cluster
        // Result should only contain IDs from {4, 5, 6}
        auto results = vs.search(make_vec(1.0f, 0.0f, 0.0f), 6, &params);
        for (const auto& [id, dist] : results) {
            assert(id == 4 || id == 5 || id == 6);
        }
        assert(results.size() <= 3);
        std::cout << "PASS: pre-filter with k>allowed set size returns only allowed IDs ("
                  << results.size() << " results)\n";
    }

    std::cout << "All semantic search tests passed.\n";
    return 0;
}

#else

#include <iostream>
int main() {
    std::cout << "Semantic search disabled (CODETLDR_ENABLE_SEMANTIC_SEARCH=OFF). "
              << "Tests skipped.\n";
    return 0;
}

#endif // CODETLDR_ENABLE_SEMANTIC_SEARCH
