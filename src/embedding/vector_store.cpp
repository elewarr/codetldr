#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH

#include "embedding/vector_store.h"
#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>
#include <algorithm>
#include <stdexcept>

namespace codetldr {

VectorStore::VectorStore(int dim)
    : dim_(dim)
    , index_(std::make_unique<faiss::IndexIDMap2>(new faiss::IndexFlatL2(dim)))
{
}

void VectorStore::add(int64_t symbol_id, const std::vector<float>& vec) {
    if (static_cast<int>(vec.size()) != dim_) {
        throw std::invalid_argument("VectorStore::add: vector dimension mismatch");
    }
    std::unique_lock lock(mutex_);
    // IDMap2 supports remove — remove existing entry if present, then add.
    faiss::IDSelectorBatch sel(1, &symbol_id);
    index_->remove_ids(sel);
    index_->add_with_ids(1, vec.data(), &symbol_id);
}

void VectorStore::remove(int64_t symbol_id) {
    std::unique_lock lock(mutex_);
    faiss::IDSelectorBatch sel(1, &symbol_id);
    index_->remove_ids(sel);
}

std::vector<std::pair<int64_t, float>> VectorStore::search(
    const std::vector<float>& query, int k) const {
    return search(query, k, nullptr);
}

std::vector<std::pair<int64_t, float>> VectorStore::search(
    const std::vector<float>& query, int k,
    const faiss::SearchParameters* params) const {

    std::shared_lock lock(mutex_);
    if (!index_ || index_->ntotal == 0) return {};
    if (static_cast<int>(query.size()) != dim_) return {};

    int actual_k = std::min(k, static_cast<int>(index_->ntotal));
    std::vector<faiss::idx_t> ids(actual_k, -1);
    std::vector<float> dists(actual_k, 0.0f);

    index_->search(1, query.data(), actual_k,
                   dists.data(), ids.data(),
                   const_cast<faiss::SearchParameters*>(params));

    std::vector<std::pair<int64_t, float>> results;
    results.reserve(actual_k);
    for (int i = 0; i < actual_k; ++i) {
        if (ids[i] >= 0) {
            results.emplace_back(ids[i], dists[i]);
        }
    }
    return results;
}

int64_t VectorStore::ntotal() const {
    std::shared_lock lock(mutex_);
    if (!index_) return 0;
    return index_->ntotal;
}

void VectorStore::save(const std::filesystem::path& path) const {
    std::shared_lock lock(mutex_);
    faiss::write_index(index_.get(), path.string().c_str());
}

void VectorStore::load(const std::filesystem::path& path) {
    std::unique_lock lock(mutex_);
    faiss::Index* raw = faiss::read_index(path.string().c_str());
    index_.reset(dynamic_cast<faiss::IndexIDMap2*>(raw));
    if (!index_) {
        delete raw;
        throw std::runtime_error("VectorStore::load: file is not an IndexIDMap2");
    }
}

} // namespace codetldr

#endif // CODETLDR_ENABLE_SEMANTIC_SEARCH
