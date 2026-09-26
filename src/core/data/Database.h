#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace onda {

/// Thin SQLite wrapper (single-writer, WAL mode). All calls are
/// synchronous — run from worker threads; UI reads through the store.
/// schema_migrations drives versioned upgrades.
class Database {
public:
    Database() = default;
    ~Database() { close(); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& o) noexcept : db_(o.db_) { o.db_ = nullptr; }
    Database& operator=(Database&& o) noexcept {
        if (this != &o) { close(); db_ = o.db_; o.db_ = nullptr; }
        return *this;
    }

    bool open(const std::string& path);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    bool exec(const char* sql);
    int64_t lastInsertId() const;

    class Stmt {
    public:
        Stmt() = default;
        Stmt(sqlite3* db, const char* sql) { prepare(db, sql); }
        ~Stmt() { finalize(); }
        Stmt(Stmt&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
        Stmt& operator=(Stmt&&) = delete;
        Stmt(const Stmt&) = delete;

        bool prepare(sqlite3* db, const char* sql);
        void finalize();
        bool bind(int idx, int64_t v);
        bool bind(int idx, double v);
        bool bind(int idx, const std::string& v);
        bool bind(int idx, const void* blob, int n);
        bool bindNull(int idx);
        bool step();          // true = row available
        int64_t colInt(int i) const;
        double colDouble(int i) const;
        std::string colText(int i) const;
        std::vector<uint8_t> colBlob(int i) const;
        void reset();

    private:
        sqlite3_stmt* s_ { nullptr };
    };

    Stmt prepare(const char* sql) { return Stmt(db_, sql); }

private:
    bool migrate();
    sqlite3* db_ { nullptr };
};

} // namespace onda
