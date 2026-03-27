// test_model_download.cpp
// Tests for sha256_file() in src/cli/sha256.h
// No network access required — uses temp files with known content.

#include "cli/sha256.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void test_sha256_known_content() {
    // Write a file with known content and verify its SHA256.
    // echo -n "hello world" | sha256sum
    //   b94d27b9934d3e08a52e52d7da7dabfac484efe04294e576a4b0e7ae3df3a688
    // echo "hello world" | sha256sum   (adds newline -> "hello world\n")
    //   a948904f2f0f479b8f8564e9d7ad53f9162eab4b628b23b151562f12f5fbb33e (with LF)
    //
    // We write "hello world\n" (10 bytes + LF = 12 bytes total).
    // SHA256("hello world\n") = a948904f2f0f479b8f8564e9d7ad53f9162eab4b628b23b151562f12f5fbb33e

    fs::path tmp = fs::temp_directory_path() / "codetldr_sha256_test.txt";

    {
        std::ofstream out(tmp, std::ios::binary);
        out << "hello world\n";
    }

    std::string result = sha256_file(tmp);
    fs::remove(tmp);

    std::string expected = "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447";
    if (result != expected) {
        std::cerr << "FAIL test_sha256_known_content\n"
                  << "  Expected: " << expected << "\n"
                  << "  Got:      " << result << "\n";
        std::exit(1);
    }
    std::cout << "PASS test_sha256_known_content\n";
}

static void test_sha256_nonexistent_returns_empty() {
    fs::path missing = fs::temp_directory_path() / "codetldr_no_such_file_xyz.bin";
    // Ensure it does not exist
    fs::remove(missing);

    std::string result = sha256_file(missing);

    if (!result.empty()) {
        std::cerr << "FAIL test_sha256_nonexistent_returns_empty\n"
                  << "  Expected empty string, got: " << result << "\n";
        std::exit(1);
    }
    std::cout << "PASS test_sha256_nonexistent_returns_empty\n";
}

static void test_sha256_mismatch_detection() {
    // Simulate SHA256 mismatch: create a file, compute its hash, compare
    // against a wrong expected hash — should be different (not match).
    fs::path tmp = fs::temp_directory_path() / "codetldr_sha256_mismatch.bin";

    {
        std::ofstream out(tmp, std::ios::binary);
        out << "some content for mismatch test\n";
    }

    std::string actual = sha256_file(tmp);
    fs::remove(tmp);

    // actual should be non-empty
    if (actual.empty()) {
        std::cerr << "FAIL test_sha256_mismatch_detection: sha256_file returned empty\n";
        std::exit(1);
    }
    // A clearly wrong hash should not match the actual hash
    std::string wrong_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    if (actual == wrong_hash) {
        std::cerr << "FAIL test_sha256_mismatch_detection: actual hash matched all-zeros\n";
        std::exit(1);
    }
    std::cout << "PASS test_sha256_mismatch_detection\n";
}

int main() {
    test_sha256_known_content();
    test_sha256_nonexistent_returns_empty();
    test_sha256_mismatch_detection();
    std::cout << "All tests passed.\n";
    return 0;
}
