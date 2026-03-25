#include "storage/migrations.h"
#include "storage/schema.h"
#include <stdexcept>
#include <string>

namespace codetldr {

void run_migrations(SQLite::Database& db) {
    // Create schema_version table if not exists
    db.exec(
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "  version INTEGER PRIMARY KEY,"
        "  applied TEXT NOT NULL DEFAULT (datetime('now'))"
        ")"
    );

    // Query current schema version
    int current_version = 0;
    {
        SQLite::Statement q(db, "SELECT MAX(version) FROM schema_version");
        if (q.executeStep()) {
            if (!q.getColumn(0).isNull()) {
                current_version = q.getColumn(0).getInt();
            }
        }
    }

    // Apply pending migrations
    for (const auto& [ver, sql] : kMigrations) {
        if (ver <= current_version) {
            continue;
        }

        try {
            SQLite::Transaction txn(db);
            db.exec(std::string(sql));
            SQLite::Statement ins(db, "INSERT INTO schema_version (version) VALUES (?)");
            ins.bind(1, ver);
            ins.exec();
            txn.commit();
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::string("Migration v") + std::to_string(ver) + " failed: " + e.what()
            );
        }
    }
}

} // namespace codetldr
