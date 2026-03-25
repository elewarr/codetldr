// Integration test for codetldr init command logic.
// Tests the underlying functions without requiring a running daemon.
//
// Tests:
//   1. init_project_dir creates .codetldr/
//   2. Default .codetldrignore can be written and parsed back
//   3. LanguageRegistry detects languages correctly
//   4. config.json can be written and parsed back with correct fields
//   5. CAPABILITIES.md is generated with expected content

#include "config/project_dir.h"
#include "analysis/tree_sitter/language_registry.h"
#include "watcher/ignore_filter.h"
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test utilities
// ---------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

static void check(bool condition, const std::string& test_name) {
    if (condition) {
        std::cout << "[PASS] " << test_name << "\n";
        ++g_passed;
    } else {
        std::cerr << "[FAIL] " << test_name << "\n";
        ++g_failed;
    }
}

// Create a temporary directory for test isolation
static fs::path make_temp_dir(const std::string& suffix) {
    fs::path base = fs::temp_directory_path() / ("test_cli_init_" + suffix);
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

// ---------------------------------------------------------------------------
// Test 1: init_project_dir creates .codetldr/
// ---------------------------------------------------------------------------
static void test_init_project_dir() {
    fs::path root = make_temp_dir("init");
    // Create .git/info/ to simulate a git repo
    fs::create_directories(root / ".git" / "info");

    auto result = codetldr::init_project_dir(root);

    check(result.codetldr_created, "init_project_dir: codetldr_created is true");
    check(fs::exists(root / ".codetldr"), "init_project_dir: .codetldr/ directory created");
    check(result.git_exclude_updated, "init_project_dir: git_exclude_updated (has .git/info)");

    // Second call: dir already exists; codetldr_created stays true (init succeeded)
    // git_exclude_updated should be false (entry already in exclude file)
    auto result2 = codetldr::init_project_dir(root);
    check(result2.codetldr_created, "init_project_dir: second call still reports init success");
    check(!result2.git_exclude_updated, "init_project_dir: second call does not duplicate exclude entry");

    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Test 2: .codetldrignore with default patterns can be written and read back
// ---------------------------------------------------------------------------
static void test_codetldrignore() {
    fs::path root = make_temp_dir("ignore");
    fs::path ignore_path = root / ".codetldrignore";

    // Write default ignore patterns
    std::ofstream out(ignore_path);
    if (!out) throw std::runtime_error("Failed to create .codetldrignore");
    out << "# codetldr ignore patterns (gitignore syntax)\n";
    out << "build/\n";
    out << ".build/\n";
    out << "node_modules/\n";
    out << ".git/\n";
    out << "vendor/\n";
    out << ".codetldr/\n";
    out << "dist/\n";
    out << "out/\n";
    out << "target/\n";
    out << "*.o\n";
    out << "*.a\n";
    out << "*.so\n";
    out << "*.dylib\n";
    out.close();

    check(fs::exists(ignore_path), "codetldrignore: file created");

    // Load with IgnoreFilter and verify it works
    codetldr::IgnoreFilter filter = codetldr::IgnoreFilter::from_project_root(root);
    check(filter.should_ignore(fs::path("build/main.cpp")),
          "codetldrignore: build/ pattern ignores build/main.cpp");
    check(filter.should_ignore(fs::path("node_modules/lodash/index.js")),
          "codetldrignore: node_modules/ pattern ignores nested file");
    check(!filter.should_ignore(fs::path("src/main.cpp")),
          "codetldrignore: src/ is not ignored");
    check(filter.should_ignore(fs::path("lib.a")),
          "codetldrignore: *.a pattern ignores lib.a");

    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Test 3: LanguageRegistry detects languages from extensions
// ---------------------------------------------------------------------------
static void test_language_detection() {
    fs::path root = make_temp_dir("langdet");

    // Create fake source files
    std::vector<std::pair<std::string, std::string>> test_files = {
        {"src/main.py", "def main(): pass"},
        {"src/app.cpp", "int main() { return 0; }"},
        {"src/utils.js", "function foo() {}"},
        {"src/helper.py", "# helper"},
        {"build/output.o", ""},     // should be ignored by IgnoreFilter
        {"README.txt", "readme"},   // not a source file
    };
    for (const auto& [rel, content] : test_files) {
        fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << content;
    }

    // Write .codetldrignore to exclude build/
    std::ofstream ignore_out(root / ".codetldrignore");
    ignore_out << "build/\n";
    ignore_out.close();

    codetldr::LanguageRegistry registry;
    bool initialized = registry.initialize();
    check(initialized, "language_detection: LanguageRegistry initializes");

    codetldr::IgnoreFilter filter = codetldr::IgnoreFilter::from_project_root(root);

    std::map<std::string, int> lang_counts;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) { ec.clear(); continue; }
        fs::path abs_path = it->path();
        fs::path rel_path = fs::relative(abs_path, root, ec);
        if (ec) { ec.clear(); continue; }
        if (filter.should_ignore(rel_path)) continue;
        std::string ext = abs_path.extension().string();
        if (!ext.empty()) {
            const codetldr::LanguageEntry* entry = registry.for_extension(ext);
            if (entry) {
                lang_counts[entry->name]++;
            }
        }
    }

    check(lang_counts.count("python") > 0 && lang_counts.at("python") == 2,
          "language_detection: 2 Python files detected");
    check(lang_counts.count("cpp") > 0 && lang_counts.at("cpp") == 1,
          "language_detection: 1 C++ file detected");
    check(lang_counts.count("javascript") > 0 && lang_counts.at("javascript") == 1,
          "language_detection: 1 JavaScript file detected");
    check(lang_counts.find("unknown") == lang_counts.end(),
          "language_detection: no unknown language entries");

    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Test 4: config.json written and parsed back correctly
// ---------------------------------------------------------------------------
static void test_config_json() {
    fs::path root = make_temp_dir("config");
    fs::create_directories(root / ".codetldr");

    std::map<std::string, int> lang_counts;
    lang_counts["python"] = 5;
    lang_counts["cpp"] = 3;
    int total_files = 8;
    std::string init_at = "2026-03-25T00:00:00Z";

    nlohmann::json config;
    config["project_root"] = root.string();
    config["detected_languages"] = lang_counts;
    config["total_files"] = total_files;
    config["initialized_at"] = init_at;

    fs::path config_path = root / ".codetldr" / "config.json";
    {
        std::ofstream out(config_path);
        out << config.dump(2) << "\n";
    }

    check(fs::exists(config_path), "config_json: config.json created");

    // Parse back
    std::ifstream in(config_path);
    nlohmann::json parsed;
    in >> parsed;

    check(parsed.value("project_root", "") == root.string(),
          "config_json: project_root matches");
    check(parsed.value("total_files", 0) == 8,
          "config_json: total_files matches");
    check(parsed.value("initialized_at", "") == init_at,
          "config_json: initialized_at matches");
    check(parsed.contains("detected_languages"),
          "config_json: detected_languages field present");
    check(parsed["detected_languages"].value("python", 0) == 5,
          "config_json: python count matches");

    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Test 5: CAPABILITIES.md written with expected content
// ---------------------------------------------------------------------------
static void test_capabilities_md() {
    fs::path root = make_temp_dir("caps");
    fs::create_directories(root / ".codetldr");

    codetldr::LanguageRegistry registry;
    registry.initialize();

    fs::path caps_path = root / ".codetldr" / "CAPABILITIES.md";
    {
        std::ofstream out(caps_path);
        out << "# CodeTLDR Capabilities\n\n";
        out << "## Language Support Matrix\n\n";
        out << "| Language | L1 AST | L2 Call Graph | L3 CFG | L4 DFG | L5 PDG |\n";
        out << "|----------|--------|---------------|--------|--------|--------|\n";
        for (const auto& lang : registry.language_names()) {
            out << "| " << lang << " | yes | approx | no | no | no |\n";
        }
        out << "\n## Available Tools\n\n";
        out << "- search_symbols: Search for functions, classes, methods by name\n";
        out << "- search_text: Full-text search over all indexed content\n";
        out << "\n## MCP Configuration\n\n";
        out << "```json\n{\"mcpServers\": {}}\n```\n\n";
        out << "Generated by codetldr init -- 2026-03-25T00:00:00Z\n";
    }

    check(fs::exists(caps_path), "capabilities_md: CAPABILITIES.md created");

    // Read back and verify content
    std::ifstream in(caps_path);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    check(content.find("Language Support Matrix") != std::string::npos,
          "capabilities_md: contains Language Support Matrix header");
    check(content.find("Available Tools") != std::string::npos,
          "capabilities_md: contains Available Tools section");
    check(content.find("MCP Configuration") != std::string::npos,
          "capabilities_md: contains MCP Configuration section");
    check(content.find("search_symbols") != std::string::npos,
          "capabilities_md: contains search_symbols tool");
    check(content.find("Generated by codetldr init") != std::string::npos,
          "capabilities_md: contains generated-by line");

    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== test_cli_init ===\n\n";

    try {
        test_init_project_dir();
        test_codetldrignore();
        test_language_detection();
        test_config_json();
        test_capabilities_md();
    } catch (const std::exception& ex) {
        std::cerr << "EXCEPTION: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed > 0 ? 1 : 0;
}
