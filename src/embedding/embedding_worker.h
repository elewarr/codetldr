#pragma once
// embedding_worker.h -- Background embedding worker thread (Phase 17)
//
// SRC-03: All ONNX inference runs off the main event loop thread.
// VEC-04: Incremental update per file — remove old vectors, embed new chunks, add back.
// MDL-03: Full rebuild triggered when model fingerprint changes (size:mtime proxy).
//
// Usage:
//   EmbeddingWorker worker(db, root, model_ptr, store_ptr, model_path);
//   // After analyze_file succeeds:
//   worker.enqueue(result.file_id);
//   // At startup if model hash changed:
//   worker.enqueue_full_rebuild();
//   // During shutdown (before database closes):
//   worker.stop();

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations — avoid pulling in heavy headers into the public interface
namespace SQLite { class Database; }

namespace codetldr {

class ModelManager;
class VectorStore;

class EmbeddingWorker {
public:
    /// Construct an EmbeddingWorker.
    /// If model or store is nullptr, the worker thread is NOT started and all
    /// enqueue() calls are no-ops. This allows the coordinator to unconditionally
    /// construct an EmbeddingWorker regardless of whether semantic search is enabled.
    ///
    /// db must remain valid for the lifetime of EmbeddingWorker (stop() must be
    /// called before the database is destroyed).
    ///
    /// model_path is used only for compute_model_fingerprint during full rebuild
    /// to update the stored fingerprint in the metadata table.
    EmbeddingWorker(SQLite::Database& db,
                    const std::filesystem::path& project_root,
                    ModelManager* model,
                    VectorStore* store,
                    const std::filesystem::path& model_path);

    ~EmbeddingWorker();

    EmbeddingWorker(const EmbeddingWorker&) = delete;
    EmbeddingWorker& operator=(const EmbeddingWorker&) = delete;

    /// Enqueue file_id for background re-embedding. Thread-safe.
    /// No-op if model_ == nullptr (semantic search disabled).
    void enqueue(int64_t file_id);

    /// Enqueue full-rebuild sentinel. Worker will:
    ///   1. Remove all existing vectors from VectorStore
    ///   2. Re-embed all files in the database
    ///   3. Update model_fingerprint in metadata table
    /// Thread-safe. No-op if model_ == nullptr.
    void enqueue_full_rebuild();

    /// Signal worker thread to stop and join. Safe to call multiple times.
    /// Blocks until the worker thread has exited.
    /// Must be called before the SQLite::Database passed to the constructor
    /// is destroyed.
    void stop();

    /// Compute a lightweight model fingerprint: "file_size:mtime_ns".
    /// Returns empty string on error (file not found, stat failure).
    /// Public static for testability.
    static std::string compute_model_fingerprint(const std::filesystem::path& model_path);

    /// Sentinel value pushed to the queue to trigger a full rebuild.
    static constexpr int64_t kFullRebuildSentinel = -1;

private:
    /// Worker thread entry point.
    void worker_loop();

    /// Embed all chunks for one file and update VectorStore incrementally.
    void process_file(int64_t file_id);

    /// Clear VectorStore, re-embed all files, update metadata fingerprint.
    void process_full_rebuild();

    /// Persist {file_id -> [symbol_ids]} mapping in embedded_files table.
    /// Replaces any previous entry for this file_id.
    void store_embedded_ids(int64_t file_id,
        const std::vector<std::pair<int64_t, std::vector<float>>>& batch);

    /// Retrieve symbol_ids previously embedded for file_id.
    std::vector<int64_t> get_embedded_ids(int64_t file_id);

    /// Update model_fingerprint in metadata table after a full rebuild.
    void update_metadata_fingerprint();

    SQLite::Database& db_;
    std::filesystem::path project_root_;
    ModelManager* model_;       // non-owning, nullable
    VectorStore* store_;        // non-owning, nullable
    std::filesystem::path model_path_;  // for fingerprint computation

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<int64_t> pending_ids_;
    bool stop_requested_ = false;
    bool active_ = false;       // true only if thread was started (model != nullptr)

    std::thread thread_;
};

} // namespace codetldr
