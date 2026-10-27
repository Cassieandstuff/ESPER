// ---------------------------------------------------------------------------
// esper-sqlite — exports a load order into a relational SQLite database.
//
//   esper-sqlite <DataFolder> <out.db> [--plugins-txt <file>] [--strings <dir>]
//
// Without --plugins-txt the game's own plugins.txt is used (%LOCALAPPDATA%), so
// the database holds whatever load order the game would load. --strings points
// at loose string tables, for names of localized plugins (vanilla ships them
// in BSAs, which ESPER doesn't read).
//
// Tables (docs/sql-examples.md explains the design and has example queries):
//   plugins         one row per loaded plugin, in load order
//   plugin_masters  each plugin's master list (plugin → plugin)
//   records         one row per record: runtime FormID, type, EditorID, name,
//                   defining plugin, winning plugin
//   overrides       one row per plugin that overrides a record
//   fields          the schema paths FormIDs are found at, per record type
//   refs            record → referenced FormID, via a field
//   <SIG>           one table per schema record type (NPC_, WEAP, ...), its
//                   columns generated from the schema: every single-valued
//                   field at the record's root, structs flattened
//
// Everything is read from the winning version of each record. FormIDs are
// runtime FormIDs (what the console shows), so they join across plugins.
// Opt-in target: configure with -DESPER_BUILD_SQLITE=ON.
// ---------------------------------------------------------------------------
#include <ESPER/LoadOrder.h>

#include <sqlite3.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace esper;
using schema::Kind;

// ── SQLite helpers ───────────────────────────────────────────────────────────

class Db {
public:
    ~Db() { if (db_) sqlite3_close(db_); }
    bool Open(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) == SQLITE_OK) return true;
        std::printf("FAIL  open %s: %s\n", path.c_str(), sqlite3_errmsg(db_));
        return false;
    }
    bool Exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK) return true;
        std::printf("FAIL  %s\n  in: %.200s\n", err ? err : "?", sql.c_str());
        sqlite3_free(err);
        return false;
    }
    sqlite3* Handle() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

// A prepared statement, bound by 1-based parameter index and run by Step().
class Stmt {
public:
    Stmt() = default;
    Stmt(const Db& db, const std::string& sql) {
        if (sqlite3_prepare_v2(db.Handle(), sql.c_str(), -1, &s_, nullptr) != SQLITE_OK)
            std::printf("FAIL  prepare: %s\n  in: %.200s\n", sqlite3_errmsg(db.Handle()), sql.c_str());
    }
    Stmt(Stmt&& o) noexcept : s_(std::exchange(o.s_, nullptr)) {}
    Stmt& operator=(Stmt&& o) noexcept { std::swap(s_, o.s_); return *this; }
    ~Stmt() { sqlite3_finalize(s_); }

