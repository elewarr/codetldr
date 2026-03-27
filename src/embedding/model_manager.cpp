#include "embedding/model_manager.h"
#include <onnxruntime_cxx_api.h>
#include <tokenizers_cpp.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <cmath>
#include <numeric>
#include <optional>
#include <tuple>
#include <stdexcept>
#include <string>

namespace codetldr {

// ---------------------------------------------------------------------------
// PIMPL implementation struct — owns ORT and tokenizer objects
// ---------------------------------------------------------------------------
struct ModelManager::Impl {
    Ort::Env env_;
    std::optional<Ort::Session> session_;
    Ort::MemoryInfo memory_info_;
    std::unique_ptr<tokenizers::Tokenizer> tokenizer_;
    int32_t pad_token_id_ = 0;

    Impl()
        : env_(ORT_LOGGING_LEVEL_WARNING, "codetldr")
        , memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {}
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ModelManager::ModelManager(const std::filesystem::path& model_path,
                            const std::filesystem::path& tokenizer_json_path)
    : impl_(std::make_unique<Impl>())
    , status_(ModelStatus::model_not_installed)
{
    // Step 1: Check if model file exists. If absent, gracefully degrade.
    if (!std::filesystem::exists(model_path)) {
        spdlog::info("ModelManager: model file absent at {} — running in model_not_installed mode",
                     model_path.string());
        return;
    }

    // Step 2: Load tokenizer from JSON blob
    if (!std::filesystem::exists(tokenizer_json_path)) {
        spdlog::error("ModelManager: tokenizer JSON absent at {}", tokenizer_json_path.string());
        status_ = ModelStatus::load_failed;
        return;
    }

    try {
        std::ifstream tok_file(tokenizer_json_path);
        if (!tok_file.is_open()) {
            spdlog::error("ModelManager: could not open tokenizer JSON at {}",
                          tokenizer_json_path.string());
            status_ = ModelStatus::load_failed;
            return;
        }
        std::string json_blob((std::istreambuf_iterator<char>(tok_file)),
                               std::istreambuf_iterator<char>());
        impl_->tokenizer_ = tokenizers::Tokenizer::FromBlobJSON(json_blob);
        if (!impl_->tokenizer_) {
            spdlog::error("ModelManager: Tokenizer::FromBlobJSON returned null");
            status_ = ModelStatus::load_failed;
            return;
        }

        // Step 3: Get pad token id
        int32_t pad_id = impl_->tokenizer_->TokenToId("[PAD]");
        impl_->pad_token_id_ = (pad_id < 0) ? 0 : pad_id;
        spdlog::debug("ModelManager: pad_token_id={}", impl_->pad_token_id_);

    } catch (const std::exception& e) {
        spdlog::error("ModelManager: tokenizer load failed: {}", e.what());
        status_ = ModelStatus::load_failed;
        return;
    }

    // Step 4: Build SessionOptions
    try {
        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);

#if defined(__APPLE__)
        // Use the new string-based API (NOT the deprecated C function)
        // CoreML EP with CPU+NeuralEngine compute units
        try {
            session_options.AppendExecutionProvider("CoreML",
                {{"ModelFormat", "MLProgram"}, {"MLComputeUnits", "CPUAndNeuralEngine"}});
            spdlog::debug("ModelManager: CoreML EP registered");
        } catch (const std::exception& coreml_err) {
            // CoreML not available (e.g., macOS version too old) — fall back to CPU EP silently
            spdlog::info("ModelManager: CoreML EP unavailable ({}), using CPU EP",
                         coreml_err.what());
        }
#endif

        // Step 5: Create ORT session
        impl_->session_.emplace(impl_->env_,
                                model_path.c_str(),
                                session_options);

        // Step 6: Validate input/output names
        Ort::AllocatorWithDefaultOptions allocator;

        size_t input_count = impl_->session_->GetInputCount();
        size_t output_count = impl_->session_->GetOutputCount();

        spdlog::debug("ModelManager: model has {} inputs, {} outputs", input_count, output_count);

        if (input_count < 1 || output_count < 1) {
            spdlog::error("ModelManager: unexpected input/output count: {} inputs, {} outputs",
                           input_count, output_count);
            status_ = ModelStatus::load_failed;
            return;
        }

        // Log all input/output names for debugging
        for (size_t i = 0; i < input_count; ++i) {
            auto name = impl_->session_->GetInputNameAllocated(i, allocator);
            spdlog::debug("ModelManager: input[{}] = {}", i, name.get());
        }
        for (size_t i = 0; i < output_count; ++i) {
            auto name = impl_->session_->GetOutputNameAllocated(i, allocator);
            spdlog::debug("ModelManager: output[{}] = {}", i, name.get());
        }

    } catch (const Ort::Exception& ort_err) {
        spdlog::error("ModelManager: ORT session creation failed: {}", ort_err.what());
        status_ = ModelStatus::load_failed;
        return;
    } catch (const std::exception& e) {
        spdlog::error("ModelManager: session creation failed: {}", e.what());
        status_ = ModelStatus::load_failed;
        return;
    }

