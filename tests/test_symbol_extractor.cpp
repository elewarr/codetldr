// test_symbol_extractor.cpp -- Symbol and call extraction tests across all 10 languages.
// No test framework: returns 0 on pass, 1 on failure.

#include "analysis/tree_sitter/extractor.h"
#include "analysis/tree_sitter/language_registry.h"
#include "analysis/tree_sitter/parser.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;
using namespace codetldr;

// Helper: read a file fully into a string
static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) {
        std::cerr << "ERROR: cannot open " << p << "\n";
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Helper: find symbol by name
static const Symbol* find_symbol(const std::vector<Symbol>& syms, const std::string& name) {
    for (const auto& s : syms) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// Helper: find call by callee name
static bool has_call(const std::vector<CallEdge>& calls, const std::string& callee) {
    for (const auto& c : calls) {
        if (c.callee_name == callee) return true;
    }
    return false;
}

// Helper: count calls with caller_name
static int count_calls_with_caller(const std::vector<CallEdge>& calls, const std::string& caller) {
    int n = 0;
    for (const auto& c : calls) {
        if (c.caller_name == caller) n++;
    }
    return n;
}

static fs::path fixture_dir() {
    return fs::path(__FILE__).parent_path() / "fixtures";
}

// ============================================================
// test_python_symbols
// ============================================================
static bool test_python_symbols(const LanguageRegistry& reg) {
    std::string test_name = "test_python_symbols";
    auto entry = reg.for_extension(".py");
    if (!entry) {
        std::cerr << "[FAIL] " << test_name << ": no .py entry in registry\n";
        return false;
    }

    auto path = fixture_dir() / "sample.py";
    auto source = read_file(path);
    if (source.empty()) {
        std::cerr << "[FAIL] " << test_name << ": empty source\n";
        return false;
    }

    auto tree = parse_source(entry->language, source);
    if (!tree) {
        std::cerr << "[FAIL] " << test_name << ": parse failed\n";
        return false;
    }

    auto syms = extract_symbols(tree.get(), entry->symbol_query.get(), source);

    // Expect at least 6 symbols
    if (syms.size() < 6) {
        std::cerr << "[FAIL] " << test_name << ": expected >= 6 symbols, got " << syms.size() << "\n";
        for (const auto& s : syms) {
            std::cerr << "  symbol: " << s.name << " kind=" << s.kind
                      << " lines=" << s.line_start << "-" << s.line_end << "\n";
        }
        return false;
    }

    // Check for specific symbols
    struct Expected { std::string name; std::string kind; };
    std::vector<Expected> expected = {
        {"greet",      "function"},
        {"farewell",   "function"},
        {"Calculator", "class"},
        {"add",        "method"},
        {"multiply",   "method"},
        {"main",       "function"},
    };

    for (const auto& ex : expected) {
        const Symbol* s = find_symbol(syms, ex.name);
        if (!s) {
            std::cerr << "[FAIL] " << test_name << ": symbol '" << ex.name << "' not found\n";
            return false;
        }
        if (s->kind != ex.kind) {
            std::cerr << "[FAIL] " << test_name << ": symbol '" << ex.name
                      << "' expected kind=" << ex.kind << " got=" << s->kind << "\n";
            return false;
        }
        if (s->line_start <= 0 || s->line_end < s->line_start) {
            std::cerr << "[FAIL] " << test_name << ": symbol '" << ex.name
                      << "' has invalid line range " << s->line_start << "-" << s->line_end << "\n";
            return false;
        }
    }

    // Check greet has documentation (docstring)
    const Symbol* greet = find_symbol(syms, "greet");
    if (!greet || greet->documentation.empty()) {
        std::cerr << "[FAIL] " << test_name << ": greet has no documentation\n";
        return false;
    }
    if (greet->documentation.find("greeting") == std::string::npos) {
        std::cerr << "[FAIL] " << test_name << ": greet documentation does not contain 'greeting': "
                  << greet->documentation << "\n";
        return false;
    }

    std::cout << "[PASS] " << test_name << " (" << syms.size() << " symbols)\n";
    return true;
}

// ============================================================
// test_python_calls
// ============================================================
static bool test_python_calls(const LanguageRegistry& reg) {
    std::string test_name = "test_python_calls";
    auto entry = reg.for_extension(".py");
    if (!entry) { std::cerr << "[FAIL] " << test_name << ": no .py entry\n"; return false; }

    auto path = fixture_dir() / "sample.py";
    auto source = read_file(path);
    auto tree = parse_source(entry->language, source);
    if (!tree) { std::cerr << "[FAIL] " << test_name << ": parse failed\n"; return false; }

    auto syms  = extract_symbols(tree.get(), entry->symbol_query.get(), source);
    auto calls = extract_calls(tree.get(), entry->call_query.get(), source, syms);

    if (calls.size() < 2) {
        std::cerr << "[FAIL] " << test_name << ": expected >= 2 calls, got " << calls.size() << "\n";
        return false;
    }

    // Calls to greet and farewell should be present
    if (!has_call(calls, "greet")) {
        std::cerr << "[FAIL] " << test_name << ": no call to 'greet'\n";
        return false;
    }
    if (!has_call(calls, "farewell")) {
        std::cerr << "[FAIL] " << test_name << ": no call to 'farewell'\n";
        return false;
    }

    // Both should have caller_name "main"
    bool greet_from_main = false;
    bool farewell_from_main = false;
    for (const auto& c : calls) {
        if (c.callee_name == "greet"   && c.caller_name == "main") greet_from_main = true;
        if (c.callee_name == "farewell" && c.caller_name == "main") farewell_from_main = true;
    }
    if (!greet_from_main) {
        std::cerr << "[FAIL] " << test_name << ": call to 'greet' not attributed to 'main'\n";
        for (const auto& c : calls) {
            std::cerr << "  call: " << c.caller_name << " -> " << c.callee_name << " line=" << c.line << "\n";
        }
        return false;
    }
    if (!farewell_from_main) {
        std::cerr << "[FAIL] " << test_name << ": call to 'farewell' not attributed to 'main'\n";
        return false;
    }

    std::cout << "[PASS] " << test_name << " (" << calls.size() << " calls)\n";
    return true;
}

// ============================================================
// test_all_languages_extract_symbols
// ============================================================
static bool test_all_languages_extract_symbols(const LanguageRegistry& reg) {
    std::string test_name = "test_all_languages_extract_symbols";
    bool ok = true;

    struct Fixture { std::string ext; std::string file; };
    std::vector<Fixture> fixtures = {
        {".py",    "sample.py"},
        {".js",    "sample.js"},
        {".ts",    "sample.ts"},
        {".rs",    "sample.rs"},
        {".c",     "sample.c"},
        {".cpp",   "sample.cpp"},
        {".java",  "sample.java"},
        {".kt",    "sample.kt"},
        {".swift", "sample.swift"},
        {".m",     "sample.m"},
    };

    for (const auto& fix : fixtures) {
        auto entry = reg.for_extension(fix.ext);
        if (!entry) {
            std::cerr << "[FAIL] " << test_name << ": no entry for " << fix.ext << "\n";
            ok = false;
            continue;
        }

        auto path = fixture_dir() / fix.file;
        auto source = read_file(path);
        if (source.empty()) {
            std::cerr << "[FAIL] " << test_name << ": empty source for " << fix.file << "\n";
            ok = false;
            continue;
        }

        auto tree = parse_source(entry->language, source);
        if (!tree) {
            std::cerr << "[FAIL] " << test_name << ": parse failed for " << fix.file << "\n";
            ok = false;
            continue;
        }

        auto syms = extract_symbols(tree.get(), entry->symbol_query.get(), source);
        std::cout << "  " << entry->name << ": " << syms.size() << " symbols\n";

        if (syms.size() < 3) {
            std::cerr << "[FAIL] " << test_name << ": " << fix.file
                      << " expected >= 3 symbols, got " << syms.size() << "\n";
            ok = false;
        }
    }

    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// test_all_languages_extract_calls
// ============================================================
static bool test_all_languages_extract_calls(const LanguageRegistry& reg) {
    std::string test_name = "test_all_languages_extract_calls";
    bool ok = true;

    struct Fixture { std::string ext; std::string file; };
    std::vector<Fixture> fixtures = {
        {".py",    "sample.py"},
        {".js",    "sample.js"},
        {".ts",    "sample.ts"},
        {".rs",    "sample.rs"},
        {".c",     "sample.c"},
        {".cpp",   "sample.cpp"},
        {".java",  "sample.java"},
        {".kt",    "sample.kt"},
        {".swift", "sample.swift"},
        {".m",     "sample.m"},
    };

    for (const auto& fix : fixtures) {
        auto entry = reg.for_extension(fix.ext);
        if (!entry) {
            std::cerr << "[FAIL] " << test_name << ": no entry for " << fix.ext << "\n";
            ok = false;
            continue;
        }

        auto path = fixture_dir() / fix.file;
        auto source = read_file(path);
        auto tree = parse_source(entry->language, source);
        if (!tree) {
            std::cerr << "[FAIL] " << test_name << ": parse failed for " << fix.file << "\n";
            ok = false;
            continue;
        }

        auto syms  = extract_symbols(tree.get(), entry->symbol_query.get(), source);
        auto calls = extract_calls(tree.get(), entry->call_query.get(), source, syms);
        std::cout << "  " << entry->name << ": " << calls.size() << " calls\n";

        if (calls.size() < 2) {
            std::cerr << "[FAIL] " << test_name << ": " << fix.file
                      << " expected >= 2 calls, got " << calls.size() << "\n";
            ok = false;
        }
    }

    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// test_symbol_kinds_correct
// ============================================================
static bool test_symbol_kinds_correct(const LanguageRegistry& reg) {
    std::string test_name = "test_symbol_kinds_correct";

    auto entry = reg.for_extension(".py");
    if (!entry) { std::cerr << "[FAIL] " << test_name << ": no .py entry\n"; return false; }

    auto path = fixture_dir() / "sample.py";
    auto source = read_file(path);
    auto tree = parse_source(entry->language, source);
    if (!tree) { std::cerr << "[FAIL] " << test_name << ": parse failed\n"; return false; }

    auto syms = extract_symbols(tree.get(), entry->symbol_query.get(), source);

    // Validate all symbol kinds are from the allowed set
    std::vector<std::string> valid_kinds = {
        "function", "method", "class", "struct", "enum", "interface", "import", "export", "constant"
    };

    bool ok = true;
    for (const auto& s : syms) {
        bool valid = false;
        for (const auto& k : valid_kinds) {
            if (s.kind == k) { valid = true; break; }
        }
        if (!valid) {
            std::cerr << "[FAIL] " << test_name << ": symbol '" << s.name
                      << "' has invalid kind '" << s.kind << "'\n";
            ok = false;
        }
    }

    // Spot-check specific kinds
    const Symbol* greet = find_symbol(syms, "greet");
    if (greet && greet->kind != "function") {
        std::cerr << "[FAIL] " << test_name << ": greet expected kind=function, got=" << greet->kind << "\n";
        ok = false;
    }
    const Symbol* calc = find_symbol(syms, "Calculator");
    if (calc && calc->kind != "class") {
        std::cerr << "[FAIL] " << test_name << ": Calculator expected kind=class, got=" << calc->kind << "\n";
        ok = false;
    }
    const Symbol* add = find_symbol(syms, "add");
    if (add && add->kind != "method") {
        std::cerr << "[FAIL] " << test_name << ": add expected kind=method, got=" << add->kind << "\n";
        ok = false;
    }

    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// main
// ============================================================
int main() {
    LanguageRegistry reg;
    if (!reg.initialize()) {
        std::cerr << "ERROR: LanguageRegistry::initialize() returned false\n";
        return 1;
    }

    bool all_pass = true;
    all_pass &= test_python_symbols(reg);
    all_pass &= test_python_calls(reg);
    all_pass &= test_all_languages_extract_symbols(reg);
    all_pass &= test_all_languages_extract_calls(reg);
    all_pass &= test_symbol_kinds_correct(reg);

    if (all_pass) {
        std::cout << "\nAll tests PASSED\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED\n";
        return 1;
    }
}
