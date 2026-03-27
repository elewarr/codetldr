#include "embedding/model_manager.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    namespace fs = std::filesystem;

    // Test 1: Constants are correct (EMB-02, EMB-03)
    assert(codetldr::ModelManager::kEmbeddingDim == 768);
    assert(codetldr::ModelManager::kMaxTokens == 512);
    std::cout << "PASS: kEmbeddingDim=768, kMaxTokens=512\n";

    // Test 2: Query prefix is correct (EMB-04)
    std::string prefix(codetldr::ModelManager::kQueryPrefix);
    assert(prefix.find("Represent this query for searching relevant code") != std::string::npos);
    std::cout << "PASS: kQueryPrefix contains correct instruction\n";

    // Test 3: ModelManager with non-existent model -> model_not_installed (EMB-01)
    {
        codetldr::ModelManager mgr(
            fs::temp_directory_path() / "nonexistent_model.onnx",
            fs::temp_directory_path() / "nonexistent_tokenizer.json");
        assert(mgr.status() == codetldr::ModelStatus::model_not_installed);
        std::cout << "PASS: absent model -> model_not_installed\n";
    }

    // Test 4: embed() throws when model not loaded (EMB-01)
    {
        codetldr::ModelManager mgr(
            fs::temp_directory_path() / "nonexistent_model.onnx",
            fs::temp_directory_path() / "nonexistent_tokenizer.json");
        bool threw = false;
        try {
            mgr.embed("test");
        } catch (const std::runtime_error& e) {
            threw = true;
            assert(std::string(e.what()).find("model_not_installed") != std::string::npos);
        }
        assert(threw);
        std::cout << "PASS: embed() throws model_not_installed\n";
    }

    // Test 5: mean_pool_and_normalize with known input (EMB-03)
    // 3 tokens, 4 dims. Attention mask: [1, 1, 0] (only first 2 tokens)
    // Token 0 hidden: [1, 2, 3, 4], Token 1 hidden: [3, 4, 5, 6], Token 2 (masked): [99,99,99,99]
    {
        const int seq = 3, dim = 4;
        float hidden[] = {1,2,3,4, 3,4,5,6, 99,99,99,99};
        int64_t mask[] = {1, 1, 0};
        auto result = codetldr::ModelManager::mean_pool_and_normalize(hidden, mask, seq, dim);
        assert(result.size() == static_cast<size_t>(dim));
        // Mean: [(1+3)/2, (2+4)/2, (3+5)/2, (4+6)/2] = [2, 3, 4, 5]
        // Norm: sqrt(4+9+16+25) = sqrt(54) ~= 7.348
        float norm = 0;
        for (float v : result) norm += v * v;
        norm = std::sqrt(norm);
        assert(std::abs(norm - 1.0f) < 1e-5f);
        // Check ratios: result[0]/result[1] should be 2/3
        assert(std::abs(result[0] / result[1] - 2.0f/3.0f) < 1e-5f);
        std::cout << "PASS: mean_pool_and_normalize correct output, norm=1.0\n";
    }

    // Test 6: mean_pool_and_normalize with all-zero mask -> zero vector
    {
        const int seq = 2, dim = 3;
        float hidden[] = {1,2,3, 4,5,6};
        int64_t mask[] = {0, 0};
        auto result = codetldr::ModelManager::mean_pool_and_normalize(hidden, mask, seq, dim);
        for (float v : result) assert(v == 0.0f);
        std::cout << "PASS: all-zero mask -> zero vector\n";
    }

    // Test 7: mean_pool_and_normalize with single token
    {
        const int seq = 1, dim = 3;
        float hidden[] = {3, 4, 0};  // norm = 5
        int64_t mask[] = {1};
        auto result = codetldr::ModelManager::mean_pool_and_normalize(hidden, mask, seq, dim);
        assert(std::abs(result[0] - 3.0f/5.0f) < 1e-5f);
        assert(std::abs(result[1] - 4.0f/5.0f) < 1e-5f);
        assert(std::abs(result[2] - 0.0f) < 1e-5f);
        std::cout << "PASS: single token mean_pool correct\n";
    }

    std::cout << "\nAll embedding pipeline tests passed.\n";
    return 0;
}
