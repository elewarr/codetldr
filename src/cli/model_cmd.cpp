#include "cli/model_cmd.h"
#include "cli/sha256.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

// CodeRankEmbed INT8 ONNX model from nomic-ai on HuggingFace.
// Using /resolve/main/ (floating pointer) — SHA256 left empty for development.
// To pin a specific commit: replace "main" with commit SHA and fill in hashes.
constexpr const char* kModelUrl =
    "https://huggingface.co/nomic-ai/CodeRankEmbed/resolve/main/onnx/model_quantized.onnx";
constexpr const char* kTokenizerUrl =
    "https://huggingface.co/nomic-ai/CodeRankEmbed/resolve/main/tokenizer.json";

// SHA256 hashes for integrity verification.
// Empty string means "skip verification" (development/floating mode).
// Fill in after pinning commit SHA in the URLs above.
constexpr const char* kModelSha256     = "";
constexpr const char* kTokenizerSha256 = "";

// Subdirectory under XDG_CACHE_HOME where models are stored.
constexpr const char* kModelDir = "codetldr/models/CodeRankEmbed";

}  // namespace

// Download a single file using curl CLI, then optionally verify SHA256.
// Returns true on success, false on failure (also removes dest on failure).
static bool download_file(const std::string& url,
                          const std::filesystem::path& dest,
                          const std::string& expected_sha256) {
    std::cout << "Downloading " << dest.filename().string() << " ...\n";
    std::cout.flush();

    // curl flags:
    //   -L  follow redirects (HuggingFace uses redirects)
    //   --progress-bar  human-readable progress
    //   --fail  exit non-zero on HTTP error
    // Redirect stderr to stdout so progress bar is visible via popen.
    std::string cmd = "curl -L --progress-bar --fail \""
                    + url + "\" -o \"" + dest.string() + "\" 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        std::cerr << "Failed to launch curl\n";
        return false;
    }
    char buf[256];
    while (fgets(buf, sizeof(buf), p)) {
        std::cout << buf;
        std::cout.flush();
    }
    int rc = pclose(p);
    if (rc != 0) {
        std::cerr << "Download failed (curl exit " << rc << ")\n";
        std::filesystem::remove(dest);
        return false;
    }

    // Verify SHA256 (skip if expected is empty -- development/floating mode)
    if (!expected_sha256.empty()) {
        std::string actual = sha256_file(dest);
        if (actual != expected_sha256) {
            std::cerr << "SHA256 mismatch!\n"
                      << "  Expected: " << expected_sha256 << "\n"
                      << "  Got:      " << actual << "\n";
            std::filesystem::remove(dest);
            return false;
        }
    }

    std::cout << "OK (" << dest.filename().string() << ")\n";
    return true;
}

void register_model_cmd(CLI::App& app, std::string& /*project_root_str*/) {
    auto* model_cmd = app.add_subcommand("model", "Manage embedding models");
    model_cmd->require_subcommand(1);

    auto* download_cmd = model_cmd->add_subcommand("download",
        "Download the CodeRankEmbed INT8 embedding model to the XDG cache path");

    download_cmd->callback([]() {
        namespace fs = std::filesystem;

        // Resolve XDG cache directory (XDG Base Directory Specification)
        const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
        fs::path cache_home;
        if (xdg_cache && xdg_cache[0] != '\0') {
            cache_home = fs::path(xdg_cache);
        } else {
            const char* home = std::getenv("HOME");
            if (!home || home[0] == '\0') {
                std::cerr << "Cannot determine home directory (HOME not set)\n";
                std::exit(1);
            }
            cache_home = fs::path(home) / ".cache";
        }

        fs::path model_dir = cache_home / kModelDir;
        fs::create_directories(model_dir);

        fs::path model_path     = model_dir / "model_quantized.onnx";
        fs::path tokenizer_path = model_dir / "tokenizer.json";

        // Check if already downloaded and verify integrity
        bool model_exists     = fs::exists(model_path);
        bool tokenizer_exists = fs::exists(tokenizer_path);

        if (model_exists && tokenizer_exists) {
            // If SHA256 pins are set, verify before skipping download
            if (kModelSha256[0] != '\0') {
                std::string actual = sha256_file(model_path);
                if (actual == kModelSha256) {
                    std::cout << "Model already downloaded and verified at "
                              << model_dir.string() << "\n";
                    return;
                }
                std::cout << "SHA256 mismatch on cached model -- re-downloading\n";
            } else {
                std::cout << "Model already downloaded at " << model_dir.string() << "\n";
                return;
            }
        }

        // Download model ONNX file
        if (!download_file(kModelUrl, model_path, kModelSha256)) {
            std::exit(1);
        }

        // Download tokenizer
        if (!download_file(kTokenizerUrl, tokenizer_path, kTokenizerSha256)) {
            // Clean up model too on tokenizer failure
            fs::remove(model_path);
            std::exit(1);
        }

        std::cout << "\nModel downloaded to " << model_dir.string() << "\n";
        std::cout << "Restart the daemon to enable semantic search.\n";
    });
}
