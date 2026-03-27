// src/embedding/model_registry.h
// Model registry: single source of truth for all registered embedding models.
// Consumed by model_cmd.cpp (CLI), coordinator.cpp (daemon), and tests.
// No ONNX Runtime or other heavy dependencies — pure header, always compiled.
#pragma once
#include <array>
#include <string_view>

namespace codetldr {

struct ModelSpec {
    const char* id;
    const char* display_name;
    int         dim;
    int         max_tokens;
    const char* quantization;
    const char* model_url;
    const char* tokenizer_url;
    const char* model_sha256;     // "" = skip verification (development mode)
    const char* tokenizer_sha256; // "" = skip verification
    const char* cache_subdir;     // relative to XDG_CACHE_HOME
    bool        is_default;
};

// 4 registered models for v1.4. Extend here for future releases.
// model_sha256/tokenizer_sha256 left empty until commit SHAs are pinned.
inline constexpr ModelSpec kRegisteredModels[] = {
    {
        "CodeRankEmbed",
        "CodeRankEmbed (nomic-ai, 768-dim, INT8)",
        768, 512, "INT8",
        "https://huggingface.co/nomic-ai/CodeRankEmbed/resolve/main/onnx/model_quantized.onnx",
        "https://huggingface.co/nomic-ai/CodeRankEmbed/resolve/main/tokenizer.json",
        "", "",
        "codetldr/models/CodeRankEmbed",
        true
    },
    {
        "jina-code-v2",
        "jina-code-v2 (jina-ai, 768-dim, INT8)",
        768, 8192, "INT8",
        "https://huggingface.co/jinaai/jina-embeddings-v2-base-code/resolve/main/onnx/model_quantized.onnx",
        "https://huggingface.co/jinaai/jina-embeddings-v2-base-code/resolve/main/tokenizer.json",
        "", "",
        "codetldr/models/jina-code-v2",
        false
    },
    {
        "bge-base-en-v1.5",
        "bge-base-en-v1.5 (BAAI, 768-dim, INT8)",
        768, 512, "INT8",
        "https://huggingface.co/BAAI/bge-base-en-v1.5/resolve/main/onnx/model_quantized.onnx",
        "https://huggingface.co/BAAI/bge-base-en-v1.5/resolve/main/tokenizer.json",
        "", "",
        "codetldr/models/bge-base-en-v1.5",
        false
    },
    {
        "bge-large-en-v1.5",
        "bge-large-en-v1.5 (BAAI, 1024-dim, INT8)",
        1024, 512, "INT8",
        "https://huggingface.co/BAAI/bge-large-en-v1.5/resolve/main/onnx/model_quantized.onnx",
        "https://huggingface.co/BAAI/bge-large-en-v1.5/resolve/main/tokenizer.json",
        "", "",
        "codetldr/models/bge-large-en-v1.5",
        false
    },
};

inline constexpr std::size_t kRegisteredModelCount = std::size(kRegisteredModels);

// Find a registered model by ID. Returns nullptr if not found.
inline const ModelSpec* find_model(std::string_view id) {
    for (const auto& m : kRegisteredModels) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

// Return the default model spec (the one with is_default = true).
inline const ModelSpec& default_model() {
    for (const auto& m : kRegisteredModels) {
        if (m.is_default) return m;
    }
    return kRegisteredModels[0];  // fallback: first entry
}

} // namespace codetldr
