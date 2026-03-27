// hybrid_search_engine.cpp -- RRF fusion of FTS5 and FAISS search results.
//
// When CODETLDR_ENABLE_SEMANTIC_SEARCH is OFF (or model/store are null),
// degrades transparently to FTS5-only search.

#include "query/hybrid_search_engine.h"

#include <algorithm>
#include <future>
#include <unordered_map>

#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
// Forward declared in header; include real headers here when semantic build is active.
// ModelManager and VectorStore headers will be provided by Phase 15+ implementation.
// Until then, the semantic path cannot be compiled — guarded by the flag.
#include "embedding/model_manager.h"
#include "embedding/vector_store.h"
#include <faiss/Index.h>
#include <faiss/impl/IDSelector.h>
#endif

namespace codetldr {

HybridSearchEngine::HybridSearchEngine(const std::filesystem::path& db_path,
                                        ModelManager* model,
                                        VectorStore* store,
                                        HybridSearchConfig config)
    : db_(db_path.string(), SQLite::OPEN_READONLY)
    , fts5_(db_)
    , model_(model)
    , store_(store)
    , config_(config)
{}

bool HybridSearchEngine::vector_available() const {
#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
    return model_ != nullptr
        && store_ != nullptr
        && store_->ntotal() > 0
        && model_->status() == ModelStatus::loaded;
#else
    return false;
#endif
}

void HybridSearchEngine::set_config(HybridSearchConfig config) {
    config_ = config;
}

HybridSearchResult HybridSearchEngine::run_hybrid(
    const std::string& query, const std::string& kind,
    const std::string& language, int limit)
{
    // Determine effective limit: use return_limit as default when limit <= 0
    int effective_limit = limit > 0 ? limit : config_.return_limit;

    // Compute BM25/FAISS candidate counts
    int bm25_candidates = config_.bm25_limit > 0
        ? config_.bm25_limit
        : std::max(effective_limit * config_.candidate_multiplier, 60);
    int vec_candidates = config_.vec_limit > 0
        ? config_.vec_limit
        : std::max(effective_limit * config_.candidate_multiplier, 60);

    if (!vector_available()) {
        // HYB-03: degrade to FTS5-only transparently
        HybridSearchResult hybrid_result;
        auto results = fts5_.search_symbols(query, kind, language, effective_limit);
        for (auto& r : results) r.provenance = "fts5";
        hybrid_result.results = std::move(results);
        hybrid_result.search_mode = "fts5_only";
        return hybrid_result;
    }

    (void)bm25_candidates; // suppress unused warning when semantic search disabled
    (void)vec_candidates;

    // HYB-02: parallel retrieval via std::async
    auto fts5_future = std::async(std::launch::async, [&]() {
        return fts5_.search_symbols(query, kind, language, bm25_candidates);
    });

#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
    auto faiss_future = std::async(std::launch::async, [&]() -> std::vector<std::pair<int64_t, float>> {
        std::vector<float> qvec = model_->embed(query, /*is_query=*/true);
        if (qvec.empty()) return {};
        if (!language.empty()) {
            // Build IDSelectorBatch for language filter
            std::vector<faiss::idx_t> allowed_ids;
            try {
                SQLite::Statement q(db_,
                    "SELECT ef.symbol_id FROM embedded_files ef "
                    "JOIN files f ON f.id = ef.file_id "
                    "WHERE f.language = ?");
                q.bind(1, language);
                while (q.executeStep()) {
                    allowed_ids.push_back(q.getColumn(0).getInt64());
                }
            } catch (...) { return {}; }
            if (allowed_ids.empty()) return {};
            faiss::IDSelectorBatch sel(
                static_cast<faiss::idx_t>(allowed_ids.size()),
                allowed_ids.data());
            faiss::SearchParameters params;
            params.sel = &sel;
            return store_->search(qvec, vec_candidates, &params);
        }
        return store_->search(qvec, vec_candidates);
    });
    auto faiss_results = faiss_future.get();
#else
    std::vector<std::pair<int64_t, float>> faiss_results;
#endif

    auto fts5_results = fts5_future.get();

    // HYB-01: RRF fusion with k = config_.rrf_k
    // Build symbol_id -> SearchResult map from FTS5 results (already have metadata)
    std::unordered_map<int64_t, SearchResult> symbol_map;
    for (const auto& r : fts5_results) {
        symbol_map[r.symbol_id] = r;
    }

    // Fetch metadata for FAISS-only results that are not already in symbol_map
    for (const auto& [id, dist] : faiss_results) {
        if (id >= 0 && symbol_map.find(id) == symbol_map.end()) {
            try {
                SQLite::Statement q(db_,
                    "SELECT s.id, s.name, s.kind, COALESCE(s.signature,''), "
                    "COALESCE(s.documentation,''), f.path, s.line_start "
                    "FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.id = ?");
                q.bind(1, static_cast<long long>(id));
                if (q.executeStep()) {
                    SearchResult r;
                    r.symbol_id     = q.getColumn(0).getInt64();
                    r.name          = q.getColumn(1).getText();
                    r.kind          = q.getColumn(2).getText();
                    r.signature     = q.getColumn(3).getText();
                    r.documentation = q.getColumn(4).getText();
                    r.file_path     = q.getColumn(5).getText();
                    r.line_start    = q.getColumn(6).getInt();
                    symbol_map[id]  = std::move(r);
                }
            } catch (...) {}
        }
    }

    HybridSearchResult hybrid_result;
    hybrid_result.results = rrf_merge(fts5_results, faiss_results, symbol_map,
                                       effective_limit, config_.rrf_k);
    hybrid_result.search_mode = "hybrid";
    return hybrid_result;
}

std::vector<SearchResult> rrf_merge(
    const std::vector<SearchResult>& fts5_results,
    const std::vector<std::pair<int64_t, float>>& faiss_results,
    const std::unordered_map<int64_t, SearchResult>& symbol_lookup,
    int limit,
    int rrf_k)
{
    // Accumulate RRF scores and provenance
    std::unordered_map<int64_t, double> scores;
    std::unordered_map<int64_t, std::string> provenance;

    for (int i = 0; i < static_cast<int>(fts5_results.size()); ++i) {
        int64_t id = fts5_results[i].symbol_id;
        scores[id] += 1.0 / (rrf_k + i + 1);
        provenance[id] = "fts5";
    }
    for (int i = 0; i < static_cast<int>(faiss_results.size()); ++i) {
        int64_t id = faiss_results[i].first;
        if (id < 0) continue;
        scores[id] += 1.0 / (rrf_k + i + 1);
        auto it = provenance.find(id);
        if (it != provenance.end() && it->second == "fts5") {
            it->second = "both";
        } else {
            provenance[id] = "vector";
        }
    }

    // Sort by RRF score descending
    std::vector<std::pair<int64_t, double>> ranked(scores.begin(), scores.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<SearchResult> results;
    results.reserve(static_cast<size_t>(std::min(static_cast<int>(ranked.size()), limit)));
    for (int i = 0; i < limit && i < static_cast<int>(ranked.size()); ++i) {
        int64_t id = ranked[i].first;
        auto it = symbol_lookup.find(id);
        if (it == symbol_lookup.end()) continue;
        SearchResult r = it->second;
        r.rank = ranked[i].second;
        r.provenance = provenance.at(id);
        results.push_back(std::move(r));
    }
    return results;
}

HybridSearchResult HybridSearchEngine::search_text(
    const std::string& query,
    const std::string& language,
    int limit)
{
    return run_hybrid(query, /*kind=*/"", language, limit);
}

HybridSearchResult HybridSearchEngine::search_symbols(
    const std::string& query,
    const std::string& kind,
    const std::string& language,
    int limit)
{
    return run_hybrid(query, kind, language, limit);
}

} // namespace codetldr
