/// vector_store.h — Thread-safe, crash-safe FAISS vector index (Phase 16)
///
/// VEC-01: IndexIDMap2(IndexFlatL2) stores and retrieves vectors by int64 ID.
/// VEC-02: Atomic save via write_index to .tmp then std::filesystem::rename.
/// VEC-03: std::shared_mutex — shared lock for search/save, exclusive for add/remove.
/// VEC-05: Dimension validation at open(); auto-rebuild on mismatch or wrong type.
///
/// On L2-normalized vectors (CodeRankEmbed): distance = 2 - 2*cos(theta).
/// Lower distance = more similar. VectorStore exposes raw L2 distances.
/// Phase 18 (search surface) converts to cosine similarity if needed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <utility>
#include <vector>

// IndexIDMap2 is a type alias (using IndexIDMap2 = IndexIDMap2Template<Index>)
// so it cannot be forward-declared as a struct. Include the FAISS header here.
#include <faiss/IndexIDMap.h>

namespace codetldr {

class VectorStore {
public:
    /// Load existing index from faiss_path, or create a new empty index with the
    /// given dimension. On load, validates that the stored dimension matches dim
    /// and that the index type is IndexIDMap2. If either check fails, logs a warning
    /// and creates a fresh empty index (auto-rebuild per VEC-05).
    static VectorStore open(const std::filesystem::path& faiss_path, int dim);

    ~VectorStore();
    VectorStore(VectorStore&&) noexcept;
    VectorStore& operator=(VectorStore&&) noexcept;

    // Non-copyable: shared_mutex and unique_ptr are move-only.
    VectorStore(const VectorStore&) = delete;
    VectorStore& operator=(const VectorStore&) = delete;

    /// Add a single vector with the given application-level ID.
    /// vec.size() must equal dim(). Holds exclusive lock during call.
    /// Throws std::invalid_argument if vec.size() != dim().
    void add(int64_t id, const std::vector<float>& vec);

    /// Add a batch of (id, vector) pairs in a single FAISS call.
    /// Each vector must have size == dim(). Holds exclusive lock during call.
    /// Throws std::invalid_argument if any vector has wrong size.
    void add_batch(const std::vector<std::pair<int64_t, std::vector<float>>>& items);

    /// Remove vectors by their application-level IDs.
    /// Empty ids vector is a no-op. Holds exclusive lock during call.
    void remove(const std::vector<int64_t>& ids);

    /// K-nearest-neighbor search. Returns (id, L2_distance) pairs in ascending
    /// distance order. Shared lock held during call.
    /// Returns fewer than k results if index has fewer vectors than k.
    /// Returns empty vector if index is empty.
    std::vector<std::pair<int64_t, float>> search(
        const std::vector<float>& query, int k) const;

    /// Atomic save: write_index to path + ".tmp", then std::filesystem::rename.
    /// Shared lock is held (read-only on index state). Concurrent searches may
    /// proceed during save.
    void save() const;

    /// Total number of vectors currently in the index.
    int64_t ntotal() const;

    /// Embedding dimension (immutable after construction).
    int dim() const noexcept { return dim_; }

private:
    VectorStore() = default;

    std::filesystem::path path_;
    int dim_ = 0;

    // IndexIDMap2 owns the inner IndexFlatL2 via own_fields = true.
    // IMPORTANT: inner IndexFlatL2 is allocated with `new`, then passed to
    // IndexIDMap2 constructor. After setting own_fields = true, the raw pointer
    // must NOT also be held in a unique_ptr — that would double-free.
    std::unique_ptr<faiss::IndexIDMap2> index_;

    mutable std::shared_mutex mutex_;
};

} // namespace codetldr
