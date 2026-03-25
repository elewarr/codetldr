#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
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

class ContextBuilder {
public:
    explicit ContextBuilder(SQLite::Database& db);
    ContextResponse build(const ContextRequest& req);

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

    std::vector<std::string> get_callee_names(int64_t symbol_id);
    std::vector<std::string> get_caller_names(int64_t symbol_id);

    SQLite::Database& db_;
};

} // namespace codetldr
