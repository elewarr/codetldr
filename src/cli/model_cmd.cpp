#include "cli/model_cmd.h"
#include "cli/sha256.h"
#include "embedding/model_registry.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// Resolve XDG_CACHE_HOME. Returns $XDG_CACHE_HOME if set, else ~/.cache.
static std::filesystem::path resolve_cache_home() {
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    if (xdg_cache && xdg_cache[0] != '\0') {
        return std::filesystem::path(xdg_cache);
    }
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0') {
        std::cerr << "Cannot determine home directory (HOME not set)\n";
        std::exit(1);
    }
    return std::filesystem::path(home) / ".cache";
}

// Read [embedding].model from a minimal TOML-like config file.
// Looks for a line like: model = "SomeModelId" inside [embedding] section.
// Returns "" if file doesn't exist or key is missing.
static std::string read_model_from_config(const std::filesystem::path& config_path) {
    if (!std::filesystem::exists(config_path)) return "";
    std::ifstream in(config_path);
    if (!in) return "";

    bool in_embedding_section = false;
    std::string line;
    while (std::getline(in, line)) {
        // Trim leading whitespace
        std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);

        // Section header
        if (trimmed[0] == '[') {
            in_embedding_section = (trimmed.find("[embedding]") == 0);
            continue;
        }

        if (!in_embedding_section) continue;

        // Look for: model = "..."
        if (trimmed.find("model") == 0) {
            std::size_t eq = trimmed.find('=');
            if (eq == std::string::npos) continue;
            std::string val = trimmed.substr(eq + 1);
            // Strip whitespace and quotes
            std::size_t q1 = val.find('"');
            std::size_t q2 = val.rfind('"');
            if (q1 != std::string::npos && q2 != q1) {
                return val.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }
    return "";
}

// Write (or overwrite) [embedding]\nmodel = "<id>"\n to config file.
// Creates parent directories if needed.
// Preserves other sections in existing files; overwrites [embedding] model key.
static bool write_model_to_config(const std::filesystem::path& config_path,
                                   const std::string& model_id) {
    namespace fs = std::filesystem;

    fs::create_directories(config_path.parent_path());

    // If file doesn't exist, write minimal config
    if (!fs::exists(config_path)) {
        std::ofstream out(config_path);
        if (!out) {
            std::cerr << "Failed to write config: " << config_path.string() << "\n";
            return false;
        }
        out << "[embedding]\nmodel = \"" << model_id << "\"\n";
        return true;
    }

    // File exists: read it, update or insert [embedding].model
    std::ifstream in(config_path);
    if (!in) {
        std::cerr << "Failed to read config: " << config_path.string() << "\n";
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    in.close();

    // Find [embedding] section and model key
    bool found_embedding = false;
    bool found_model_key = false;
    bool in_emb = false;

    for (auto& l : lines) {
        std::size_t s = l.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        std::string t = l.substr(s);
        if (t[0] == '[') {
            if (t.find("[embedding]") == 0) {
                found_embedding = true;
                in_emb = true;
            } else {
                in_emb = false;
            }
            continue;
        }
        if (in_emb && t.find("model") == 0 && t.find('=') != std::string::npos) {
            // Replace the line
            l = "model = \"" + model_id + "\"";
            found_model_key = true;
        }
    }

    if (found_embedding && !found_model_key) {
        // Insert model key after [embedding] header
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::size_t s = lines[i].find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            std::string t = lines[i].substr(s);
            if (t.find("[embedding]") == 0) {
                lines.insert(lines.begin() + static_cast<long>(i) + 1,
                             "model = \"" + model_id + "\"");
                break;
            }
        }
    }

    if (!found_embedding) {
        // Append [embedding] section
        lines.push_back("");
        lines.push_back("[embedding]");
        lines.push_back("model = \"" + model_id + "\"");
    }

    std::ofstream out(config_path);
    if (!out) {
        std::cerr << "Failed to write config: " << config_path.string() << "\n";
        return false;
    }
    for (const auto& l : lines) {
        out << l << "\n";
    }
    return true;
}

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

// Compute disk size in MB for a model install directory (model + tokenizer).
static double model_dir_size_mb(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    double total = 0.0;
    for (const auto& entry : { "model_quantized.onnx", "tokenizer.json" }) {
        fs::path p = dir / entry;
        std::error_code ec;
        auto sz = fs::file_size(p, ec);
        if (!ec) total += static_cast<double>(sz);
    }
    return total / (1024.0 * 1024.0);
}

}  // namespace

