// test_context_builder.cpp -- ContextBuilder unit tests.
// Tests all 7 behaviors: condensed format, token ratio, detailed format,
// diff-aware filter, max_symbols cap, response metadata, and empty file handling.
// Uses assert() + return 0/1 pattern consistent with existing tests.

#include "storage/database.h"
#include "query/context_builder.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace codetldr;

// Insert a file row and return its file_id
static int64_t insert_file(SQLite::Database& db, const std::string& path) {
    SQLite::Statement ins(db,
        "INSERT INTO files(path, language, mtime_ns) VALUES(?, 'cpp', 0)");
    ins.bind(1, path);
    ins.exec();
    return db.getLastInsertRowid();
}

// Insert a symbol row and return its symbol_id
static int64_t insert_symbol(SQLite::Database& db, int64_t file_id,
                              const std::string& kind, const std::string& name,
                              const std::string& sig, const std::string& doc,
                              int line_start, int line_end) {
    SQLite::Statement ins(db,
        "INSERT INTO symbols(file_id, kind, name, signature, documentation, line_start, line_end) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, file_id);
    ins.bind(2, kind);
    ins.bind(3, name);
    ins.bind(4, sig);
    ins.bind(5, doc);
    ins.bind(6, line_start);
    ins.bind(7, line_end);
    ins.exec();
    return db.getLastInsertRowid();
}

// Insert a call edge
static void insert_call(SQLite::Database& db, int64_t caller_id, int64_t callee_id,
                         const std::string& callee_name, int64_t file_id, int line) {
    SQLite::Statement ins(db,
        "INSERT INTO calls(caller_id, callee_id, callee_name, file_id, line) "
        "VALUES (?, ?, ?, ?, ?)");
    ins.bind(1, caller_id);
    ins.bind(2, callee_id);
    ins.bind(3, callee_name);
    ins.bind(4, file_id);
    ins.bind(5, line);
    ins.exec();
}

int main() {
    const fs::path test_dir = fs::temp_directory_path() / "codetldr_test_context_builder";
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    const fs::path db_path = test_dir / "index.sqlite";
    auto db_obj = Database::open(db_path);
    SQLite::Database& db = db_obj.raw();

    // Set up test data:
    //   File A: /src/a.cpp - 3 symbols (function, class, method)
    //   File B: /src/b.cpp - 2 symbols (function, class)
    int64_t file_a = insert_file(db, "/src/a.cpp");
    int64_t file_b = insert_file(db, "/src/b.cpp");

    int64_t sym_a1 = insert_symbol(db, file_a, "function", "compute",
                                   "int compute(int x, int y)",
                                   "Computes the sum of x and y.", 1, 10);
    int64_t sym_a2 = insert_symbol(db, file_a, "class", "Calculator",
                                   "class Calculator",
                                   "A simple calculator class.", 12, 50);
    int64_t sym_a3 = insert_symbol(db, file_a, "method", "add",
                                   "int add(int a, int b)",
                                   "Adds two numbers.", 20, 30);

    int64_t sym_b1 = insert_symbol(db, file_b, "function", "process",
                                   "void process(std::vector<int>& data)",
                                   "Processes the data vector.", 1, 25);
    int64_t sym_b2 = insert_symbol(db, file_b, "class", "DataProcessor",
                                   "class DataProcessor",
                                   "Handles data processing pipeline.", 27, 80);

    // Insert call edges: compute calls add; add called by compute
    insert_call(db, sym_a1, sym_a3, "add", file_a, 5);

    // Build a "raw source" string of ~50 lines to compare token counts against condensed output
    // This represents the actual source that would contain the 3 symbols in file_a
    std::string raw_source;
    raw_source += "// calculator.cpp - full source for 3 symbols\n";
    raw_source += "#include <iostream>\n";
    raw_source += "#include <vector>\n";
    raw_source += "#include <string>\n";
    raw_source += "\n";
    raw_source += "// Computes the sum of x and y.\n";
    raw_source += "// This is a detailed implementation comment that explains\n";
    raw_source += "// all the edge cases and behavior of the function.\n";
    raw_source += "int compute(int x, int y) {\n";
    raw_source += "    // validate inputs\n";
    raw_source += "    if (x < 0 || y < 0) {\n";
    raw_source += "        throw std::invalid_argument(\"negative input\");\n";
    raw_source += "    }\n";
    raw_source += "    return add(x, y);\n";
    raw_source += "}\n";
    raw_source += "\n";
    raw_source += "// A simple calculator class.\n";
    raw_source += "// Provides basic arithmetic operations.\n";
    raw_source += "class Calculator {\n";
    raw_source += "public:\n";
    raw_source += "    Calculator() = default;\n";
    raw_source += "    ~Calculator() = default;\n";
    raw_source += "\n";
    raw_source += "    // Adds two numbers.\n";
    raw_source += "    // This method validates inputs and returns the sum.\n";
    raw_source += "    int add(int a, int b) {\n";
    raw_source += "        // check for overflow\n";
    raw_source += "        if (a > 0 && b > INT_MAX - a) {\n";
    raw_source += "            throw std::overflow_error(\"integer overflow\");\n";
    raw_source += "        }\n";
    raw_source += "        return a + b;\n";
    raw_source += "    }\n";
    raw_source += "\n";
    raw_source += "private:\n";
    raw_source += "    int history_[100] = {};\n";
    raw_source += "    int history_count_ = 0;\n";
    raw_source += "    bool debug_mode_ = false;\n";
    raw_source += "    std::string name_ = \"default\";\n";
    raw_source += "    std::vector<int> cache_;\n";
    raw_source += "    void reset_history() { history_count_ = 0; }\n";
    raw_source += "    void clear_cache() { cache_.clear(); }\n";
    raw_source += "    bool has_history() { return history_count_ > 0; }\n";
    raw_source += "    void debug_log(const std::string& msg) { if (debug_mode_) std::cout << msg; }\n";
    raw_source += "};\n";
    raw_source += "\n";
    raw_source += "// End of file\n";
    raw_source += "// Total: approximately 50 lines\n";
    raw_source += "// This padding ensures we have enough raw source to test token ratio\n";
    raw_source += "// The condensed output should be much smaller than this raw source.\n";
    // Ensure ~50+ lines total
    for (int i = 0; i < 15; ++i) {
        raw_source += "// padding line " + std::to_string(i) + " to reach 50+ line count\n";
    }

    int raw_tokens = static_cast<int>(raw_source.size()) / 4;

    ContextBuilder builder(db);

    // Test 1: Condensed format for file_a with 3 symbols outputs [FILE] header + one line per symbol
    {
        ContextRequest req;
        req.format = ContextFormat::kCondensed;
        req.file_paths = {"/src/a.cpp"};

        ContextResponse resp = builder.build(req);

        // Should have [FILE] header
        assert(resp.text.find("[FILE] /src/a.cpp") != std::string::npos);
        // Should mention 3 symbols
        assert(resp.text.find("3 symbols") != std::string::npos);
        // Should have one line per symbol: compute, Calculator, add
        assert(resp.text.find("compute") != std::string::npos);
        assert(resp.text.find("Calculator") != std::string::npos);
        assert(resp.text.find("add") != std::string::npos);
        std::cout << "PASS: Test 1 - condensed format has [FILE] header + symbol lines\n";
    }

    // Test 2: Condensed output token count is less than 10% of raw source token count
    {
        ContextRequest req;
        req.format = ContextFormat::kCondensed;
        req.file_paths = {"/src/a.cpp"};

        ContextResponse resp = builder.build(req);
        int condensed_tokens = static_cast<int>(resp.text.size()) / 4;

        std::cout << "  raw_tokens=" << raw_tokens << " condensed_tokens=" << condensed_tokens
                  << " ratio=" << (condensed_tokens * 100.0 / raw_tokens) << "%\n";
        assert(condensed_tokens < raw_tokens / 10);
        std::cout << "PASS: Test 2 - condensed token count < 10% of raw source\n";
    }

    // Test 3: Detailed format includes signature, documentation, calls, called_by, and line range
    {
        ContextRequest req;
        req.format = ContextFormat::kDetailed;
        req.file_paths = {"/src/a.cpp"};

        ContextResponse resp = builder.build(req);

        // Should have [FILE] header
        assert(resp.text.find("[FILE] /src/a.cpp") != std::string::npos);
        // Should have signature
        assert(resp.text.find("signature:") != std::string::npos);
        // Should have doc
        assert(resp.text.find("doc:") != std::string::npos);
        // Should have calls section
        assert(resp.text.find("calls:") != std::string::npos);
        // Should have called_by section
        assert(resp.text.find("called_by:") != std::string::npos);
        // Should have line range
        assert(resp.text.find("lines:") != std::string::npos);
        std::cout << "PASS: Test 3 - detailed format includes signature, doc, calls, called_by, lines\n";
    }

    // Test 4: Diff-aware format with changed_paths=["a.cpp"] only includes symbols from a.cpp
    {
        ContextRequest req;
        req.format = ContextFormat::kDiffAware;
        req.changed_paths = {"/src/a.cpp"};

        ContextResponse resp = builder.build(req);

        // Should have a.cpp symbols
        assert(resp.text.find("/src/a.cpp") != std::string::npos);
        // Should NOT have b.cpp symbols
        assert(resp.text.find("/src/b.cpp") == std::string::npos);
        assert(resp.text.find("process") == std::string::npos);
        assert(resp.text.find("DataProcessor") == std::string::npos);
        std::cout << "PASS: Test 4 - diff-aware format only includes symbols from changed paths\n";
    }

    // Test 5: Empty file_paths with max_symbols cap returns at most max_symbols entries
    {
        ContextRequest req;
        req.format = ContextFormat::kCondensed;
        req.file_paths = {};   // all files
        req.max_symbols = 2;   // cap at 2

        ContextResponse resp = builder.build(req);
        assert(resp.symbol_count <= 2);
        std::cout << "PASS: Test 5 - max_symbols cap limits results (got " << resp.symbol_count << ")\n";
    }

    // Test 6: ContextResponse includes correct symbol_count, file_count, and estimated_tokens
    {
        ContextRequest req;
        req.format = ContextFormat::kCondensed;
        req.file_paths = {"/src/a.cpp", "/src/b.cpp"};

        ContextResponse resp = builder.build(req);

        assert(resp.symbol_count == 5);  // 3 from a.cpp + 2 from b.cpp
        assert(resp.file_count == 2);
        assert(resp.estimated_tokens == static_cast<int>(resp.text.size()) / 4);
        std::cout << "PASS: Test 6 - response metadata: symbol_count=" << resp.symbol_count
                  << " file_count=" << resp.file_count
                  << " estimated_tokens=" << resp.estimated_tokens << "\n";
    }

    // Test 7: File with no symbols produces "[FILE] path (0 symbols)" with no symbol lines
    {
        // Insert a file with no symbols
        int64_t file_empty = insert_file(db, "/src/empty.h");
        (void)file_empty;

        ContextRequest req;
        req.format = ContextFormat::kCondensed;
        req.file_paths = {"/src/empty.h"};

        ContextResponse resp = builder.build(req);
        assert(resp.text.find("[FILE] /src/empty.h") != std::string::npos);
        assert(resp.text.find("0 symbols") != std::string::npos);
        assert(resp.symbol_count == 0);
        std::cout << "PASS: Test 7 - empty file produces (0 symbols) header\n";
    }

    fs::remove_all(test_dir);
    std::cout << "All context_builder tests passed.\n";
    return 0;
}
