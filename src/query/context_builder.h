#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace codetldr {

enum class ContextFormat { kCondensed, kDetailed, kDiffAware };

struct ContextRequest {
    ContextFormat format = ContextFormat::kCondensed;
    std::vector<std::string> file_paths;    // empty = all files (capped by max_symbols)
    std::vector<std::string> changed_paths; // for kDiffAware
    int max_symbols = 200;                  // safety cap
};

struct ContextResponse {
    std::string text;
    int symbol_count = 0;
    int file_count = 0;
    int estimated_tokens = 0;  // text.size() / 4
};

/// Detailed info about a single symbol (for get_function_detail RPC).
struct SymbolInfo {
    int64_t id = 0;
    std::string name;
    std::string kind;
    std::string signature;
    std::string documentation;
    std::string file_path;
    int line_start = 0;
    int line_end = 0;
    bool found = false;
};

class ContextBuilder {
public:
    explicit ContextBuilder(SQLite::Database& db);
    ContextResponse build(const ContextRequest& req);

    /// Look up a symbol by name, optionally filtered by file_path.
    /// Returns SymbolInfo with found=false if no match.
    SymbolInfo find_symbol(const std::string& name,
                           const std::string& file_path = "");

    /// Return names of all functions/methods called by symbol_id.
    std::vector<std::string> get_callee_names(int64_t symbol_id);

    /// Return names of all callers of symbol_id.
    std::vector<std::string> get_caller_names(int64_t symbol_id);

    /// Return CFG nodes for the given symbol_id, ordered by line ascending.
    /// Returns empty JSON array if symbol has no CFG data (unsupported language).
    nlohmann::json get_control_flow(int64_t symbol_id);

    /// Return DFG edges for the given symbol_id, ordered by line ascending.
    /// Returns empty JSON array if symbol has no DFG data (unsupported language).
    nlohmann::json get_data_flow(int64_t symbol_id);

private:
    struct SymbolRow {
        int64_t id;
        std::string name, kind, signature, documentation;
        int line_start, line_end;
    };

    std::string format_condensed(const std::string& file_path,
                                 const std::vector<SymbolRow>& symbols);
    std::string format_detailed(const std::string& file_path,
                                const std::vector<SymbolRow>& symbols);

    SQLite::Database& db_;
};

} // namespace codetldr
