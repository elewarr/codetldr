#pragma once
#include <cstdint>

namespace codetldr {

/// VectorStore: manages a FAISS index for semantic similarity search.
/// Phase 15+ will implement the full VectorStore with FAISS.
/// Phase 22 uses only ntotal() for observability (INDEX_INCONSISTENT check).
class VectorStore {
public:
    VectorStore() = default;
    ~VectorStore() = default;

    /// Return total number of vectors in the FAISS index.
    int64_t ntotal() const noexcept { return ntotal_; }

private:
    int64_t ntotal_ = 0;
};

} // namespace codetldr
