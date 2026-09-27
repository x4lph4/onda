#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../data/Database.h"
#include "SampleBank.h"
#include "SamplerTypes.h"

namespace onda::sampler {

/// Persistence for samples/banks/slots (task §27/§28). Shares the
/// library Database connection — a sample row may be referenced by
/// many slots across banks. All calls are synchronous: keep them on
/// worker/control threads.
class SampleRepository {
public:
    explicit SampleRepository(Database& db) : db_(db) {}

    bool init() {
        std::lock_guard lk(db_.mutex());
        return db_.exec(R"SQL(
            CREATE TABLE IF NOT EXISTS samples (
                id INTEGER PRIMARY KEY,
                path TEXT NOT NULL,
                name TEXT NOT NULL DEFAULT '',
                color INTEGER NOT NULL DEFAULT 0,
                duration REAL NOT NULL DEFAULT 0,
                peak REAL NOT NULL DEFAULT 0,
                bpm REAL NOT NULL DEFAULT 0,
                sample_rate REAL NOT NULL DEFAULT 0,
                channels INTEGER NOT NULL DEFAULT 0,
                first_seen REAL DEFAULT (unixepoch()),
                UNIQUE(path)
            );
            CREATE TABLE IF NOT EXISTS sample_banks (
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL,
                position INTEGER NOT NULL DEFAULT 0,
                slot_count INTEGER NOT NULL DEFAULT 8
            );
            CREATE TABLE IF NOT EXISTS sample_slots (
                bank_id INTEGER NOT NULL,
                slot INTEGER NOT NULL,
                sample_id INTEGER NOT NULL DEFAULT -1,
                name TEXT NOT NULL DEFAULT '',
                color INTEGER NOT NULL DEFAULT 0,
                volume REAL NOT NULL DEFAULT 1,
                pan REAL NOT NULL DEFAULT 0,
                gain REAL NOT NULL DEFAULT 1,
                pitch REAL NOT NULL DEFAULT 0,
                mode INTEGER NOT NULL DEFAULT 0,
                quantize INTEGER NOT NULL DEFAULT 0,
                sync_on INTEGER NOT NULL DEFAULT 0,
                loop_on INTEGER NOT NULL DEFAULT 0,
                mute INTEGER NOT NULL DEFAULT 0,
                solo INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (bank_id, slot)
            );
            CREATE INDEX IF NOT EXISTS idx_slots_bank
                ON sample_slots(bank_id);
        )SQL") && ensureColumns();
    }

    // ---- samples ----
    int64_t upsertSample(const std::string& path,
                         const std::string& name, double dur,
                         float peak, double bpm, double sr, int ch) {
        std::lock_guard lk(db_.mutex());
        {
            auto s = db_.prepare(
                "INSERT INTO samples(path,name,duration,peak,bpm,"
                "sample_rate,channels) VALUES(?,?,?,?,?,?,?) "
                "ON CONFLICT(path) DO UPDATE SET "
                "name=excluded.name,duration=excluded.duration,"
                "peak=excluded.peak,bpm=excluded.bpm");
            s.bind(1, path); s.bind(2, name); s.bind(3, dur);
            s.bind(4, (double)peak); s.bind(5, bpm); s.bind(6, sr);
            s.bind(7, (int64_t)ch);
            s.step();          // INSERT → SQLITE_DONE (no row)
        }
        auto q = db_.prepare("SELECT id FROM samples WHERE path=?");
        q.bind(1, path);
        return q.step() ? q.colInt(0) : -1;
    }