    bool Ok() const { return s_ != nullptr; }
    void Null(int i)                      { sqlite3_bind_null(s_, i); }
    void Int(int i, int64_t v)            { sqlite3_bind_int64(s_, i, v); }
    void Real(int i, double v)            { sqlite3_bind_double(s_, i, v); }
    void Text(int i, const std::string& v) {
        if (v.empty()) Null(i);
        else sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void Int(int i, std::optional<int64_t> v) { if (v) Int(i, *v); else Null(i); }
    bool Step() {
        const int rc = sqlite3_step(s_);
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
        if (rc == SQLITE_DONE || rc == SQLITE_ROW) return true;
        std::printf("FAIL  step: %s\n", sqlite3_errmsg(sqlite3_db_handle(s_)));
        return false;
    }

private:
    sqlite3_stmt* s_ = nullptr;
};

static std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ── Runtime FormIDs ──────────────────────────────────────────────────────────
// A FormKey (plugin + local id) becomes the FormID the game uses at runtime:
// regular plugins take index byte 0x00..0xFD, light plugins share 0xFE with a
// 12-bit slot. That makes one integer key valid across the whole load order.

struct Slot {
    bool     light = false;
    uint32_t index = 0;
};

class RuntimeIds {
public:
    explicit RuntimeIds(const LoadOrderIndex& index) {
        for (size_t i = 0; i < index.RegularOrder().size(); ++i)
            slots_[Lower(index.RegularOrder()[i]->filename)] = { false, static_cast<uint32_t>(i) };
        for (size_t i = 0; i < index.EslOrder().size(); ++i)
            slots_[Lower(index.EslOrder()[i]->filename)] = { true, static_cast<uint32_t>(i) };
    }
    std::optional<int64_t> Of(const FormKey& key) const {
        auto it = slots_.find(Lower(key.plugin));
        if (it == slots_.end()) return std::nullopt;
        if (it->second.light) return 0xFE000000ll | (int64_t{ it->second.index } << 12) | (key.localId & 0xFFF);
        return (int64_t{ it->second.index } << 24) | (key.localId & 0xFFFFFF);
    }
    std::string Prefix(const std::string& plugin) const {
        auto it = slots_.find(Lower(plugin));
        if (it == slots_.end()) return {};
        char buf[8];
        if (it->second.light) std::snprintf(buf, sizeof buf, "FE%03X", it->second.index);
        else std::snprintf(buf, sizeof buf, "%02X", it->second.index);
        return buf;
    }

private:
    std::unordered_map<std::string, Slot> slots_;
};

// ── Per-type tables, reflected from the schema ───────────────────────────────
// Every single-valued subrecord at a record's root becomes columns: a scalar
// is one column, a struct is flattened into one column per scalar field
// ("data_value", "data_weight"). Repeated subrecords, groups, arrays, unions
// and opaque bytes are left out: they aren't one value per record (FormIDs in
// them are still in `refs`).

struct Column {
    std::string           name;
    RelPath               path;
    const schema::TypeDef* type = nullptr;
};

struct TypeTable {
    std::string         name;
    std::vector<Column> columns;
    Stmt                insert;
};

static bool IsScalar(Kind k) {
    switch (k) {
    case Kind::U8: case Kind::U16: case Kind::U32: case Kind::U64:
    case Kind::I8: case Kind::I16: case Kind::I32: case Kind::I64:
    case Kind::F32: case Kind::FormId: case Kind::Sig4:
    case Kind::ZString: case Kind::WString: case Kind::LString:
        return true;
    default:
        return false;
    }
}

static const char* SqlType(Kind k) {
    switch (k) {
    case Kind::F32: return "REAL";
    case Kind::Sig4: case Kind::ZString: case Kind::WString: case Kind::LString: return "TEXT";
    default: return "INTEGER";
    }
}

static void AddColumns(const schema::SchemaSet& s, const std::string& name, uint16_t typeIdx,
                       const RelPath& path, std::set<std::string>& taken, std::vector<Column>& out) {
    const schema::TypeDef& t = s.types[typeIdx];
    if (IsScalar(t.kind)) {
        std::string col = name;
        for (char& c : col) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        std::string unique = col;
        for (int n = 2; !taken.insert(Lower(unique)).second; ++n) unique = col + "_" + std::to_string(n);
        out.push_back({ unique, path, &t });
    } else if (t.kind == Kind::Struct && path.depth < path.idx.size()) {
        uint16_t vi = 0;
        for (uint16_t i = 0; i < t.refCount; ++i) {
            const schema::FieldDef& f = s.fields[t.ref + i];
            if (f.isBreak) continue;
            AddColumns(s, name + "_" + f.name, f.type, Extend(path, vi++), taken, out);
        }
    }
}

static std::vector<Column> ReflectColumns(const schema::SchemaSet& s, const schema::RecordDef& rd) {
    std::vector<Column> out;
    std::set<std::string> taken = { "form_id" };
    const schema::GroupDef& g = s.groups[rd.root];
    for (uint16_t i = 0; i < g.memberCount; ++i) {
        const schema::MemberDef& md = s.members[g.firstMember + i];
        if (md.isGroup || md.repeat || std::string_view(md.name) == "editorId") continue;
        RelPath p;
        p.idx[0] = i;
        p.memberDepth = p.depth = 1;
        AddColumns(s, md.name, md.type, p, taken, out);
    }
    return out;
}

// ── References, labelled with the field they came from ───────────────────────
// Record::ForEachFormId is the authority on which FormIDs a record holds; it
// doesn't say where each one sits. This walk over the same schema adds the
// field path, and is used only when it finds exactly the FormIDs
// ForEachFormId does (otherwise the record's refs get the path "?").

struct Ref {
    uint32_t    raw;
    std::string path;
};

static void WalkValue(const schema::SchemaSet& s, uint16_t typeIdx, const Value& v,
                      const std::string& path, std::vector<Ref>& out) {
    const schema::TypeDef& t = s.types[typeIdx];
    switch (t.kind) {
    case Kind::FormId:
        out.push_back({ static_cast<uint32_t>(v.bits), path });
        return;
    case Kind::Struct: {
        uint16_t vi = 0;
        for (uint16_t i = 0; i < t.refCount && vi < v.items.size(); ++i) {
            const schema::FieldDef& f = s.fields[t.ref + i];
            if (f.isBreak) continue;
            WalkValue(s, f.type, v.items[vi++], path + "/" + f.name, out);
        }
        return;
    }
    case Kind::Array:
        for (const Value& item : v.items) WalkValue(s, t.ref, item, path, out);
        return;
    case Kind::Union:
        if (v.bits < t.refCount && !v.items.empty()) {
            const schema::CaseDef& cd = s.cases[t.ref + static_cast<uint16_t>(v.bits)];
            if (!cd.empty) WalkValue(s, cd.type, v.items[0], cd.name ? path + "/" + cd.name : path, out);
        }
        return;
    default:
        return;
    }
}

static void WalkScope(Record& rec, uint32_t inst, uint16_t groupIdx, const std::string& prefix,
                      std::vector<Ref>& out) {
    const schema::SchemaSet& s = rec.Schema();
    const schema::GroupDef& g = s.groups[groupIdx];
    for (uint16_t i = 0; i < g.memberCount; ++i) {
        const schema::MemberDef& md = s.members[g.firstMember + i];
        RelPath p;
        p.idx[0] = i;
        p.memberDepth = p.depth = 1;
        const std::string path = prefix + md.name;
        if (md.isGroup) {
            for (const Cursor& c : rec.GroupsAt(inst, p)) WalkScope(rec, c.inst, md.group, path + "/", out);
        } else {
            for (const Value* v : rec.FindAllValues(inst, p)) WalkValue(s, md.type, *v, path, out);
        }
    }
}

// ── Export ───────────────────────────────────────────────────────────────────

static const char* kCoreSchema = R"sql(
CREATE TABLE plugins (
    load_order     INTEGER PRIMARY KEY,           -- 0 = first loaded
    name           TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    runtime_prefix TEXT    NOT NULL,              -- '00'..'FD', or 'FE000'.. for light plugins
    is_master      INTEGER NOT NULL,
    is_light       INTEGER NOT NULL,
    is_localized   INTEGER NOT NULL
);
CREATE TABLE plugin_masters (
    plugin      INTEGER NOT NULL REFERENCES plugins(load_order),
    position    INTEGER NOT NULL,                 -- order in the plugin's master list
    master_name TEXT    NOT NULL COLLATE NOCASE,
    master      INTEGER REFERENCES plugins(load_order),   -- NULL when the master isn't loaded
    PRIMARY KEY (plugin, position)
);
CREATE TABLE records (
    form_id         INTEGER PRIMARY KEY,          -- runtime FormID
    type            TEXT    NOT NULL,             -- record signature: 'NPC_', 'WEAP', ...
    editor_id       TEXT    COLLATE NOCASE,
    name            TEXT,                         -- FULL; NULL when absent or unresolved
    defining_plugin INTEGER NOT NULL REFERENCES plugins(load_order),
    winning_plugin  INTEGER NOT NULL REFERENCES plugins(load_order),
    is_deleted      INTEGER NOT NULL              -- the winning version deletes the record
);
CREATE TABLE overrides (
    form_id    INTEGER NOT NULL REFERENCES records(form_id),
    plugin     INTEGER NOT NULL REFERENCES plugins(load_order),
    is_deleted INTEGER NOT NULL,
    PRIMARY KEY (form_id, plugin)
) WITHOUT ROWID;
CREATE TABLE fields (
    field_id    INTEGER PRIMARY KEY,
    record_type TEXT NOT NULL,
    path        TEXT NOT NULL,                    -- schema path: 'items/item', 'race', ...
    UNIQUE (record_type, path)
);
CREATE TABLE refs (
    from_form_id INTEGER NOT NULL REFERENCES records(form_id),
    field_id     INTEGER NOT NULL REFERENCES fields(field_id),
    to_form_id   INTEGER                          -- no FK: real data has dangling references
);
)sql";

