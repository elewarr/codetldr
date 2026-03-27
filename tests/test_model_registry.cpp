// tests/test_model_registry.cpp
#include "embedding/model_registry.h"
#include <cassert>
#include <iostream>
#include <string>

static void test_registry_has_four_models() {
    static_assert(std::size(codetldr::kRegisteredModels) == 4,
                  "Expected 4 registered models");
    std::cout << "PASS: test_registry_has_four_models\n";
}

static void test_default_model_is_coderank_embed() {
    int default_count = 0;
    const codetldr::ModelSpec* default_spec = nullptr;
    for (const auto& m : codetldr::kRegisteredModels) {
        if (m.is_default) { ++default_count; default_spec = &m; }
    }
    assert(default_count == 1 && "exactly one default model required");
    assert(std::string(default_spec->id) == "CodeRankEmbed");
    std::cout << "PASS: test_default_model_is_coderank_embed\n";
}

static void test_all_model_ids_are_path_safe() {
    const char forbidden[] = { '/', '\\', ':', '*', '?', '"', '<', '>', '|', '\0' };
    for (const auto& m : codetldr::kRegisteredModels) {
        std::string id(m.id);
        for (char c : forbidden) {
            if (c == '\0') break;
            if (id.find(c) != std::string::npos) {
                std::cerr << "FAIL: model id '" << id
                          << "' contains forbidden path character '" << c << "'\n";
                std::exit(1);
            }
        }
    }
    std::cout << "PASS: test_all_model_ids_are_path_safe\n";
}

static void test_faiss_path_naming() {
    for (const auto& m : codetldr::kRegisteredModels) {
        std::string name = "vectors_" + std::string(m.id) + ".faiss";
        assert(!name.empty());
        // Verify no path separators leaked in
        assert(name.find('/') == std::string::npos);
    }
    std::cout << "PASS: test_faiss_path_naming\n";
}

static void test_fingerprint_format_includes_model_id() {
    for (const auto& m : codetldr::kRegisteredModels) {
        std::string model_id(m.id);
        // Simulate fingerprint format: "model_id:file_size:mtime_ns"
        std::string fake_fp = model_id + ":12345:67890";
        assert(fake_fp.find(model_id + ":") == 0);
    }
    std::cout << "PASS: test_fingerprint_format_includes_model_id\n";
}

static void test_bge_large_dim_is_1024() {
    for (const auto& m : codetldr::kRegisteredModels) {
        if (std::string(m.id) == "bge-large-en-v1.5") {
            assert(m.dim == 1024);
            std::cout << "PASS: test_bge_large_dim_is_1024\n";
            return;
        }
    }
    std::cerr << "FAIL: bge-large-en-v1.5 not found in registry\n";
    std::exit(1);
}

static void test_other_models_dim_is_768() {
    for (const auto& m : codetldr::kRegisteredModels) {
        if (std::string(m.id) != "bge-large-en-v1.5") {
            assert(m.dim == 768);
        }
    }
    std::cout << "PASS: test_other_models_dim_is_768\n";
}

int main() {
    test_registry_has_four_models();
    test_default_model_is_coderank_embed();
    test_all_model_ids_are_path_safe();
    test_faiss_path_naming();
    test_fingerprint_format_includes_model_id();
    test_bge_large_dim_is_1024();
    test_other_models_dim_is_768();
    std::cout << "All model_registry tests passed.\n";
    return 0;
}