    // ---- banks ----
    int64_t upsertBank(int64_t id, const std::string& name, int pos,
                       int slotCount) {
        std::lock_guard lk(db_.mutex());
        if (id < 0) {
            auto s = db_.prepare(
                "INSERT INTO sample_banks(name,position,slot_count)"
                " VALUES(?,?,?)");
            s.bind(1, name); s.bind(2, (int64_t)pos);
            s.bind(3, (int64_t)slotCount);
            s.step();          // INSERT → SQLITE_DONE (no row)
            return db_.lastInsertId();
        }
        auto s = db_.prepare(
            "INSERT INTO sample_banks(id,name,position,slot_count)"
            " VALUES(?,?,?,?) ON CONFLICT(id) DO UPDATE SET"
            " name=excluded.name,position=excluded.position,"
            "slot_count=excluded.slot_count");
        s.bind(1, id); s.bind(2, name); s.bind(3, (int64_t)pos);
        s.bind(4, (int64_t)slotCount);
        s.step();
        return id;
    }
    bool deleteBank(int64_t id) {
        std::lock_guard lk(db_.mutex());
        Database::Tx tx(db_);
        auto s = db_.prepare("DELETE FROM sample_slots WHERE bank_id=?");
        s.bind(1, id); s.step();
        auto b = db_.prepare("DELETE FROM sample_banks WHERE id=?");
        b.bind(1, id);
        b.step();              // DELETE → SQLITE_DONE (no row)
        tx.commit();
        return true;
    }

    // ---- slots ----
    bool saveSlot(int64_t bankId, const SampleSlot& s) {
        std::lock_guard lk(db_.mutex());
        auto q = db_.prepare(
            "INSERT INTO sample_slots(bank_id,slot,sample_id,name,color,"
            "volume,pan,gain,pitch,mode,quantize,sync_on,loop_on,mute,solo,"
            "attack_ms,release_ms,trim_start,trim_end,loop_in,loop_out,"
            "filter_pos)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(bank_id,slot) DO UPDATE SET"
            " sample_id=excluded.sample_id,name=excluded.name,"
            "color=excluded.color,volume=excluded.volume,"
            "pan=excluded.pan,gain=excluded.gain,pitch=excluded.pitch,"
            "mode=excluded.mode,quantize=excluded.quantize,"
            "sync_on=excluded.sync_on,loop_on=excluded.loop_on,"
            "mute=excluded.mute,solo=excluded.solo,"
            "attack_ms=excluded.attack_ms,"
            "release_ms=excluded.release_ms,"
            "trim_start=excluded.trim_start,"
            "trim_end=excluded.trim_end,loop_in=excluded.loop_in,"
            "loop_out=excluded.loop_out,"
            "filter_pos=excluded.filter_pos");
        q.bind(1, bankId); q.bind(2, (int64_t)s.slot);
        q.bind(3, s.sampleId); q.bind(4, s.name);
        q.bind(5, (int64_t)s.colorArgb); q.bind(6, (double)s.volume);
        q.bind(7, (double)s.pan); q.bind(8, (double)s.gain);
        q.bind(9, (double)s.pitch); q.bind(10, (int64_t)s.mode);
        q.bind(11, (int64_t)s.quant);
        q.bind(12, (int64_t)(s.syncOn ? 1 : 0));
        q.bind(13, (int64_t)(s.loopOn ? 1 : 0));
        q.bind(14, (int64_t)(s.mute ? 1 : 0));
        q.bind(15, (int64_t)(s.solo ? 1 : 0));
        q.bind(16, (double)s.attackMs); q.bind(17, (double)s.releaseMs);
        q.bind(18, s.startF); q.bind(19, s.endF);
        q.bind(20, s.loopIn); q.bind(21, s.loopOut);
        q.bind(22, (double)s.filterPos);
        q.step();              // INSERT/UPDATE → SQLITE_DONE (no row)
        return true;
    }

