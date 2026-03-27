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
    {2, R"sql(
        CREATE TABLE IF NOT EXISTS symbols (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            file_id       INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
            kind          TEXT NOT NULL,
            name          TEXT NOT NULL,
            signature     TEXT,
            line_start    INTEGER NOT NULL,
            line_end      INTEGER NOT NULL,
            documentation TEXT,
            indexed_at    TEXT NOT NULL DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_symbols_file ON symbols(file_id);
        CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(name);
        CREATE INDEX IF NOT EXISTS idx_symbols_kind ON symbols(kind);
    )sql"},
    {3, R"sql(
        CREATE TABLE IF NOT EXISTS calls (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            caller_id     INTEGER REFERENCES symbols(id) ON DELETE CASCADE,
            callee_id     INTEGER REFERENCES symbols(id) ON DELETE SET NULL,
            callee_name   TEXT NOT NULL,
            file_id       INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
            line          INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_calls_caller ON calls(caller_id);
        CREATE INDEX IF NOT EXISTS idx_calls_callee_name ON calls(callee_name);
        CREATE INDEX IF NOT EXISTS idx_calls_file ON calls(file_id);
    )sql"},
    {4, R"sql(
        CREATE VIRTUAL TABLE IF NOT EXISTS symbols_fts USING fts5(
            name, signature, documentation,
            content='symbols', content_rowid='id',
            prefix='2 3', tokenize='unicode61'
        );
        INSERT INTO symbols_fts(rowid, name, signature, documentation)
            SELECT id, name, COALESCE(signature,''), COALESCE(documentation,'') FROM symbols;
    )sql"},
    {5, R"sql(
        CREATE TABLE IF NOT EXISTS cfg_nodes (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            file_id    INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
            symbol_id  INTEGER REFERENCES symbols(id) ON DELETE CASCADE,
            node_type  TEXT NOT NULL,
            condition  TEXT,
            line       INTEGER NOT NULL,
            depth      INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_cfg_nodes_file   ON cfg_nodes(file_id);
        CREATE INDEX IF NOT EXISTS idx_cfg_nodes_symbol ON cfg_nodes(symbol_id);
    )sql"},
    {6, R"sql(
        CREATE TABLE IF NOT EXISTS dfg_edges (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            file_id     INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
            symbol_id   INTEGER REFERENCES symbols(id) ON DELETE CASCADE,
            edge_type   TEXT NOT NULL,
            lhs         TEXT NOT NULL,
            rhs_snippet TEXT,
            line        INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_dfg_edges_file   ON dfg_edges(file_id);
        CREATE INDEX IF NOT EXISTS idx_dfg_edges_symbol ON dfg_edges(symbol_id);
    )sql"},
    {7, R"sql(
        CREATE TABLE IF NOT EXISTS metadata (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )sql"},
    {8, R"sql(
        CREATE TABLE IF NOT EXISTS embedded_files (
            file_id   INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
            symbol_id INTEGER NOT NULL,
            PRIMARY KEY (file_id, symbol_id)
        );
        CREATE INDEX IF NOT EXISTS idx_embedded_files_file ON embedded_files(file_id);
    )sql"},
};

} // namespace codetldr
