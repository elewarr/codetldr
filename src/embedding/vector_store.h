#pragma once
#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH

#include <faiss/IndexIDMap.h>
#include <faiss/Index.h>
#include <faiss/impl/IDSelector.h>
#include <shared_mutex>
#include <vector>
#include <utility>
#include <cstdint>
#include <filesystem>

namespace codetldr {

/// Thread-safe FAISS vector store for symbol embeddings.
/// Uses IndexIDMap2 over IndexFlatL2 to map external symbol IDs to dense vectors.
///
/// Concurrency: shared_mutex — multiple concurrent readers, exclusive writes.
class VectorStore {
public:
    /// Construct an empty VectorStore with embedding dimension `dim`.
    explicit VectorStore(int dim);
    ~VectorStore() = default;

    // Non-copyable
    VectorStore(const VectorStore&) = delete;
    VectorStore& operator=(const VectorStore&) = delete;

    /// Add a single vector for the given symbol_id.
    /// Replaces any existing entry for that symbol_id (via IDMap2 remove+add).
    void add(int64_t symbol_id, const std::vector<float>& vec);

    /// Remove the vector for the given symbol_id.
    void remove(int64_t symbol_id);

    /// K-nearest-neighbor search (unfiltered).
    /// Returns (symbol_id, distance) pairs, best match first.
    /// Shared lock held during call.
    std::vector<std::pair<int64_t, float>> search(
        const std::vector<float>& query, int k) const;

    /// K-nearest-neighbor search with FAISS SearchParameters (e.g. IDSelectorBatch pre-filter).
    /// params may be nullptr — equivalent to calling the unfiltered overload.
    /// Shared lock held during call.
    std::vector<std::pair<int64_t, float>> search(
        const std::vector<float>& query, int k,
        const faiss::SearchParameters* params) const;

    /// Total number of vectors currently stored.
    int64_t ntotal() const;

    /// Persist the index to a file.
    void save(const std::filesystem::path& path) const;

    /// Load the index from a file. Replaces current index.
    void load(const std::filesystem::path& path);

private:
    int dim_;
    mutable std::shared_mutex mutex_;
    std::unique_ptr<faiss::IndexIDMap2> index_;
};

} // namespace codetldr

#endif // CODETLDR_ENABLE_SEMANTIC_SEARCH
