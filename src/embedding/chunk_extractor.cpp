#include "embedding/chunk_extractor.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace codetldr {

// ---------------------------------------------------------------------------
// make_context_header
// Format: "// {file_path}\n// {kind}: {name}{signature}\n\n"
// If signature is empty, omit it (no trailing space).
// ---------------------------------------------------------------------------
std::string make_context_header(const std::string& file_path,
                                 const std::string& kind,
                                 const std::string& name,
                                 const std::string& signature)
{
    std::string header = "// " + file_path + "\n// " + kind + ": " + name;
    if (!signature.empty()) {
        header += signature;
    }
    header += "\n\n";
    return header;
}

// ---------------------------------------------------------------------------
// Helper: read file into lines
// ---------------------------------------------------------------------------
static std::vector<std::string> read_lines(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// extract_chunks
// ---------------------------------------------------------------------------
std::vector<Chunk> extract_chunks(
    SQLite::Database& db,
    const std::filesystem::path& project_root,
    int64_t file_id)
{
    // Build SQL query — filter by file_id if specified
    std::string sql =
        "SELECT s.id, f.path, s.kind, s.name, COALESCE(s.signature,''), "
        "       s.line_start, s.line_end "
        "FROM symbols s "
        "JOIN files f ON f.id = s.file_id "
        "WHERE s.kind IN ('function','method') ";

    if (file_id >= 0) {
        sql += "AND s.file_id = ? ";
    }
    sql += "ORDER BY f.path, s.line_start";

    SQLite::Statement stmt(db, sql);
    if (file_id >= 0) {
        stmt.bind(1, static_cast<long long>(file_id));
    }

    // Collect results grouped by file path for efficient file reading
    // key: relative file path, value: list of symbol rows
    struct SymbolRow {
        int64_t id;
        std::string rel_path;
        std::string kind;
        std::string name;
        std::string signature;
        int line_start;
        int line_end;
    };

    // Use ordered processing — since ORDER BY f.path, s.line_start,
    // consecutive rows with the same f.path can share a single file read
    std::vector<SymbolRow> rows;
    while (stmt.executeStep()) {
        SymbolRow row;
        row.id         = stmt.getColumn(0).getInt64();
        row.rel_path   = stmt.getColumn(1).getText();
        row.kind       = stmt.getColumn(2).getText();
        row.name       = stmt.getColumn(3).getText();
        row.signature  = stmt.getColumn(4).getText();
        row.line_start = stmt.getColumn(5).getInt();
        row.line_end   = stmt.getColumn(6).getInt();
        rows.push_back(std::move(row));
    }

    std::vector<Chunk> chunks;
    chunks.reserve(rows.size());

    // Cache file lines per path to avoid re-reading the same file
    std::unordered_map<std::string, std::vector<std::string>> file_cache;

    for (const auto& row : rows) {
        // Skip invalid line ranges
        if (row.line_start > row.line_end || row.line_start < 1) {
            spdlog::warn("extract_chunks: symbol '{}' has invalid line range [{}, {}], skipping",
                         row.name, row.line_start, row.line_end);
            continue;
        }

        // Get or load file lines
        auto it = file_cache.find(row.rel_path);
        if (it == file_cache.end()) {
            const std::filesystem::path abs_path = project_root / row.rel_path;
            auto lines = read_lines(abs_path);
            if (lines.empty()) {
                spdlog::warn("extract_chunks: could not read file '{}', skipping symbols in it",
                             abs_path.string());
                // Cache empty vector so we don't retry this file
                file_cache.emplace(row.rel_path, std::vector<std::string>{});
                continue;
            }
            it = file_cache.emplace(row.rel_path, std::move(lines)).first;
        }

        const auto& lines = it->second;
        if (lines.empty()) {
            // File was unreadable (cached empty)
            continue;
        }

        // Extract lines [line_start, line_end] (1-based inclusive), clamp to file end
        const int total_lines = static_cast<int>(lines.size());
        const int start = row.line_start - 1;  // 0-based
        const int end   = std::min(row.line_end, total_lines) - 1;  // 0-based inclusive

        if (start > end || start >= total_lines) {
            spdlog::warn("extract_chunks: symbol '{}' line range [{}, {}] out of file bounds ({}), skipping",
                         row.name, row.line_start, row.line_end, total_lines);
            continue;
        }

        // Join extracted lines into body text
        std::string body;
        for (int i = start; i <= end; ++i) {
            if (i > start) body += '\n';
            body += lines[i];
        }

        // Build context header
        const std::string header = make_context_header(
            row.rel_path, row.kind, row.name, row.signature);

        // Assemble chunk
        Chunk chunk;
        chunk.symbol_id  = row.id;
        chunk.file_path  = row.rel_path;
        chunk.kind       = row.kind;
        chunk.name       = row.name;
        chunk.signature  = row.signature;
        chunk.line_start = row.line_start;
        chunk.line_end   = row.line_end;
        chunk.text       = header + body;
        chunks.push_back(std::move(chunk));
    }

    return chunks;
}

} // namespace codetldr
