/// vector_store.cpp — VectorStore implementation (Phase 16)
///
/// FAISS API notes (from build/_deps/faiss-src/faiss/):
///   - IndexIDMap2::own_fields: default false. Set true + release inner unique_ptr
///     to avoid double-free.
///   - read_index_up: returns unique_ptr<faiss::Index> (base). dynamic_cast required.
///   - save(): uses shared_lock (reads index state only), not exclusive.
///   - IDSelectorBatch: O(1) hash lookup for remove. Prefer over IDSelectorArray.
#include "embedding/vector_store.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexIDMap.h>
#include <faiss/MetricType.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/index_io.h>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace codetldr {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Build a fresh IndexIDMap2(IndexFlatL2(dim)).
/// Sets own_fields = true so IndexIDMap2 destructor deletes the inner flat index.
static std::unique_ptr<faiss::IndexIDMap2> make_fresh_index(int dim) {
    // Allocate inner flat index with raw new — ownership transfers to IndexIDMap2.
    auto* flat_raw = new faiss::IndexFlatL2(dim);
    auto index = std::make_unique<faiss::IndexIDMap2>(flat_raw);
    index->own_fields = true; // IndexIDMap2 will delete flat_raw in its destructor
    // Do NOT hold flat_raw in a unique_ptr after this point — that would double-free.
    return index;
}

// ---------------------------------------------------------------------------
// VectorStore::open
// ---------------------------------------------------------------------------

VectorStore VectorStore::open(const std::filesystem::path& faiss_path, int dim) {
    VectorStore vs;
    vs.path_ = faiss_path;
    vs.dim_ = dim;

    if (fs::exists(faiss_path)) {
        // Attempt to load existing index.
        try {
            auto loaded = faiss::read_index_up(faiss_path.c_str());
            auto* as_idmap = dynamic_cast<faiss::IndexIDMap2*>(loaded.get());

            if (!as_idmap) {
                spdlog::warn(
                    "VectorStore: {} has wrong index type (not IndexIDMap2) — rebuilding",
                    faiss_path.string());
                std::filesystem::remove(faiss_path);
                vs.index_ = make_fresh_index(dim);
            } else if (loaded->d != dim) {
                spdlog::warn(
                    "VectorStore: {} dimension mismatch (got {}, expected {}) — rebuilding",
                    faiss_path.string(), loaded->d, dim);
                std::filesystem::remove(faiss_path);
                vs.index_ = make_fresh_index(dim);
            } else {
                // Valid: transfer ownership from unique_ptr<Index> to unique_ptr<IndexIDMap2>.
                loaded.release(); // prevents Index destructor; IndexIDMap2 dtor handles cleanup
                vs.index_.reset(as_idmap);
            }
        } catch (const std::exception& e) {
            spdlog::warn(
                "VectorStore: failed to load {} ({}), rebuilding",
                faiss_path.string(), e.what());
            std::filesystem::remove(faiss_path);
            vs.index_ = make_fresh_index(dim);
        }
    } else {
        vs.index_ = make_fresh_index(dim);
    }

    return vs;
}

// ---------------------------------------------------------------------------
// Destructor / move
// ---------------------------------------------------------------------------

VectorStore::~VectorStore() = default;

VectorStore::VectorStore(VectorStore&& other) noexcept
    : path_(std::move(other.path_)),
      dim_(other.dim_),
      index_(std::move(other.index_)) {
    other.dim_ = 0;
}

VectorStore& VectorStore::operator=(VectorStore&& other) noexcept {
    if (this != &other) {
        path_ = std::move(other.path_);
        dim_ = other.dim_;
        index_ = std::move(other.index_);
        other.dim_ = 0;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// add
// ---------------------------------------------------------------------------

void VectorStore::add(int64_t id, const std::vector<float>& vec) {
    if (static_cast<int>(vec.size()) != dim_) {
        throw std::invalid_argument(
            "VectorStore::add: vector size " + std::to_string(vec.size()) +
            " != dim " + std::to_string(dim_));
    }
    const faiss::idx_t fid = static_cast<faiss::idx_t>(id);
    std::unique_lock lock(mutex_);
    index_->add_with_ids(1, vec.data(), &fid);
}

// ---------------------------------------------------------------------------
// add_batch
// ---------------------------------------------------------------------------

void VectorStore::add_batch(
    const std::vector<std::pair<int64_t, std::vector<float>>>& items) {
    if (items.empty()) return;

    // Validate all vectors and build contiguous matrix.
    std::vector<float> matrix;
    std::vector<faiss::idx_t> ids;
    matrix.reserve(items.size() * static_cast<size_t>(dim_));
    ids.reserve(items.size());

    for (const auto& [id, vec] : items) {
        if (static_cast<int>(vec.size()) != dim_) {
            throw std::invalid_argument(
                "VectorStore::add_batch: vector size " + std::to_string(vec.size()) +
                " != dim " + std::to_string(dim_));
        }
        ids.push_back(static_cast<faiss::idx_t>(id));
        matrix.insert(matrix.end(), vec.begin(), vec.end());
    }

    std::unique_lock lock(mutex_);
    index_->add_with_ids(static_cast<faiss::idx_t>(items.size()),
                         matrix.data(), ids.data());
}

// ---------------------------------------------------------------------------
// remove
// ---------------------------------------------------------------------------

void VectorStore::remove(const std::vector<int64_t>& ids) {
    if (ids.empty()) return;

    // IDSelectorBatch copies IDs into an unordered_set — O(1) lookup per vector.
    // The ids vector can be released after construction.
    faiss::IDSelectorBatch sel(ids.size(),
                               reinterpret_cast<const faiss::idx_t*>(ids.data()));
    std::unique_lock lock(mutex_);
    index_->remove_ids(sel);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

std::vector<std::pair<int64_t, float>> VectorStore::search(
    const std::vector<float>& query, int k) const {
    if (static_cast<int>(query.size()) != dim_) {
        throw std::invalid_argument(
            "VectorStore::search: query size " + std::to_string(query.size()) +
            " != dim " + std::to_string(dim_));
    }

    std::shared_lock lock(mutex_);

    if (index_->ntotal == 0) return {};

    const int actual_k = static_cast<int>(
        std::min(static_cast<int64_t>(k), index_->ntotal));

    std::vector<faiss::idx_t> labels(actual_k, -1);
    std::vector<float> distances(actual_k, std::numeric_limits<float>::max());

    index_->search(1, query.data(), actual_k, distances.data(), labels.data());

    std::vector<std::pair<int64_t, float>> results;
    results.reserve(actual_k);
    for (int i = 0; i < actual_k; ++i) {
        if (labels[i] != -1) {
            results.emplace_back(static_cast<int64_t>(labels[i]), distances[i]);
        }
    }
    return results;
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

void VectorStore::save() const {
    // Ensure parent directory exists.
    std::filesystem::create_directories(path_.parent_path());

    auto tmp_path = path_.parent_path() / (path_.filename().string() + ".tmp");

    std::shared_lock lock(mutex_); // read-only on index structure
    faiss::write_index(index_.get(), tmp_path.c_str());
    std::filesystem::rename(tmp_path, path_);
}

// ---------------------------------------------------------------------------
// ntotal
// ---------------------------------------------------------------------------

int64_t VectorStore::ntotal() const {
    std::shared_lock lock(mutex_);
    return static_cast<int64_t>(index_->ntotal);
}

} // namespace codetldr
