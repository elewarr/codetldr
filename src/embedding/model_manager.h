#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <optional>

// Forward-declare to avoid including ORT headers in the public interface
namespace Ort { class Env; class Session; class MemoryInfo; class SessionOptions; }
namespace tokenizers { class Tokenizer; }

namespace codetldr {

enum class ModelStatus { loaded, model_not_installed, load_failed };

class ModelManager {
public:
    explicit ModelManager(const std::filesystem::path& model_path,
                          const std::filesystem::path& tokenizer_json_path);
    ~ModelManager();

    ModelManager(const ModelManager&) = delete;
    ModelManager& operator=(const ModelManager&) = delete;

    ModelStatus status() const noexcept { return status_; }

    // Embed text. is_query=true prepends instruction prefix (EMB-04).
    // Returns 768-dim L2-normalized float vector.
    // Throws std::runtime_error if status != loaded.
    std::vector<float> embed(const std::string& text, bool is_query = false);

    static constexpr int kEmbeddingDim = 768;
    static constexpr int kMaxTokens = 512;

    // Exposed for unit testing without a real model
    static std::vector<float> mean_pool_and_normalize(
        const float* last_hidden_state,
        const int64_t* attention_mask,
        int seq_len, int embed_dim);

    static constexpr const char* kQueryPrefix =
        "Represent this query for searching relevant code: ";

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  // PIMPL -- hides ORT headers
    ModelStatus status_ = ModelStatus::model_not_installed;
};

} // namespace codetldr