void register_model_cmd(CLI::App& app, std::string& project_root_str) {
    auto* model_cmd = app.add_subcommand("model", "Manage embedding models");
    model_cmd->require_subcommand(1);

    // ----------------------------------------------------------------
    // model list — tabular output with active model marked
    // ----------------------------------------------------------------
    auto* list_cmd = model_cmd->add_subcommand("list", "List available embedding models");
    list_cmd->callback([&project_root_str]() {
        namespace fs = std::filesystem;

        // Resolve active model: project config first, then global
        std::string active_id;
        if (!project_root_str.empty()) {
            fs::path proj_cfg = fs::path(project_root_str) / ".codetldr" / "config.toml";
            active_id = read_model_from_config(proj_cfg);
        }
        if (active_id.empty()) {
            // Try global config
            const char* xdg_cfg = std::getenv("XDG_CONFIG_HOME");
            fs::path config_home;
            if (xdg_cfg && xdg_cfg[0] != '\0') {
                config_home = fs::path(xdg_cfg);
            } else {
                const char* home = std::getenv("HOME");
                if (home && home[0] != '\0') {
                    config_home = fs::path(home) / ".config";
                }
            }
            if (!config_home.empty()) {
                fs::path global_cfg = config_home / "codetldr" / "config.toml";
                active_id = read_model_from_config(global_cfg);
            }
        }
        // Default fallback
        if (active_id.empty()) {
            active_id = codetldr::default_model().id;
        }

        // cache_subdir is relative to XDG_CACHE_HOME/codetldr
        fs::path cache_home = resolve_cache_home() / "codetldr";

        // Print table header
        std::cout << "\n";
        std::cout << std::left
                  << std::setw(2)  << " "
                  << std::setw(20) << "ID"
                  << std::setw(10) << "DIM"
                  << std::setw(8)  << "QUANT"
                  << std::setw(14) << "SIZE"
                  << "DESCRIPTION\n";
        std::cout << std::string(2 + 20 + 10 + 8 + 14 + 30, '-') << "\n";

        for (const auto& m : codetldr::kRegisteredModels) {
            fs::path model_dir = cache_home / m.cache_subdir;
            bool installed = fs::exists(model_dir / "model_quantized.onnx")
                          && fs::exists(model_dir / "tokenizer.json");
            bool is_active = (std::string(m.id) == active_id);

            std::string prefix   = is_active ? "* " : "  ";
            std::string size_str = installed
                ? (std::to_string(static_cast<int>(model_dir_size_mb(model_dir))) + " MB")
                : "not installed";

            std::cout << std::left
                      << std::setw(2)  << prefix
                      << std::setw(20) << m.id
                      << std::setw(10) << m.dim
                      << std::setw(8)  << m.quantization
                      << std::setw(14) << size_str
                      << m.display_name << "\n";
        }
        std::cout << "\n";
        std::cout << "Active model: " << active_id << "\n";
        std::cout << "Use 'codetldr model download <id>' to install a model.\n";
        std::cout << "Use 'codetldr model select <id>' to change the active model.\n\n";
    });

    // ----------------------------------------------------------------
    // model select <id> — validate + write to config.toml
    // ----------------------------------------------------------------
    auto* select_cmd = model_cmd->add_subcommand("select",
        "Set the active embedding model (writes to .codetldr/config.toml)");
    auto select_id = std::make_shared<std::string>();
    select_cmd->add_option("id", *select_id, "Model ID to activate")->required();
    select_cmd->callback([select_id, &project_root_str]() {
        namespace fs = std::filesystem;

        // Look up model in registry
        const codetldr::ModelSpec* spec = codetldr::find_model(*select_id);
        if (!spec) {
            std::cerr << "Error: unknown model '" << *select_id << "'\n";
            std::cerr << "Available models:\n";
            for (const auto& m : codetldr::kRegisteredModels) {
                std::cerr << "  " << m.id << "\n";
            }
            std::exit(1);
        }

        // Check if model is installed (cache_subdir is relative to XDG_CACHE_HOME/codetldr)
        fs::path cache_home = resolve_cache_home() / "codetldr";
        fs::path model_dir = cache_home / spec->cache_subdir;
        bool installed = fs::exists(model_dir / "model_quantized.onnx")
                      && fs::exists(model_dir / "tokenizer.json");
        if (!installed) {
            std::cerr << "Error: model '" << spec->id << "' is not installed.\n";
            std::cerr << "Run: codetldr model download " << spec->id << "\n";
            std::exit(1);
        }

        // Resolve project config path
        fs::path project_root;
        if (!project_root_str.empty()) {
            project_root = fs::path(project_root_str);
        } else {
            project_root = fs::current_path();
        }
        fs::path config_path = project_root / ".codetldr" / "config.toml";

        if (!write_model_to_config(config_path, spec->id)) {
            std::exit(1);
        }

        std::cout << "Model '" << spec->id << "' selected.\n";
        std::cout << "Restart the daemon to apply: codetldr restart\n";
    });

    // ----------------------------------------------------------------
    // model download [id] — generalized to accept any registered model ID
    // ----------------------------------------------------------------
    auto* download_cmd = model_cmd->add_subcommand("download",
        "Download an embedding model to the XDG cache");
    auto download_id = std::make_shared<std::string>("CodeRankEmbed");
    download_cmd->add_option("id", *download_id,
        "Model ID to download (default: CodeRankEmbed)")->default_val("CodeRankEmbed");
    download_cmd->callback([download_id]() {
        namespace fs = std::filesystem;

        // Look up model in registry
        const codetldr::ModelSpec* spec = codetldr::find_model(*download_id);
        if (!spec) {
            std::cerr << "Error: unknown model '" << *download_id << "'\n";
            std::cerr << "Available models:\n";
            for (const auto& m : codetldr::kRegisteredModels) {
                std::cerr << "  " << m.id << "\n";
            }
            std::exit(1);
        }

        // cache_subdir is relative to XDG_CACHE_HOME/codetldr
        fs::path cache_home = resolve_cache_home() / "codetldr";
        fs::path model_dir = cache_home / spec->cache_subdir;
        fs::create_directories(model_dir);

        fs::path model_path     = model_dir / "model_quantized.onnx";
        fs::path tokenizer_path = model_dir / "tokenizer.json";

        // Check if already downloaded and verify integrity
        bool model_exists     = fs::exists(model_path);
        bool tokenizer_exists = fs::exists(tokenizer_path);

        if (model_exists && tokenizer_exists) {
            // If SHA256 pins are set, verify before skipping download
            if (spec->model_sha256[0] != '\0') {
                std::string actual = sha256_file(model_path);
                if (actual == spec->model_sha256) {
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

        std::cout << "Downloading " << spec->display_name << "\n";

        // Download model ONNX file
        if (!download_file(spec->model_url, model_path, spec->model_sha256)) {
            std::exit(1);
        }

        // Download tokenizer
        if (!download_file(spec->tokenizer_url, tokenizer_path, spec->tokenizer_sha256)) {
            // Clean up model too on tokenizer failure
            fs::remove(model_path);
            std::exit(1);
        }

        std::cout << "\nModel downloaded to " << model_dir.string() << "\n";
        std::cout << "Restart the daemon to enable semantic search.\n";
    });
}
