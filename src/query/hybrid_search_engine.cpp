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

std::vector<SearchResult> HybridSearchEngine::run_hybrid(
    const std::string& query, const std::string& kind,
    const std::string& language, int limit)
{
    int candidate_limit = std::max(limit * config_.candidate_multiplier, 60);

    if (!vector_available()) {
        // HYB-03: degrade to FTS5-only transparently
        auto results = fts5_.search_symbols(query, kind, language, limit);
        for (auto& r : results) r.provenance = "fts5";
        return results;
    }

    // HYB-02: parallel retrieval via std::async
    auto fts5_future = std::async(std::launch::async, [&]() {
        return fts5_.search_symbols(query, kind, language, candidate_limit);
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
            return store_->search(qvec, candidate_limit, &params);
        }
        return store_->search(qvec, candidate_limit);
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

    // Accumulate RRF scores and provenance
    const int k = config_.rrf_k;
    std::unordered_map<int64_t, double> scores;
    std::unordered_map<int64_t, std::string> provenance;

    for (int i = 0; i < static_cast<int>(fts5_results.size()); ++i) {
        int64_t id = fts5_results[i].symbol_id;
        scores[id] += 1.0 / (k + i + 1);
        provenance[id] = "fts5";
    }
    for (int i = 0; i < static_cast<int>(faiss_results.size()); ++i) {
        int64_t id = faiss_results[i].first;
        if (id < 0) continue;
        scores[id] += 1.0 / (k + i + 1);
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
        auto it = symbol_map.find(id);
        if (it == symbol_map.end()) continue;
        SearchResult r = it->second;
        r.rank = ranked[i].second;
        r.provenance = provenance.at(id);
        results.push_back(std::move(r));
    }
    return results;
}

std::vector<SearchResult> HybridSearchEngine::search_text(
    const std::string& query,
    const std::string& language,
    int limit)
{
    return run_hybrid(query, /*kind=*/"", language, limit);
}

std::vector<SearchResult> HybridSearchEngine::search_symbols(
    const std::string& query,
    const std::string& kind,
    const std::string& language,
    int limit)
{
    return run_hybrid(query, kind, language, limit);
}

} // namespace codetldr
