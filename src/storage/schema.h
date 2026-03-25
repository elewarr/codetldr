#pragma once
#include <string_view>
#include <vector>
#include <utility>

namespace codetldr {

// Each migration: {version_number, sql_statements}
// Migrations are additive only (CREATE, ADD COLUMN) -- never DROP or destructive.
inline const std::vector<std::pair<int, std::string_view>> kMigrations = {
    {1, R"sql(
        CREATE TABLE IF NOT EXISTS files (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            path         TEXT NOT NULL UNIQUE,
            language     TEXT,
            mtime_ns     INTEGER NOT NULL DEFAULT 0,
            content_hash TEXT,
            indexed_at   TEXT NOT NULL DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
    )sql"},
};

} // namespace codetldr