    /// Load all banks (+slots +sample path). App thread.
    std::vector<SampleBank> loadAll() {
        std::lock_guard lk(db_.mutex());
        std::vector<SampleBank> banks;
        {
            auto b = db_.prepare(
                "SELECT id,name,slot_count FROM sample_banks"
                " ORDER BY position, id");
            while (b.step()) {
                SampleBank bank;
                bank.id = b.colInt(0);
                bank.name = b.colText(1);
                bank.resize((int)b.colInt(2));
                banks.push_back(std::move(bank));
            }
        }
        for (auto& bank : banks) {
            auto s = db_.prepare(
                "SELECT sl.slot,sl.sample_id,sl.name,sl.color,sl.volume,"
                "sl.pan,sl.gain,sl.pitch,sl.mode,sl.quantize,sl.sync_on,"
                "sl.loop_on,sl.mute,sl.solo,sm.path,sm.bpm,sm.duration,"
                "sl.attack_ms,sl.release_ms,sl.trim_start,sl.trim_end,"
                "sl.loop_in,sl.loop_out,sl.filter_pos"
                " FROM sample_slots sl LEFT JOIN samples sm"
                " ON sm.id=sl.sample_id"
                " WHERE sl.bank_id=? ORDER BY sl.slot");
            s.bind(1, bank.id);
            while (s.step()) {
                const int i = (int)s.colInt(0);
                if (i < 0 || i >= bank.slotCount()) continue;
                SampleSlot& sl = bank.slot(i);
                sl.slot = i;
                sl.sampleId = s.colInt(1);
                sl.name = s.colText(2);
                sl.colorArgb = (uint32_t)s.colInt(3);
                sl.volume = (float)s.colDouble(4);
                sl.pan = (float)s.colDouble(5);
                sl.gain = (float)s.colDouble(6);
                sl.pitch = (float)s.colDouble(7);
                sl.mode = (PlaybackMode)(int)s.colInt(8);
                sl.quant = (QuantizeMode)(int)s.colInt(9);
                sl.syncOn = s.colInt(10) != 0;
                sl.loopOn = s.colInt(11) != 0;
                sl.mute = s.colInt(12) != 0;
                sl.solo = s.colInt(13) != 0;
                sl.attackMs = (float)s.colDouble(17);
                sl.releaseMs = (float)s.colDouble(18);
                sl.startF = s.colDouble(19);
                sl.endF = s.colDouble(20);
                sl.loopIn = s.colDouble(21);
                sl.loopOut = s.colDouble(22);
                sl.filterPos = (float)s.colDouble(23);
                if (sl.sampleId > 0) {
                    sl.path = s.colText(14);
                    sl.bpm.store(s.colDouble(15));
                    sl.state.store((int)SlotState::Queued);
                }
            }
        }
        return banks;
    }

private:
    /// Phase 34 — add metadata columns to DBs created before they
    /// existed. Each ALTER is guarded by PRAGMA table_info so init()
    /// stays idempotent on both old and new databases.
    bool ensureColumns() {
        struct Add { const char* name; const char* sql; };
        static const Add adds[] = {
            {"attack_ms",  "ALTER TABLE sample_slots ADD COLUMN"
             " attack_ms REAL NOT NULL DEFAULT 0"},
            {"release_ms", "ALTER TABLE sample_slots ADD COLUMN"
             " release_ms REAL NOT NULL DEFAULT 0"},
            {"trim_start", "ALTER TABLE sample_slots ADD COLUMN"
             " trim_start REAL NOT NULL DEFAULT -1"},
            {"trim_end",   "ALTER TABLE sample_slots ADD COLUMN"
             " trim_end REAL NOT NULL DEFAULT -1"},
            {"loop_in",    "ALTER TABLE sample_slots ADD COLUMN"
             " loop_in REAL NOT NULL DEFAULT -1"},
            {"loop_out",   "ALTER TABLE sample_slots ADD COLUMN"
             " loop_out REAL NOT NULL DEFAULT -1"},
            {"filter_pos", "ALTER TABLE sample_slots ADD COLUMN"
             " filter_pos REAL NOT NULL DEFAULT 0"},
        };
        std::vector<std::string> have;
        {
            auto q = db_.prepare("PRAGMA table_info(sample_slots)");
            while (q.step()) have.push_back(q.colText(1));
        }
        for (const Add& a : adds) {
            bool found = false;
            for (const auto& h : have)
                if (h == a.name) { found = true; break; }
            if (!found && !db_.exec(a.sql)) return false;
        }
        return true;
    }

    Database& db_;
};

} // namespace onda::sampler
