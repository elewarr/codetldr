#include "analysis/tree_sitter/language_registry.h"
#include "analysis/tree_sitter/parser.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace fs = std::filesystem;

static void assert_true(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "ASSERTION FAILED: " << msg << "\n";
        std::exit(1);
    }
}

// Compute project root from __FILE__ (this source file is at tests/)
static fs::path project_root() {
    fs::path this_file = __FILE__;
    // __FILE__ is tests/test_tree_sitter_parser.cpp relative to project root
    // resolved to absolute at compile time on most compilers
    return this_file.parent_path().parent_path();
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    assert_true(f.is_open(), "Could not open fixture: " + p.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ----------------------------------------------------------------
// test_registry_initializes
// ----------------------------------------------------------------
static void test_registry_initializes() {
    codetldr::LanguageRegistry reg;
    bool ok = reg.initialize();
    assert_true(ok, "LanguageRegistry::initialize() should return true for all languages");
    std::cout << "PASS: test_registry_initializes\n";
}

// ----------------------------------------------------------------
// test_all_extensions_resolve
// ----------------------------------------------------------------
static void test_all_extensions_resolve() {
    codetldr::LanguageRegistry reg;
    reg.initialize();

    const std::vector<std::string> extensions = {
        ".py", ".pyi",
        ".js", ".mjs", ".cjs",
        ".ts", ".tsx",
        ".rs",
        ".go",
        ".c", ".h",
        ".cpp", ".cc", ".cxx", ".hpp", ".hxx",
        ".java",
        ".kt", ".kts",
        ".swift",
        ".m", ".mm",
        ".rb", ".rake", ".gemspec", ".ru",
        ".lua"
    };

    for (const auto& ext : extensions) {
        const auto* entry = reg.for_extension(ext);
        assert_true(entry != nullptr,
            "for_extension(\"" + ext + "\") should return non-null LanguageEntry");
        assert_true(entry->language != nullptr,
            "for_extension(\"" + ext + "\") entry->language should be non-null");
    }
    std::cout << "PASS: test_all_extensions_resolve (" << extensions.size() << " extensions)\n";
}

// ----------------------------------------------------------------
// test_unknown_extension_returns_null
// ----------------------------------------------------------------
static void test_unknown_extension_returns_null() {
    codetldr::LanguageRegistry reg;
    reg.initialize();

    assert_true(reg.for_extension(".xyz") == nullptr,
        "for_extension(\".xyz\") should return nullptr");
    assert_true(reg.for_extension("") == nullptr,
        "for_extension(\"\") should return nullptr");
    assert_true(reg.for_extension(".zig") == nullptr,
        "for_extension(\".zig\") should return nullptr");
    std::cout << "PASS: test_unknown_extension_returns_null\n";
}

// ----------------------------------------------------------------
// test_parse_each_language
// ----------------------------------------------------------------
static void test_parse_each_language() {
    codetldr::LanguageRegistry reg;
    reg.initialize();

    fs::path fixtures = project_root() / "tests" / "fixtures";

    // Map fixture file extension to its file
    struct Fixture { std::string ext; std::string filename; };
    const std::vector<Fixture> fixtures_list = {
        {".py",    "sample.py"},
        {".js",    "sample.js"},
        {".ts",    "sample.ts"},
        {".tsx",   "sample.tsx"},
        {".rs",    "sample.rs"},
        {".go",    "sample.go"},
        {".c",     "sample.c"},
        {".cpp",   "sample.cpp"},
        {".java",  "sample.java"},
        {".kt",    "sample.kt"},
        {".swift", "sample.swift"},
        {".m",     "sample.m"},
        {".rb",    "sample.rb"},
        {".lua",   "sample.lua"},
    };

    for (const auto& fx : fixtures_list) {
        fs::path fp = fixtures / fx.filename;
        std::string content = read_file(fp);

        const auto* entry = reg.for_extension(fx.ext);
        assert_true(entry != nullptr,
            "No language entry for extension: " + fx.ext);

        codetldr::TsTreePtr tree = codetldr::parse_source(entry->language, content);
        assert_true(tree != nullptr,
            "parse_source returned null tree for " + fx.filename);

        TSNode root = ts_tree_root_node(tree.get());
        uint32_t child_count = ts_node_child_count(root);
        assert_true(child_count > 0,
            "Root node has 0 children for " + fx.filename);

        std::cout << "PASS: parsed " << fx.filename
                  << " (root children: " << child_count << ")\n";
    }
}

// ----------------------------------------------------------------
// test_query_compilation_all_languages
// ----------------------------------------------------------------
static void test_query_compilation_all_languages() {
    codetldr::LanguageRegistry reg;
    reg.initialize();

    for (const auto& name : reg.language_names()) {
        // Find entry by name by looking through all extensions
        // (use a known extension for each language)
        const std::vector<std::pair<std::string, std::string>> lang_ext = {
            {"python",     ".py"},
            {"javascript", ".js"},
            {"typescript", ".ts"},
            {"tsx",        ".tsx"},
            {"rust",       ".rs"},
            {"go",         ".go"},
            {"c",          ".c"},
            {"cpp",        ".cpp"},
            {"java",       ".java"},
            {"kotlin",     ".kt"},
            {"swift",      ".swift"},
            {"objc",       ".m"},
            {"ruby",       ".rb"},
            {"lua",        ".lua"},
        };
        for (const auto& [lang, ext] : lang_ext) {
            const auto* entry = reg.for_extension(ext);
            if (!entry) continue;
            assert_true(entry->symbol_query != nullptr,
                "symbol_query is null for language: " + entry->name);
            assert_true(entry->call_query != nullptr,
                "call_query is null for language: " + entry->name);
        }
    }
    std::cout << "PASS: test_query_compilation_all_languages\n";
}

// ----------------------------------------------------------------
// main
// ----------------------------------------------------------------
int main() {
    test_registry_initializes();
    test_all_extensions_resolve();
    test_unknown_extension_returns_null();
    test_parse_each_language();
    test_query_compilation_all_languages();

    std::cout << "All tree_sitter_parser tests PASSED\n";
    return 0;
}
