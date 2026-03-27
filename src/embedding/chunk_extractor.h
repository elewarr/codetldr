#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SQLite { class Database; }

namespace codetldr {

struct Chunk {
    int64_t symbol_id;      // symbols.id -- for vector persistence in Phase 16+
    std::string file_path;  // relative to project root (from files.path)
    std::string kind;       // "function", "method", etc.
    std::string name;       // symbol name
    std::string signature;  // may be empty
    int line_start;
    int line_end;
    std::string text;       // context_header + "\n\n" + body (CHK-02)
};

// Format: "// {file_path}\n// {kind}: {name}{signature}\n\n"
std::string make_context_header(const std::string& file_path,
                                 const std::string& kind,
                                 const std::string& name,
                                 const std::string& signature);

// Extract function/method-level chunks from the symbols table.
// Groups by file to avoid reading the same file multiple times.
// file_id >= 0: extract only chunks for that file.
// file_id < 0 (default -1): extract all files.
std::vector<Chunk> extract_chunks(
    SQLite::Database& db,
    const std::filesystem::path& project_root,
    int64_t file_id = -1);

} // namespace codetldr
