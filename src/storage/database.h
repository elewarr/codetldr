#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <memory>

namespace codetldr {

class Database {
public:
    // Open (or create) database at db_path with WAL mode enabled.
    // Runs all pending migrations. Throws on failure.
    static Database open(const std::filesystem::path& db_path);

    // Check that WAL mode is active
    bool wal_mode_confirmed() const;

    // Get current schema version (max applied migration)
    int schema_version() const;

    // Access underlying SQLite::Database for queries
    SQLite::Database& raw() { return *db_; }

private:
    explicit Database(std::unique_ptr<SQLite::Database> db) : db_(std::move(db)) {}
    std::unique_ptr<SQLite::Database> db_;
};

} // namespace codetldr