    status_ = ModelStatus::loaded;
    spdlog::info("ModelManager: model loaded successfully from {}", model_path.string());
}

ModelManager::~ModelManager() = default;

// ---------------------------------------------------------------------------
// Internal helper: tokenize and pad to kMaxTokens
// Returns (input_ids, attention_mask, token_type_ids) as int64_t vectors
// ---------------------------------------------------------------------------
static std::tuple<std::vector<int64_t>, std::vector<int64_t>, std::vector<int64_t>>
tokenize_and_pad(const std::string& text,
                 tokenizers::Tokenizer& tokenizer,
                 int32_t pad_token_id)
{
    // tokenizers-cpp Encode() returns std::vector<int32_t> — must cast to int64_t for ORT
    auto raw_ids = tokenizer.Encode(text);

    // Truncate to kMaxTokens
    if (static_cast<int>(raw_ids.size()) > ModelManager::kMaxTokens) {
        raw_ids.resize(ModelManager::kMaxTokens);
    }

    const int n = static_cast<int>(raw_ids.size());

    std::vector<int64_t> input_ids(ModelManager::kMaxTokens,
                                    static_cast<int64_t>(pad_token_id));
    std::vector<int64_t> attention_mask(ModelManager::kMaxTokens, 0LL);
    std::vector<int64_t> token_type_ids(ModelManager::kMaxTokens, 0LL);

    for (int i = 0; i < n; ++i) {
        input_ids[i] = static_cast<int64_t>(raw_ids[i]);
        attention_mask[i] = 1LL;
    }

    return {std::move(input_ids), std::move(attention_mask), std::move(token_type_ids)};
}

// ---------------------------------------------------------------------------
// embed()
// ---------------------------------------------------------------------------
std::vector<float> ModelManager::embed(const std::string& text, bool is_query)
{
    if (status_ != ModelStatus::loaded) {
        throw std::runtime_error("model_not_installed");
    }

    // Prepend query prefix if requested (EMB-04)
    const std::string input = is_query
        ? (std::string(kQueryPrefix) + text)
        : text;

    // Tokenize and pad
    auto [input_ids, attention_mask, token_type_ids] =
        tokenize_and_pad(input, *impl_->tokenizer_, impl_->pad_token_id_);

    // Build ORT tensors — shape: {1, kMaxTokens}
    const std::array<int64_t, 2> shape = {1, kMaxTokens};

    auto ids_tensor = Ort::Value::CreateTensor<int64_t>(
        impl_->memory_info_,
        input_ids.data(), input_ids.size(),
        shape.data(), shape.size());

    auto mask_tensor = Ort::Value::CreateTensor<int64_t>(
        impl_->memory_info_,
        attention_mask.data(), attention_mask.size(),
        shape.data(), shape.size());

    auto type_tensor = Ort::Value::CreateTensor<int64_t>(
        impl_->memory_info_,
        token_type_ids.data(), token_type_ids.size(),
        shape.data(), shape.size());

    // Run inference
    const char* input_names[] = {"input_ids", "attention_mask", "token_type_ids"};
    const char* output_names[] = {"last_hidden_state"};

    std::array<Ort::Value, 3> inputs = {
        std::move(ids_tensor),
        std::move(mask_tensor),
        std::move(type_tensor)
    };

    auto outputs = impl_->session_->Run(
        Ort::RunOptions{nullptr},
        input_names, inputs.data(), 3,
        output_names, 1);

    // Extract last_hidden_state — shape [1, 512, 768]
    const float* hidden = outputs[0].GetTensorData<float>();

    // Mean-pool and L2-normalize
    return mean_pool_and_normalize(hidden, attention_mask.data(), kMaxTokens, kEmbeddingDim);
}

// ---------------------------------------------------------------------------
// mean_pool_and_normalize (static, public for testing)
// ---------------------------------------------------------------------------
std::vector<float> ModelManager::mean_pool_and_normalize(
    const float* last_hidden_state,
    const int64_t* attention_mask,
    int seq_len, int embed_dim)
{
    std::vector<float> result(embed_dim, 0.0f);
    int64_t token_count = 0;

    // Accumulate masked tokens
    for (int t = 0; t < seq_len; ++t) {
        if (attention_mask[t] == 1) {
            ++token_count;
            for (int d = 0; d < embed_dim; ++d) {
                result[d] += last_hidden_state[t * embed_dim + d];
            }
        }
    }

    // Degenerate case: no active tokens -> return zero vector
    if (token_count == 0) {
        return result;
    }

    // Mean
    const float inv_count = 1.0f / static_cast<float>(token_count);
    for (float& v : result) v *= inv_count;

    // L2 normalize
    float norm_sq = 0.0f;
    for (float v : result) norm_sq += v * v;
    const float norm = std::sqrt(norm_sq);

    if (norm >= 1e-12f) {
        const float inv_norm = 1.0f / norm;
        for (float& v : result) v *= inv_norm;
    }

    return result;
}

} // namespace codetldr
