#pragma once

#include <cstdint>
#include <mutex>
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

    /// Guards every statement on this connection. Recursive so a caller
    /// holding the lock across several repo calls (multi-statement
    /// operations) doesn't deadlock against inner calls.
    std::recursive_mutex& mutex() { return m_; }

    /// RAII transaction: BEGIN IMMEDIATE on construction, ROLLBACK on
    /// destruction unless commit() ran. Failed operations never leave
    /// partial rows behind.
    class Tx {
    public:
        explicit Tx(Database& d) : d_(d) { d_.exec("BEGIN IMMEDIATE"); }
        ~Tx() { if (!done_) d_.exec("ROLLBACK"); }
        Tx(const Tx&) = delete;
        void commit() { d_.exec("COMMIT"); done_ = true; }
        void rollback() { d_.exec("ROLLBACK"); done_ = true; }
    private:
        Database& d_;
        bool done_ { false };
    };

private:
    bool migrate();
    sqlite3* db_ { nullptr };
    std::recursive_mutex m_;
};

} // namespace onda
