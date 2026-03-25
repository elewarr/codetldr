#include "storage/database.h"
#include "storage/migrations.h"
#include <stdexcept>
#include <string>

namespace codetldr {

Database Database::open(const std::filesystem::path& db_path) {
    // Ensure parent directory exists
    std::filesystem::create_directories(db_path.parent_path());

    // Open (or create) the database
    auto db = std::make_unique<SQLite::Database>(
        db_path.string(),
        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE
    );

    // Enable WAL mode
    db->exec("PRAGMA journal_mode = WAL");

    // Verify WAL mode was actually enabled
    {
        SQLite::Statement q(*db, "PRAGMA journal_mode");
        if (q.executeStep()) {
            std::string actual_mode = q.getColumn(0).getText();
            if (actual_mode != "wal") {
                throw std::runtime_error("Failed to enable WAL mode: got " + actual_mode);
            }
        }
    }

    // Apply remaining pragmas
    db->exec("PRAGMA synchronous = NORMAL");
    db->exec("PRAGMA foreign_keys = ON");
    db->exec("PRAGMA wal_autocheckpoint = 1000");

    // Run schema migrations
    run_migrations(*db);

    return Database(std::move(db));
}

bool Database::wal_mode_confirmed() const {
    SQLite::Statement q(*db_, "PRAGMA journal_mode");
    if (q.executeStep()) {
        std::string mode = q.getColumn(0).getText();
        return mode == "wal";
    }
    return false;
}

int Database::schema_version() const {
    SQLite::Statement q(*db_, "SELECT MAX(version) FROM schema_version");
    if (q.executeStep()) {
        if (!q.getColumn(0).isNull()) {
            return q.getColumn(0).getInt();
        }
    }
    return 0;
}

} // namespace codetldr