static const char* kIndexes = R"sql(
CREATE INDEX records_type            ON records(type);
CREATE INDEX records_editor_id       ON records(editor_id);
CREATE INDEX records_defining_plugin ON records(defining_plugin);
CREATE INDEX records_winning_plugin  ON records(winning_plugin);
CREATE INDEX overrides_plugin        ON overrides(plugin);
CREATE INDEX refs_from               ON refs(from_form_id);
CREATE INDEX refs_to                 ON refs(to_form_id);
)sql";

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: esper-sqlite <DataFolder> <out.db> [--plugins-txt <file>] [--strings <dir>]\n");
        return 2;
    }
    LoadOrderOptions opt;
    opt.dataFolder = argv[1];
    const std::string outPath = argv[2];
    std::string stringsDir;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--plugins-txt") opt.pluginsTxt = argv[i + 1];
        else if (a == "--strings") stringsDir = argv[i + 1];
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    if (!stringsDir.empty()) {
        const std::string data = opt.dataFolder;
        opt.fetch = [data, stringsDir](const std::string& rel, std::vector<uint8_t>& out) {
            for (const fs::path& p : { fs::path(data) / rel, fs::path(stringsDir) / fs::path(rel).filename() }) {
                std::ifstream f(p, std::ios::binary);
                if (!f) continue;
                out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                return true;
            }
            return false;
        };
    }

    const auto t0 = std::chrono::steady_clock::now();
    LoadOrderDocument doc;
    if (auto ok = doc.Load(opt); !ok) {
        std::printf("FAIL  load: %s\n", ok.error().message.c_str());
        return 1;
    }
    for (const std::string& w : doc.Warnings()) std::printf("warn  %s\n", w.c_str());
    const LoadOrderIndex& index = doc.Index();
    const RuntimeIds runtime(index);

    std::error_code ec;
    fs::remove(outPath, ec);
    Db db;
    if (!db.Open(outPath)) return 1;
    // A one-shot bulk load: no journal, no fsync. A crash just means rerunning.
    if (!db.Exec("PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF; BEGIN;") ||
        !db.Exec(kCoreSchema))
        return 1;

    // Plugins and their masters.
    std::unordered_map<const Plugin*, int64_t> loadOrder;
    std::unordered_map<std::string, int64_t> loadOrderByName;
    {
        Stmt ins(db, "INSERT INTO plugins VALUES (?, ?, ?, ?, ?, ?)");
        for (size_t i = 0; i < doc.Plugins().size(); ++i) {
            const Plugin& p = *doc.Plugins()[i];
            loadOrder[&p] = static_cast<int64_t>(i);
            loadOrderByName[Lower(p.filename)] = static_cast<int64_t>(i);
            ins.Int(1, static_cast<int64_t>(i));
            ins.Text(2, p.filename);
            ins.Text(3, runtime.Prefix(p.filename));
            ins.Int(4, p.isMaster);
            ins.Int(5, p.isLight);
            ins.Int(6, p.localized);
            if (!ins.Step()) return 1;
        }
        Stmt mast(db, "INSERT INTO plugin_masters VALUES (?, ?, ?, ?)");
        for (const auto& [p, lo] : loadOrder) {
            for (size_t m = 0; m < p->masters.size(); ++m) {
                auto it = loadOrderByName.find(Lower(p->masters[m]));
                mast.Int(1, lo);
                mast.Int(2, static_cast<int64_t>(m));
                mast.Text(3, p->masters[m]);
                mast.Int(4, it == loadOrderByName.end() ? std::nullopt : std::optional<int64_t>(it->second));
                if (!mast.Step()) return 1;
            }
        }
    }

    // One table per schema record type.
    const schema::SchemaSet& set = schema::Main();
    std::unordered_map<uint32_t, TypeTable> typeTables;
    for (const schema::RecordDef& rd : set.records) {
        TypeTable tt;
        tt.name = SigName(rd.sig);
        tt.columns = ReflectColumns(set, rd);
        std::string ddl = "CREATE TABLE \"" + tt.name + "\" (\n    form_id INTEGER PRIMARY KEY REFERENCES records(form_id)";
        std::string ins = "INSERT INTO \"" + tt.name + "\" VALUES (?";
        for (const Column& c : tt.columns) {
            ddl += ",\n    \"" + c.name + "\" " + SqlType(c.type->kind);
            ins += ", ?";
        }
        if (!db.Exec(ddl + "\n);")) return 1;
        tt.insert = Stmt(db, ins + ")");
        if (!tt.insert.Ok()) return 1;
        typeTables.emplace(rd.sig, std::move(tt));
    }

    Stmt insRecord(db, "INSERT INTO records VALUES (?, ?, ?, ?, ?, ?, ?)");
    Stmt insOverride(db, "INSERT INTO overrides VALUES (?, ?, ?)");
    Stmt insField(db, "INSERT INTO fields VALUES (?, ?, ?)");
    Stmt insRef(db, "INSERT INTO refs VALUES (?, ?, ?)");
    std::map<std::pair<std::string, std::string>, int64_t> fieldIds;
    auto fieldId = [&](const std::string& type, const std::string& path) -> int64_t {
        auto [it, added] = fieldIds.try_emplace({ type, path }, static_cast<int64_t>(fieldIds.size() + 1));
        if (added) {
            insField.Int(1, it->second);
            insField.Text(2, type);
            insField.Text(3, path);
            insField.Step();
        }
        return it->second;
    };

    size_t records = 0, overrides = 0, refs = 0, typed = 0, unlabelled = 0;
    std::map<std::string, size_t> undecoded;
    std::vector<Ref> walked;
    std::vector<uint32_t> authority, mine;

    for (const auto& pluginPtr : doc.Plugins()) {
        const Plugin& plugin = *pluginPtr;
        // Records this plugin defines (not its overrides), in FormID order.
        std::vector<uint32_t> raws;
        for (const auto& [raw, node] : plugin.index)
            if (Lower(plugin.FormIdToKey(raw).plugin) == Lower(plugin.filename)) raws.push_back(raw);
        std::sort(raws.begin(), raws.end());

        for (uint32_t raw : raws) {
            const FormKey key = plugin.FormIdToKey(raw);
            const std::vector<ResolvedRecord>* chain = index.ResolveChain(key);
            const std::optional<int64_t> formId = runtime.Of(key);
            if (!chain || chain->empty() || !formId) continue;
            const ResolvedRecord& winner = chain->back();
            const std::string type = SigName(winner.Sig());
            const bool deleted = winner.node->Deleted();

            insRecord.Int(1, *formId);
            insRecord.Text(2, type);
            insRecord.Text(3, doc.EditorId(winner));
            insRecord.Text(4, deleted ? std::string() : doc.Name(winner));
            insRecord.Int(5, loadOrder.at(chain->front().owner));
            insRecord.Int(6, loadOrder.at(winner.owner));
            insRecord.Int(7, deleted);
            if (!insRecord.Step()) return 1;
            ++records;

            for (size_t v = 1; v < chain->size(); ++v) {
                insOverride.Int(1, *formId);
                insOverride.Int(2, loadOrder.at((*chain)[v].owner));
                insOverride.Int(3, (*chain)[v].node->Deleted());
                if (!insOverride.Step()) return 1;
                ++overrides;
            }
            if (deleted) continue;

            auto decoded = doc.Decode(winner);
            if (!decoded) { ++undecoded[type]; continue; }
            Record& rec = decoded->record;
            const Plugin& owner = *winner.owner;
            auto resolve = [&](uint32_t id) -> std::optional<int64_t> {
                if (id == 0) return std::nullopt;
                return runtime.Of(index.ResolveFormId(&owner, id));
            };

            // The typed row.
            if (auto it = typeTables.find(winner.Sig()); it != typeTables.end()) {
                TypeTable& tt = it->second;
                tt.insert.Int(1, *formId);
                for (size_t c = 0; c < tt.columns.size(); ++c) {
                    const Column& col = tt.columns[c];
                    const int slot = static_cast<int>(c) + 2;
                    const Value* v = rec.GetValue(col.path);
                    if (!v) continue;                       // absent: stays NULL
                    switch (col.type->kind) {
                    case Kind::I8:  tt.insert.Int(slot, static_cast<int8_t>(v->bits)); break;
                    case Kind::I16: tt.insert.Int(slot, static_cast<int16_t>(v->bits)); break;
                    case Kind::I32: tt.insert.Int(slot, static_cast<int32_t>(v->bits)); break;
                    case Kind::F32: tt.insert.Real(slot, std::bit_cast<float>(static_cast<uint32_t>(v->bits))); break;
                    case Kind::FormId: tt.insert.Int(slot, resolve(static_cast<uint32_t>(v->bits))); break;
                    case Kind::Sig4: tt.insert.Text(slot, SigName(static_cast<uint32_t>(v->bits))); break;
                    case Kind::ZString: case Kind::WString: tt.insert.Text(slot, v->str); break;
                    case Kind::LString:
                        tt.insert.Text(slot, doc.Text(owner, LocalizedString{ static_cast<uint32_t>(v->bits),
                                                                              v->str, col.type->table }));
                        break;
                    default: tt.insert.Int(slot, static_cast<int64_t>(v->bits)); break;  // unsigned
                    }
                }
                if (!tt.insert.Step()) return 1;
                ++typed;
            }

            // References.
            authority.clear();
            rec.ForEachFormId([&](FormId id, std::span<const uint32_t>) { authority.push_back(id.raw); });
            walked.clear();
            WalkScope(rec, 0, rec.Def().root, "", walked);
            mine.clear();
            for (const Ref& r : walked) mine.push_back(r.raw);
            std::sort(authority.begin(), authority.end());
            std::sort(mine.begin(), mine.end());
            if (mine != authority) {
                ++unlabelled;
                walked.clear();
                for (uint32_t id : authority) walked.push_back({ id, "?" });
            }
            for (const Ref& r : walked) {
                if (r.raw == 0) continue;                   // a null reference
                insRef.Int(1, *formId);
                insRef.Int(2, fieldId(type, r.path));
                insRef.Int(3, resolve(r.raw));
                if (!insRef.Step()) return 1;
                ++refs;
            }
        }
    }

    const double loadSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu plugins, %zu records, %zu overrides, %zu typed rows, %zu refs (%.1fs)\n",
                doc.Plugins().size(), records, overrides, typed, refs, loadSec);
    if (unlabelled) std::printf("note  %zu record(s) with refs whose field couldn't be labelled ('?')\n", unlabelled);
    for (const auto& [type, n] : undecoded) std::printf("note  %zu %s record(s) not decoded (no schema or malformed)\n", n, type.c_str());

    // Indexes go in after the data: building one sorted index at the end is
    // much faster than updating it on every insert.
    if (!db.Exec(kIndexes) || !db.Exec("COMMIT; ANALYZE;")) return 1;

    // Every foreign key the schema declares must hold.
    size_t violations = 0;
    sqlite3_exec(db.Handle(), "PRAGMA foreign_key_check;",
                 [](void* n, int, char**, char**) { ++*static_cast<size_t*>(n); return 0; }, &violations, nullptr);
    const double totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("indexed; %zu foreign-key violation(s); wrote %s (%.1f MB, %.1fs)\n", violations, outPath.c_str(),
                static_cast<double>(fs::file_size(outPath)) / (1024.0 * 1024.0), totalSec);
    return violations == 0 ? 0 : 1;
}
