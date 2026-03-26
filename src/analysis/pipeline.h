#pragma once
#include "analysis/tree_sitter/extractor.h"
#include "analysis/tree_sitter/language_registry.h"
#include <filesystem>
#include <string>

// Forward declare SQLite::Database to avoid pulling in SQLiteCpp here
namespace SQLite { class Database; }

namespace codetldr {

struct AnalysisResult {
    int symbols_count;
    int calls_count;
    int cfg_count;      // number of CFG nodes extracted
    int dfg_count;      // number of DFG edges extracted
    bool success;
    std::string error;  // empty if success
};

// Analyze a single source file: parse with Tree-sitter, extract symbols and calls,
// persist to SQLite. Uses upsert semantics -- existing symbols/calls for this file
// are deleted and re-inserted inside a transaction.
//
// The file is inserted into the `files` table if not already present, or updated.
AnalysisResult analyze_file(
    SQLite::Database& db,
    const LanguageRegistry& registry,
    const std::filesystem::path& file_path);

} // namespace codetldr
