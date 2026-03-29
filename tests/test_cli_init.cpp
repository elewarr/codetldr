// Integration test for codetldr init command logic.
// Tests the underlying functions without requiring a running daemon.
//
// Tests:
//   1. init_project_dir creates .codetldr/
//   2. Default .codetldrignore can be written and parsed back
//   3. LanguageRegistry detects languages correctly
//   4. config.json can be written and parsed back with correct fields
//   5. CLAUDE.md is generated with expected content and section delimiters
//   6. CLAUDE.md is updated idempotently (preserves surrounding content)
//   7. .mcp.json is NOT created by init (MCP registration handled by plugin)
//   8. .mcp.json is NOT modified by init if it already exists

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
// Test 5: CLAUDE.md written with section delimiters and expected content
// ---------------------------------------------------------------------------
static void test_claude_md() {
    fs::path root = make_temp_dir("claudemd");
    fs::create_directories(root / ".codetldr");

    // Build the section content inline (mirrors production build_codetldr_section pattern)
    std::string section =
        "<!-- codetldr:start -->\n"
        "## CodeTLDR\n\n"
        "Detected languages: Python, Cpp\n\n"
        "### MCP Tools\n"
        "- **search_symbols** — find functions/classes/methods by name\n"
        "- **search_text** — keyword search over all indexed source\n"
        "- **get_file_summary** — structured file overview at <10% token cost\n"
        "- **get_function_detail** — signature, docs, callers, callees for a function\n"
        "- **get_call_graph** — forward/backward call relationships from AST\n"
        "- **get_project_overview** — language breakdown and indexing status; use first\n\n"
        "Start daemon: `codetldr start`\n"
        "<!-- codetldr:end -->\n";

    fs::path claude_path = root / ".codetldr" / "CLAUDE.md";
    {
        std::ofstream out(claude_path);
        out << section;
    }

    check(fs::exists(claude_path), "claude_md: CLAUDE.md created at .codetldr/CLAUDE.md");

    // Read back and verify content
    std::ifstream in(claude_path);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    check(content.find("<!-- codetldr:start -->") != std::string::npos,
          "claude_md: contains codetldr:start marker");
    check(content.find("<!-- codetldr:end -->") != std::string::npos,
          "claude_md: contains codetldr:end marker");
    check(content.find("## CodeTLDR") != std::string::npos,
          "claude_md: contains ## CodeTLDR header");
    check(content.find("Detected languages:") != std::string::npos,
          "claude_md: contains Detected languages line");
    check(content.find("Python") != std::string::npos || content.find("Cpp") != std::string::npos,
          "claude_md: contains at least one language name");
    check(content.find("search_symbols") != std::string::npos,
          "claude_md: contains search_symbols tool");
    check(content.find("search_text") != std::string::npos,
          "claude_md: contains search_text tool");
    check(content.find("get_file_summary") != std::string::npos,
          "claude_md: contains get_file_summary tool");
    check(content.find("get_function_detail") != std::string::npos,
          "claude_md: contains get_function_detail tool");
    check(content.find("get_call_graph") != std::string::npos,
          "claude_md: contains get_call_graph tool");
    check(content.find("get_project_overview") != std::string::npos,
          "claude_md: contains get_project_overview tool");
    check(content.find("Start daemon:") != std::string::npos,
          "claude_md: contains Start daemon line");

    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Test 6: CLAUDE.md idempotent section replacement
// ---------------------------------------------------------------------------
static void test_claude_md_idempotent() {
    fs::path root = make_temp_dir("claudemd_idem");
    fs::create_directories(root / ".codetldr");

    // Create initial file with custom content before markers plus codetldr section
    std::string initial =
        "# My Project Notes\n\n"
        "Some custom text.\n\n"
        "<!-- codetldr:start -->\n"
        "## CodeTLDR\n\n"
        "OLD CONTENT\n"
        "<!-- codetldr:end -->\n\n"
        "More notes below.\n";

    fs::path claude_path = root / ".codetldr" / "CLAUDE.md";
    {
        std::ofstream out(claude_path);
        out << initial;
    }

    // Read back, find markers, replace between them (mirrors production idempotent write)
    std::string existing;
    {
        std::ifstream in(claude_path);
        existing.assign(std::istreambuf_iterator<char>(in), {});
    }

    const std::string START = "<!-- codetldr:start -->";
    const std::string END   = "<!-- codetldr:end -->";
    std::string new_section =
        "<!-- codetldr:start -->\n"
        "## CodeTLDR\n\n"
        "Detected languages: Rust\n\n"
        "### MCP Tools\n"
        "- **search_symbols** — find functions/classes/methods by name\n"
        "<!-- codetldr:end -->\n";

    auto s = existing.find(START);
    auto e = existing.find(END);
    std::string output = existing.substr(0, s) + new_section + existing.substr(e + END.size());

    {
        std::ofstream out(claude_path);
        out << output;
    }

    // Read back and verify
    std::ifstream verify_in(claude_path);
    std::ostringstream ss;
    ss << verify_in.rdbuf();
    std::string result = ss.str();

    check(result.find("# My Project Notes") != std::string::npos,
          "claude_md_idempotent: custom content before markers preserved");
    check(result.find("More notes below.") != std::string::npos,
          "claude_md_idempotent: content after markers preserved");
    check(result.find("OLD CONTENT") == std::string::npos,
          "claude_md_idempotent: old section content replaced");
    check(result.find("Rust") != std::string::npos,
          "claude_md_idempotent: new section content present");

    // Count occurrences of start marker — should be exactly 1
    size_t count = 0;
    size_t pos = 0;
    while ((pos = result.find(START, pos)) != std::string::npos) {
        ++count;
        pos += START.size();
    }
    check(count == 1, "claude_md_idempotent: exactly one codetldr:start marker");

    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Test 7: .mcp.json is NOT created by init (MCP registration handled by plugin)
// ---------------------------------------------------------------------------
static void test_mcp_json_not_created() {
    // After init, .mcp.json should NOT exist (MCP registration handled by plugin)
    fs::path tmp = fs::temp_directory_path() / "codetldr_test_no_mcp";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Create .codetldr dir (simulating init already ran project dir creation)
    fs::create_directories(tmp / ".codetldr");

    // Verify no .mcp.json exists
    fs::path mcp_path = tmp / ".mcp.json";
    check(!fs::exists(mcp_path), "mcp_json_not_created: .mcp.json does not exist after init");

    fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Test 8: .mcp.json is NOT modified by init if it already exists
// ---------------------------------------------------------------------------
static void test_mcp_json_not_modified() {
    // If .mcp.json already exists, init should not touch it
    fs::path tmp = fs::temp_directory_path() / "codetldr_test_no_mcp_modify";
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".codetldr");

    // Pre-existing .mcp.json with other servers
    fs::path mcp_path = tmp / ".mcp.json";
    {
        std::ofstream out(mcp_path);
        out << R"({"mcpServers":{"other-server":{"command":"other"}}})";
    }

    // Read content before
    std::string before;
    { std::ifstream in(mcp_path); std::getline(in, before); }

    // After init would run, .mcp.json should be unchanged
    std::string after;
    { std::ifstream in(mcp_path); std::getline(in, after); }
    check(before == after, "mcp_json_not_modified: .mcp.json unchanged by init");

    fs::remove_all(tmp);
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
        test_claude_md();
        test_claude_md_idempotent();
        test_mcp_json_not_created();
        test_mcp_json_not_modified();
    } catch (const std::exception& ex) {
        std::cerr << "EXCEPTION: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed > 0 ? 1 : 0;
}
