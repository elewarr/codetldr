#pragma once
#include "query/search_engine.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <string>
#include <vector>

namespace codetldr {

struct HybridSearchConfig {
    int rrf_k               = 60;   // RRF k parameter (Cormack et al. 2009)
    int candidate_multiplier = 3;   // candidate_limit = max(limit * multiplier, 60)
};

// Forward declarations
class ModelManager;
class VectorStore;

/// Hybrid search engine: runs FTS5 and FAISS in parallel (std::async) and merges
/// results via Reciprocal Rank Fusion (RRF). Degrades to FTS5-only when the
/// vector store is unavailable (null, empty, or model not loaded).
///
/// Owns a dedicated read-only SQLite connection to avoid interference with the
/// daemon's write connection.
class HybridSearchEngine {
public:
    /// db_path: path to the project .codetldr/index.sqlite
    /// model: nullable — if null, FAISS path is skipped
    /// store: nullable — if null, FAISS path is skipped
    HybridSearchEngine(const std::filesystem::path& db_path,
                       ModelManager* model,
                       VectorStore* store,
                       HybridSearchConfig config = {});

    /// Hybrid text search. Equivalent to the old search_text but now uses RRF.
    std::vector<SearchResult> search_text(const std::string& query,
                                          const std::string& language = "",
                                          int limit = 20);

    /// Hybrid symbol search. Kind filter applied to FTS5 path only.
    std::vector<SearchResult> search_symbols(const std::string& query,
                                              const std::string& kind,
                                              const std::string& language = "",
                                              int limit = 20);

private:
    SQLite::Database db_;    // owned read-only connection
    SearchEngine fts5_;      // wraps db_
    ModelManager* model_;    // non-owning, nullable
    VectorStore* store_;     // non-owning, nullable
    HybridSearchConfig config_;

    /// True if FAISS search is available (store non-null, non-empty, model loaded).
    bool vector_available() const;

    /// Core fusion: launch std::async for both paths, merge via RRF.
    std::vector<SearchResult> run_hybrid(const std::string& query,
                                          const std::string& kind,
                                          const std::string& language,
                                          int limit);
};

} // namespace codetldr
