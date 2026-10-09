#!/usr/bin/env python3
"""schemagen — compile ESPER's YAML schema into schema.json + C++ headers.

    schemagen.py --schema schema --out include/ESPER/generated --json schema.json
    schemagen.py ... --check        # exit 1 if any output is stale

Emits, into --out:
    Tables.h   constexpr tables the engine interprets (esper::schema::SchemaSet)
    Fields.h   typed handles (esper::FieldRef<T> / GroupRef / StructRef)
    Views.h    typed views (thin accessors over esper::Cursor)

The descriptor language is specified in docs/schema-language.md. This file is
the reference implementation of its validation rules: anything it accepts the
engine can interpret, and anything outside language 0.1 is rejected by name.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("schemagen: PyYAML is required (pip install pyyaml)")

LANGUAGE_VERSION = "0.1"

SCALAR_KINDS = {
    "u8": ("U8", "uint8_t"), "u16": ("U16", "uint16_t"), "u32": ("U32", "uint32_t"),
    "u64": ("U64", "uint64_t"), "i8": ("I8", "int8_t"), "i16": ("I16", "int16_t"),
    "i32": ("I32", "int32_t"), "i64": ("I64", "int64_t"), "f32": ("F32", "float"),
    "formid": ("FormId", "::esper::FormId"), "sig4": ("Sig4", "uint32_t"),
    "zstring": ("ZString", "std::string"), "wstring": ("WString", "std::string"),
    "lstring": ("LString", "::esper::LocalizedString"),
    "bytes": ("Bytes", "std::vector<uint8_t>"),
}
FIXED_SIZE = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4, "f32": 4,
              "formid": 4, "sig4": 4, "u64": 8, "i64": 8}
STR_TABLES = {"strings": "Strings", "dlstrings": "DLStrings", "ilstrings": "ILStrings"}

# Constructs specified by the design but not in language 0.1: reject by name.
NOT_YET = {
    "union": "unions", "if": "conditional fields", "open": "group open markers",
    "end": "group end markers", "enum": "enum hints", "flags": "flag hints",
    "unit": "unit hints",
}

CPP_KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char
char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast
continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum
explicit export extern false float for friend goto if inline int long mutable namespace new
noexcept not not_eq nullptr operator or or_eq private protected public register
reinterpret_cast requires return short signed sizeof static static_assert static_cast struct
switch template this thread_local throw true try typedef typeid typename union unsigned using
virtual void volatile wchar_t while xor xor_eq""".split())
VIEW_RESERVED = {"record", "cursor", "self"}


class SchemaError(Exception):
    pass


def fail(where: str, msg: str):
    raise SchemaError(f"{where}: {msg}")


def ident(name: str) -> str:
    return name + "_" if name in CPP_KEYWORDS else name


def pascal(name: str) -> str:
    return name[:1].upper() + name[1:]


def sig_int(sig: str) -> int:
    b = sig.encode("ascii")
    return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24


def check_sig(where: str, sig) -> str:
    if not isinstance(sig, str) or len(sig) != 4 or not all(0x20 < ord(c) < 0x7F for c in sig):
        fail(where, f"signature must be a 4-character ASCII string, got {sig!r} "
                    "(quote it in YAML if it looks like a number or boolean)")
    return sig


def check_name(where: str, name) -> str:
    if not isinstance(name, str) or not re.fullmatch(r"[a-z][A-Za-z0-9]*", name):
        fail(where, f"name must be camelCase ([a-z][A-Za-z0-9]*), got {name!r}")
    return name


def check_keys(where: str, d: dict, allowed: set[str]):
    for k in d:
        if k in NOT_YET:
            fail(where, f"'{k}' ({NOT_YET[k]}) is not supported in language {LANGUAGE_VERSION}")
        if k not in allowed:
            fail(where, f"unknown key '{k}' (allowed: {', '.join(sorted(allowed))})")


# ── Model ─────────────────────────────────────────────────────────────────────

@dataclass
class TypeNode:
    kind: str                  # C++ Kind enumerator
    spec: str                  # canonical spelling, for dedup + json
    len: str = "None"
    table: str = "None"
    flags: int = 0
    count: int = 0
    ref: int = 0               # Array elem type / Struct first field
    ref_count: int = 0
    links: tuple = ()
    name: str | None = None    # named struct
    fixed_size: int | None = None
    has_break: bool = False
    cpp: str = ""              # value type for handles
    struct_fields: list = field(default_factory=list)   # (name|None, type idx, is_break)


@dataclass
class Member:
    name: str
    is_group: bool
    sig: str = ""
    type: int = 0
    group: int = 0
    repeat: bool = False
    optional: bool = False
    count_of: str | None = None


@dataclass
class Group:
    name: str                  # view type name
    members: list
    ordered: bool
    entries: list = field(default_factory=list)   # (sig, member index)


class Schema:
    def __init__(self):
        self.types: list[TypeNode] = []
        self.type_index: dict[str, int] = {}
        self.groups: list[Group] = []
        self.records: list[tuple[str, str, int]] = []   # (sig, name, root group)
        self.struct_defs: dict[str, dict] = {}
        self.group_defs: dict[str, dict] = {}
        self.record_defs: list[tuple[str, dict]] = []
        self.manifest: dict = {}
        self.named: dict[str, int] = {}          # named struct → type index
        self.building: set[str] = set()

    # ── loading ──────────────────────────────────────────────────────────────

    def load(self, root: Path):
        files = sorted(p for p in root.rglob("*") if p.suffix in (".yaml", ".yml"))
        if not files:
            fail(str(root), "no descriptor files")
        for path in files:
            rel = path.relative_to(root).as_posix()
            with path.open(encoding="utf-8") as f:
                docs = [d for d in yaml.safe_load_all(f) if d is not None]
            for doc in docs:
                if not isinstance(doc, dict):
                    fail(rel, "a descriptor must be a mapping")
                kinds = [k for k in ("record", "struct", "group", "manifest", "enum", "table") if k in doc]
                if len(kinds) != 1:
                    fail(rel, "a descriptor must have exactly one of record/struct/group/manifest")
                kind = kinds[0]
                if kind in ("enum", "table"):
                    fail(rel, f"'{kind}' descriptors are not supported in language {LANGUAGE_VERSION}")
                if kind == "manifest":
                    if self.manifest:
                        fail(rel, "more than one manifest")
                    check_keys(rel, doc, {"manifest", "schemaVersion"})
                    self.manifest = doc
                elif kind == "struct":
                    name = doc["struct"]
                    if not isinstance(name, str) or not re.fullmatch(r"[A-Z][A-Za-z0-9]*", name):
                        fail(rel, f"struct name must be PascalCase, got {name!r}")
                    if name in self.struct_defs:
                        fail(rel, f"struct {name} defined twice")
                    self.struct_defs[name] = dict(doc, _where=rel)
                elif kind == "group":
                    name = doc["group"]
                    if not isinstance(name, str) or not re.fullmatch(r"[A-Z][A-Za-z0-9]*", name):
                        fail(rel, f"reusable group name must be PascalCase, got {name!r}")
                    if name in self.group_defs:
                        fail(rel, f"group {name} defined twice")
                    self.group_defs[name] = dict(doc, _where=rel)
                else:
                    check_sig(rel, doc["record"])
                    self.record_defs.append((rel, doc))
        if not self.manifest or not isinstance(self.manifest.get("schemaVersion"), str):
            fail(str(root), "a manifest with schemaVersion: \"x.y.z\" is required")

    # ── types ────────────────────────────────────────────────────────────────

    def intern(self, node: TypeNode) -> int:
        key = f"{node.spec}|{node.len}|{node.table}|{node.flags}|{node.links}"
        if key in self.type_index:
            return self.type_index[key]
        self.types.append(node)
        self.type_index[key] = len(self.types) - 1
        return len(self.types) - 1

    def scalar(self, where: str, base: str, opts: dict) -> int:
        kind, cpp = SCALAR_KINDS[base]
        node = TypeNode(kind=kind, spec=base, cpp=cpp, fixed_size=FIXED_SIZE.get(base))
        if base == "lstring":
            table = opts.get("table")
            if table not in STR_TABLES:
                fail(where, f"lstring needs table: strings | dlstrings | ilstrings, got {table!r}")
            node.table = STR_TABLES[table]
            node.spec = f"lstring:{table}"
        elif "table" in opts:
            fail(where, "'table' only applies to lstring")
        if base == "bytes":
            size = opts.get("size")
            if size is not None:
                if not isinstance(size, int) or size <= 0:
                    fail(where, f"bytes size must be a positive integer, got {size!r}")
                node.len, node.count, node.fixed_size = "Fixed", size, size
                node.spec = f"bytes:{size}"
            else:
                node.len = "Rest"
            fids = opts.get("formIds")
            if fids not in (None, "none"):
                fail(where, f"formIds may only be 'none', got {fids!r}")
            if fids == "none":
                node.flags = 1
                node.spec += ":noids"
        else:
            if "size" in opts:
                fail(where, "'size' only applies to bytes")
            if "formIds" in opts:
                fail(where, "'formIds' only applies to bytes")
        if base == "formid":
            to = opts.get("to", [])
            if not isinstance(to, list):
                fail(where, "'to' must be a list of record signatures")
            node.links = tuple(check_sig(where, s) for s in to)
            if node.links:
                node.spec = "formid->" + ",".join(node.links)
        elif "to" in opts:
            fail(where, "'to' only applies to formid")
        return self.intern(node)

    def resolve_type(self, where: str, spec, opts: dict) -> int:
        if not isinstance(spec, str):
            fail(where, f"type must be a string, got {spec!r}")
        m = re.fullmatch(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*([^\]]*)\s*\])?\s*", spec)
        if not m:
            fail(where, f"bad type {spec!r}")
        base, arr = m.group(1), m.group(2)
        if arr is None:
            return self.base_type(where, base, opts)
        elem = self.base_type(where, base, opts)
        et = self.types[elem]
        if et.has_break:
            fail(where, "a struct with a break can't be an array element")
        node = TypeNode(kind="Array", spec="", ref=elem, cpp=f"std::vector<{et.cpp}>")
        if arr == "":
            node.len = "Rest"
        elif re.fullmatch(r"\d+", arr):
            node.len, node.count = "Fixed", int(arr)
            if et.fixed_size is not None:
                node.fixed_size = et.fixed_size * node.count
        else:
            cm = re.fullmatch(r"count\s*:\s*(\w+)", arr)
            if not cm:
                fail(where, f"bad array length [{arr}]")
            prefix = {"u8": "Prefix8", "u16": "Prefix16", "u32": "Prefix32"}.get(cm.group(1))
            if prefix is None:
                fail(where, f"[count: {cm.group(1)}] — counts from a field are not supported in "
                            f"language {LANGUAGE_VERSION}; use u8, u16 or u32")
            node.len = prefix
        node.spec = f"{et.spec}[{arr}]"
        return self.intern(node)

    def base_type(self, where: str, base: str, opts: dict) -> int:
        if base in SCALAR_KINDS:
            return self.scalar(where, base, opts)
        for k in ("table", "size", "formIds", "to"):
            if k in opts:
                fail(where, f"'{k}' doesn't apply to struct type {base}")
        if base not in self.struct_defs:
            fail(where, f"unknown type {base!r}")
        return self.named_struct(base)

    def named_struct(self, name: str) -> int:
        if name in self.named:
            return self.named[name]
        d = self.struct_defs[name]
        where = d["_where"]
        if name in self.building:
            fail(where, f"struct {name} contains itself")
        self.building.add(name)
        check_keys(where, d, {"struct", "size", "fields", "_where"})
        idx = self.build_struct(where, d.get("fields"), spec=f"struct:{name}", name=name)
        self.building.discard(name)
        self.named[name] = idx
        size = d.get("size")
        t = self.types[idx]
        if size is not None:
            if t.fixed_size is None:
                fail(where, f"struct {name} declares size {size} but its size is variable")
            if t.fixed_size != size:
                fail(where, f"struct {name} declares size {size} but its fields sum to {t.fixed_size}")
        return idx

    def build_struct(self, where: str, fields, spec: str, name: str | None) -> int:
        if not isinstance(fields, list) or not fields:
            fail(where, "a struct needs a non-empty fields list")
        node = TypeNode(kind="Struct", spec=spec, name=name)
        total, seen, open_ended = 0, set(), False
        for i, f in enumerate(fields):
            fw = f"{where} field {i}"
            if not isinstance(f, dict):
                fail(fw, "a field must be a mapping")
            if "break" in f:
                check_keys(fw, f, {"break"})
                if f["break"] is not True:
                    fail(fw, "break must be 'true'")
                if open_ended:
                    fail(fw, "a break can't follow a variable-length field")
                node.struct_fields.append((None, 0, True))
                node.has_break = True
                continue
            check_keys(fw, f, {"name", "type", "table", "size", "formIds", "to"})
            fname = check_name(fw, f.get("name"))
            if fname in VIEW_RESERVED:
                fail(fw, f"field name {fname!r} is reserved")
            if fname in seen:
                fail(fw, f"duplicate field name {fname}")
            seen.add(fname)
            if open_ended:
                fail(fw, "only the last field may be variable-length (rest arrays/bytes)")
            ftype = self.resolve_type(fw, f.get("type"), f)
            ft = self.types[ftype]
            if ft.has_break:
                fail(fw, "a struct with a break can only be a subrecord's top-level type")
            if ft.len == "Rest" or (ft.kind == "Bytes" and ft.len == "Rest"):
                open_ended = True
            node.struct_fields.append((fname, ftype, False))
            total = None if (total is None or ft.fixed_size is None) else total + ft.fixed_size
        node.fixed_size = None if node.has_break else total
        node.cpp = "::esper::Value"
        return self.intern(node)

    # ── groups ───────────────────────────────────────────────────────────────

    def build_group(self, where: str, members, view_name: str, ordered: bool,
                    remap: dict[str, str]) -> int:
        if not isinstance(members, list) or not members:
            fail(where, "a scope needs a non-empty members list")
        group = Group(name=view_name, members=[], ordered=ordered)
        gidx = len(self.groups)
        self.groups.append(group)
        names = set()
        for i, m in enumerate(members):
            mw = f"{where} member {i}"
            if not isinstance(m, dict):
                fail(mw, "a member must be a mapping")
            if "group" in m or "use" in m:
                if "group" in m and "use" in m:
                    fail(mw, "a member can't have both 'group' and 'use'")
                if "use" in m:
                    check_keys(mw, m, {"use", "name", "sigs", "repeat", "optional"})
                    gname = m["use"]
                    if gname not in self.group_defs:
                        fail(mw, f"unknown reusable group {gname!r}")
                    gd = self.group_defs[gname]
                    check_keys(gd["_where"], gd, {"group", "members", "_where"})
                    mname = check_name(mw, m.get("name"))
                    sigs = m.get("sigs", {}) or {}
                    if not isinstance(sigs, dict):
                        fail(mw, "'sigs' must map original signatures to new ones")
                    sub = {check_sig(mw, k): check_sig(mw, v) for k, v in sigs.items()}
                    used = self.group_sigs(gd["members"])
                    for k in sub:
                        if k not in used:
                            fail(mw, f"sigs remaps {k}, which group {gname} doesn't use")
                    # The use's own remap applies first, then any enclosing one.
                    merged = {}
                    for s0 in used:
                        s1 = sub.get(s0, s0)
                        merged[s0] = remap.get(s1, s1)
                    child = self.build_group(gd["_where"], gd["members"], gname, True, merged)
                else:
                    check_keys(mw, m, {"group", "members", "repeat", "optional"})
                    mname = check_name(mw, m.get("group"))
                    child = self.build_group(mw, m.get("members"), view_name + pascal(mname),
                                             True, remap)
                member = Member(name=mname, is_group=True, group=child,
                                repeat=bool(m.get("repeat", False)),
                                optional=bool(m.get("optional", False)))
            else:
                check_keys(mw, m, {"sig", "name", "type", "fields", "repeat", "optional",
                                   "countOf", "table", "size", "formIds", "to"})
                sig = check_sig(mw, m.get("sig"))
                sig = remap.get(sig, sig)
                mname = check_name(mw, m.get("name"))
                if "fields" in m:
                    if "type" in m:
                        fail(mw, "a member has either 'type' or inline 'fields', not both")
                    tidx = self.build_struct(mw, m["fields"],
                                             spec=f"inline:{view_name}.{mname}",
                                             name=view_name + pascal(mname))
                else:
                    tidx = self.resolve_type(mw, m.get("type"), m)
                member = Member(name=mname, is_group=False, sig=sig, type=tidx,
                                repeat=bool(m.get("repeat", False)),
                                optional=bool(m.get("optional", False)),
                                count_of=m.get("countOf"))
            if member.name in names:
                fail(mw, f"duplicate member name {member.name}")
            if member.name in VIEW_RESERVED:
                fail(mw, f"member name {member.name!r} is reserved")
            names.add(member.name)
            group.members.append(member)

        # derived counts
        for i, mem in enumerate(group.members):
            if mem.count_of is None:
                continue
            target = [j for j, o in enumerate(group.members) if o.name == mem.count_of]
            if not target:
                fail(where, f"{mem.name}: countOf names unknown member {mem.count_of!r}")
            if self.types[mem.type].kind not in ("U8", "U16", "U32"):
                fail(where, f"{mem.name}: a countOf member must be u8, u16 or u32")
            tm = group.members[target[0]]
            if tm.is_group:
                fail(where, f"{mem.name}: countOf can't count a group")
            if not tm.repeat and self.types[tm.type].kind not in ("Array", "Bytes"):
                fail(where, f"{mem.name}: countOf target must be repeated or an array")
            mem.count_of = target[0]

        # entry sigs: a group member is entered by any of its entry sigs
        seen: dict[str, int] = {}
        for i, mem in enumerate(group.members):
            sigs = [mem.sig] if not mem.is_group else [s for s, _ in self.groups[mem.group].entries]
            for s in sigs:
                if s in seen:
                    a, b = group.members[seen[s]].name, mem.name
                    fail(where, f"signature {s} is ambiguous in this scope ({a} vs {b})")
                seen[s] = i
                group.entries.append((s, i))
        return gidx

    def group_sigs(self, members) -> set[str]:
        out = set()
        for m in members or []:
            if "sig" in m:
                out.add(m["sig"])
            if "members" in m:
                out |= self.group_sigs(m["members"])
            if "use" in m and m["use"] in self.group_defs:
                inner = self.group_sigs(self.group_defs[m["use"]]["members"])
                remap = m.get("sigs", {}) or {}
                out |= {remap.get(s, s) for s in inner}
        return out

    # ── records ──────────────────────────────────────────────────────────────

    def build(self):
        seen_sigs = {}
        for where, d in self.record_defs:
            check_keys(where, d, {"record", "name", "members"})
            sig = d["record"]
            if sig in seen_sigs:
                fail(where, f"record {sig} also defined in {seen_sigs[sig]}")
            seen_sigs[sig] = where
            name = d.get("name")
            if not isinstance(name, str) or not re.fullmatch(r"[A-Z][A-Za-z0-9]*", name):
                fail(where, f"record name must be PascalCase, got {name!r}")
            root = self.build_group(where, d.get("members"), name, False, {})
            self.records.append((sig, name, root))
        self.records.sort(key=lambda r: sig_int(r[0]))
        # every named struct is compiled even when unused (so json/views see it)
        for name in sorted(self.struct_defs):
            self.named_struct(name)
        self.check_view_names()

    def check_view_names(self):
        owners: dict[str, str] = {}

        def claim(n: str, what: str):
            if n in owners and owners[n] != what:
                fail("views", f"type name {n} is used by both {owners[n]} and {what}")
            owners[n] = what

        for sig, name, _ in self.records:
            claim(name, f"record {sig}")
        for t in self.types:
            if t.kind == "Struct":
                claim(t.name, f"struct {t.spec}")
        for g in self.groups:
            if g.ordered:
                claim(g.name, f"group {g.name}")


# ── Emission ──────────────────────────────────────────────────────────────────

HEADER = "// GENERATED by tools/schemagen from schema/ — do not edit. Regenerate with\n" \
         "// `cmake --build <dir> --target esper-schemagen` (or tools/schemagen/schemagen.py).\n"


def c_str(s: str | None) -> str:
    return "nullptr" if s is None else json.dumps(s)


def flatten(schema: Schema):
    """Assign flat table indices (fields, links, members, entries)."""
    fields, links = [], []
    type_rows = []
    for t in schema.types:
        row = dict(kind=t.kind, len=t.len, table=t.table, flags=t.flags, count=t.count,
                   ref=t.ref, refCount=0, linkFirst=0, linkCount=0, name=t.name)
        if t.kind == "Struct":
            row["ref"] = len(fields)
            row["refCount"] = len(t.struct_fields)
            fields.extend(t.struct_fields)
        if t.links:
            row["linkFirst"] = len(links)
            row["linkCount"] = len(t.links)
            links.extend(t.links)
        type_rows.append(row)
    members, entries, group_rows = [], [], []
    for g in schema.groups:
        group_rows.append(dict(name=g.name, firstMember=len(members), memberCount=len(g.members),
                               firstEntry=len(entries), entryCount=len(g.entries),
                               ordered=g.ordered))
        members.extend(g.members)
        entries.extend(g.entries)
    return type_rows, fields, links, members, entries, group_rows


def emit_tables(schema: Schema, ns: str) -> str:
    type_rows, fields, links, members, entries, group_rows = flatten(schema)
    out = ["#pragma once", HEADER, "#include <ESPER/SchemaTypes.h>", "",
           f"namespace {ns} {{", "", "using namespace ::esper::schema;", ""]

    out.append("inline constexpr TypeDef kTypes[] = {")
    for i, r in enumerate(type_rows):
        out.append(f"    {{ Kind::{r['kind']}, LenKind::{r['len']}, StrTable::{r['table']}, "
                   f"{r['flags']}, {r['count']}, {r['ref']}, {r['refCount']}, "
                   f"{r['linkFirst']}, {r['linkCount']}, {c_str(r['name'])} }},  // {i}: {schema.types[i].spec}")
    out.append("};")
    out.append("inline constexpr FieldDef kFields[] = {")
    if not fields:
        out.append("    { nullptr, 0, false },")
    for name, t, brk in fields:
        out.append(f"    {{ {c_str(name)}, {t}, {'true' if brk else 'false'} }},")
    out.append("};")
    out.append("inline constexpr uint32_t kLinks[] = {")
    out.append("    " + (", ".join(f"SigOf(\"{s}\")" for s in links) if links else "0") + ",")
    out.append("};")
    out.append("inline constexpr MemberDef kMembers[] = {")
    for m in members:
        sig = f"SigOf(\"{m.sig}\")" if m.sig else "0"
        count_of = m.count_of if isinstance(m.count_of, int) else -1
        out.append(f"    {{ {c_str(m.name)}, {sig}, {m.type}, {m.group}, "
                   f"{'true' if m.is_group else 'false'}, {'true' if m.repeat else 'false'}, "
                   f"{count_of} }},")
    out.append("};")
    out.append("inline constexpr EntryDef kEntries[] = {")
    for s, mi in entries:
        out.append(f"    {{ SigOf(\"{s}\"), {mi} }},")
    out.append("};")
    out.append("inline constexpr GroupDef kGroups[] = {")
    for i, g in enumerate(group_rows):
        out.append(f"    {{ {c_str(g['name'])}, {g['firstMember']}, {g['memberCount']}, "
                   f"{g['firstEntry']}, {g['entryCount']}, {'true' if g['ordered'] else 'false'} }},  // {i}")
    out.append("};")
    out.append("inline constexpr RecordDef kRecords[] = {")
    for sig, name, root in schema.records:
        out.append(f"    {{ SigOf(\"{sig}\"), {c_str(name)}, {root} }},")
    out.append("};")
    out.append("")
    out.append("inline constexpr SchemaSet kSchema{")
    out.append("    kTypes,")
    out.append(f"    std::span<const FieldDef>(kFields, {len(fields)}),")
    out.append(f"    std::span<const uint32_t>(kLinks, {len(links)}),")
    out.append("    kMembers, kEntries, kGroups, kRecords,")
    out.append(f"    {c_str(schema.manifest['schemaVersion'])}, {c_str(LANGUAGE_VERSION)},")
    out.append("};")
    out.append("")
    out.append(f"}} // namespace {ns}")
    return "\n".join(out) + "\n"


def relpath_init(idx: list[int], member_depth: int) -> str:
    if len(idx) > 8:
        raise SchemaError(f"path {idx} is deeper than 8 levels (esper::RelPath limit)")
    return "{ { " + ", ".join(str(i) for i in idx) + f" }}, {member_depth}, {len(idx)} }}"


def value_fields(schema: Schema, tidx: int):
    """(name, type idx, value index) of a struct's non-break fields."""
    out, vi = [], 0
    for name, t, brk in schema.types[tidx].struct_fields:
        if brk:
            continue
        out.append((name, t, vi))
        vi += 1
    return out


def emit_fields(schema: Schema, ns: str) -> str:
    out = ["#pragma once", HEADER, "#include <ESPER/Record.h>", "",
           "#include <cstdint>", "#include <string>", "#include <vector>", "",
           f"namespace {ns} {{", "", "using ::esper::schema::SigOf;", ""]

    def struct_handles(tidx: int, path: list[int], md: int, sig: str, indent: str):
        for fname, ft, vi in value_fields(schema, tidx):
            t = schema.types[ft]
            p = path + [vi]
            if t.kind == "Struct":
                out.append(f"{indent}namespace {ident(fname)} {{")
                out.append(f"{indent}    inline constexpr ::esper::StructRef self{{ SigOf(\"{sig}\"), {relpath_init(p, md)} }};")
                struct_handles(ft, p, md, sig, indent + "    ")
                out.append(f"{indent}}}")
            else:
                out.append(f"{indent}inline constexpr ::esper::FieldRef<{t.cpp}> {ident(fname)}"
                           f"{{ SigOf(\"{sig}\"), {relpath_init(p, md)} }};")

    def scope(gidx: int, path: list[int], sig: str, indent: str):
        for i, m in enumerate(schema.groups[gidx].members):
            p = path + [i]
            if m.is_group:
                out.append(f"{indent}namespace {ident(m.name)} {{")
                out.append(f"{indent}    inline constexpr ::esper::GroupRef self{{ SigOf(\"{sig}\"), {relpath_init(p, len(p))} }};")
                scope(m.group, p, sig, indent + "    ")
                out.append(f"{indent}}}")
                continue
            t = schema.types[m.type]
            if t.kind == "Struct":
                out.append(f"{indent}namespace {ident(m.name)} {{")
                out.append(f"{indent}    inline constexpr ::esper::StructRef self{{ SigOf(\"{sig}\"), {relpath_init(p, len(p))} }};")
                struct_handles(m.type, p, len(p), sig, indent + "    ")
                out.append(f"{indent}}}")
            else:
                out.append(f"{indent}inline constexpr ::esper::FieldRef<{t.cpp}> {ident(m.name)}"
                           f"{{ SigOf(\"{sig}\"), {relpath_init(p, len(p))} }};")

    for sig, name, root in schema.records:
        out.append(f"// {sig} — {name}")
        out.append(f"namespace {ident(sig)} {{")
        scope(root, [], sig, "    ")
        out.append("}")
        out.append("")
    out.append(f"}} // namespace {ns}")
    return "\n".join(out) + "\n"


def emit_views(schema: Schema, ns: str) -> str:
    """Views are accessors only: every call goes through esper::Cursor."""
    by_sig = {sig: name for sig, name, _ in schema.records}

    def q(name: str) -> str:            # accessors may share a view type's name
        return f"::{ns}::{name}"
    out = ["#pragma once", HEADER, "#include <ESPER/Record.h>", "",
           "#include <cstdint>", "#include <optional>", "#include <string>", "#include <vector>", "",
           f"namespace {ns} {{", ""]

    # forward declarations (typed links, nested views)
    names = sorted({name for _, name, _ in schema.records} |
                   {t.name for t in schema.types if t.kind == "Struct"} |
                   {g.name for g in schema.groups if g.ordered})
    for n in names:
        out.append(f"class {n};")
    out.append("")

    def link_view(t: TypeNode) -> str | None:
        if t.kind == "FormId" and len(t.links) == 1 and t.links[0] in by_sig:
            return q(by_sig[t.links[0]])
        return None

    def rel(path: list[int], md: int) -> str:
        return f"::esper::RelPath{relpath_init(path, md)}"

    def value_accessors(body: list[str], acc: str, t_idx: int, path_expr: str, repeat: bool,
                        cursor: str, comment: str):
        t = schema.types[t_idx]
        lv = link_view(t)
        el = schema.types[t.ref] if t.kind == "Array" else None
        elv = link_view(el) if el is not None else None
        body.append(f"    // {comment}")
        if t.kind == "Struct":
            body.append(f"    {q(t.name)} {acc}() const;")
            body.append(f"    bool Has{acc}() const {{ return {cursor}.Has({path_expr}); }}")
            return
        if repeat:
            vt = f"::esper::Link<{lv}>" if lv else t.cpp
            body.append(f"    std::vector<{vt}> {acc}() const {{ return {cursor}.GetAll<{vt}>({path_expr}); }}")
            body.append(f"    void Set{acc}(const std::vector<{vt}>& v) const {{ {cursor}.SetAll<{vt}>({path_expr}, v); }}")
            return
        if lv:
            vt = f"::esper::Link<{lv}>"
        elif elv:
            vt = f"std::vector<::esper::Link<{elv}>>"
        else:
            vt = t.cpp
        body.append(f"    std::optional<{vt}> {acc}() const {{ return {cursor}.Get<{vt}>({path_expr}); }}")
        body.append(f"    void Set{acc}(const {vt}& v) const {{ {cursor}.Set<{vt}>({path_expr}, v); }}")

    deferred: list[str] = []      # out-of-line struct accessors (need complete types)
    emitted: set[str] = set()

    def emit_struct_view(tidx: int):
        t = schema.types[tidx]
        if t.name in emitted:
            return
        emitted.add(t.name)
        for _, ft, _ in value_fields(schema, tidx):
            if schema.types[ft].kind == "Struct":
                emit_struct_view(ft)
        body = [f"class {t.name} {{", "public:",
                f"    explicit {t.name}(::esper::StructCursor s) : s_(s) {{}}",
                "    bool IsPresent() const { return s_.Has(); }"]
        for fname, ft, vi in value_fields(schema, tidx):
            acc = pascal(fname)
            if schema.types[ft].kind == "Struct":
                sv = q(schema.types[ft].name)
                body.append(f"    {sv} {acc}() const {{ return {sv}(s_.Nested({vi})); }}")
                continue
            ftn = schema.types[ft]
            lv = link_view(ftn)
            vt = f"::esper::Link<{lv}>" if lv else ftn.cpp
            body.append(f"    std::optional<{vt}> {acc}() const {{ return s_.at.Get<{vt}>(s_.Field({vi})); }}")
            body.append(f"    void Set{acc}(const {vt}& v) const {{ s_.at.Set<{vt}>(s_.Field({vi}), v); }}")
        body += ["private:", "    ::esper::StructCursor s_;", "};", ""]
        out.extend(body)

    for tidx, t in enumerate(schema.types):
        if t.kind == "Struct":
            emit_struct_view(tidx)

    def emit_scope_view(gidx: int, cls: str, is_record: bool, sig: str | None):
        g = schema.groups[gidx]
        for m in g.members:      # nested group views first
            if m.is_group and schema.groups[m.group].name not in emitted:
                emitted.add(schema.groups[m.group].name)
                emit_scope_view(m.group, schema.groups[m.group].name, False, None)
        body = [f"class {cls} {{", "public:"]
        if is_record:
            body.append(f"    static constexpr uint32_t kSig = ::esper::schema::SigOf(\"{sig}\");")
            body.append(f"    explicit {cls}(::esper::Record& r) : c_{{ &r, 0 }} {{}}")
        body.append(f"    explicit {cls}(::esper::Cursor c) : c_(c) {{}}")
        body.append("    ::esper::Record& record() const { return *c_.rec; }")
        body.append("    ::esper::Cursor cursor() const { return c_; }")
        for i, m in enumerate(g.members):
            acc = pascal(m.name)
            path = rel([i], 1)
            if m.is_group:
                gv = q(schema.groups[m.group].name)
                body.append(f"    // group {m.name}")
                if m.repeat:
                    body.append(f"    std::vector<{gv}> {acc}() const {{ std::vector<{gv}> o; for (auto c : c_.Groups({path})) o.emplace_back(c); return o; }}")
                    body.append(f"    {gv} Add{acc}() const {{ return {gv}(c_.AddGroup({path})); }}")
                else:
                    body.append(f"    std::optional<{gv}> {acc}() const {{ auto g = c_.Groups({path}); if (g.empty()) return std::nullopt; return {gv}(g.front()); }}")
                    body.append(f"    {gv} Ensure{acc}() const {{ return {gv}(c_.EnsureGroup({path})); }}")
                continue
            comment = m.sig + (" (repeated)" if m.repeat else "")
            value_accessors(body, acc, m.type, path, m.repeat, "c_", comment)
            if schema.types[m.type].kind == "Struct":
                sv = q(schema.types[m.type].name)
                deferred.append(f"inline {sv} {cls}::{acc}() const {{ "
                                f"return {sv}(::esper::StructCursor{{ c_, {path} }}); }}")
            body.append(f"    void Clear{acc}() const {{ c_.Clear({path}); }}")
        body += ["private:", "    ::esper::Cursor c_;", "};", ""]
        out.extend(body)

    for sig, name, root in schema.records:
        emit_scope_view(root, name, True, sig)
    out.extend(deferred)
    out.append("")
    out.append(f"}} // namespace {ns}")
    return "\n".join(out) + "\n"


def emit_json(schema: Schema) -> str:
    def type_json(tidx: int):
        t = schema.types[tidx]
        if t.kind == "Struct":
            fs = []
            for name, ft, brk in t.struct_fields:
                fs.append({"break": True} if brk else {"name": name, "type": type_json(ft)})
            d = {"kind": "struct", "fields": fs}
            if t.name:
                d["name"] = t.name
            return d
        if t.kind == "Array":
            d = {"kind": "array", "element": type_json(t.ref)}
            if t.len == "Fixed":
                d["count"] = t.count
            elif t.len.startswith("Prefix"):
                d["countPrefix"] = {"Prefix8": "u8", "Prefix16": "u16", "Prefix32": "u32"}[t.len]
            return d
        base = t.spec.split(":")[0].split("->")[0]
        d = {"kind": base}
        if t.kind == "LString":
            d["table"] = t.table.lower()
        if t.kind == "Bytes":
            if t.len == "Fixed":
                d["size"] = t.count
            if t.flags & 1:
                d["formIds"] = "none"
        if t.links:
            d["to"] = list(t.links)
        return d

    def scope_json(gidx: int):
        g = schema.groups[gidx]
        ms = []
        for m in g.members:
            if m.is_group:
                d = {"name": m.name, "group": schema.groups[m.group].name,
                     "members": scope_json(m.group)}
            else:
                d = {"name": m.name, "sig": m.sig, "type": type_json(m.type)}
                if isinstance(m.count_of, int):
                    d["countOf"] = g.members[m.count_of].name
            if m.repeat:
                d["repeat"] = True
            if m.optional:
                d["optional"] = True
            ms.append(d)
        return ms

    doc = {
        "schemaVersion": schema.manifest["schemaVersion"],
        "languageVersion": LANGUAGE_VERSION,
        "game": "Skyrim Special Edition",
        "records": [{"sig": sig, "name": name, "members": scope_json(root)}
                    for sig, name, root in schema.records],
    }
    return json.dumps(doc, indent=2) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--schema", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path, help="directory for Tables.h/Fields.h/Views.h")
    ap.add_argument("--json", type=Path, help="path for the schema.json bundle")
    ap.add_argument("--tables-ns", default="esper::schema::tables")
    ap.add_argument("--fields-ns", default="esper::fields")
    ap.add_argument("--views-ns", default="esper::view")
    ap.add_argument("--check", action="store_true", help="verify outputs are up to date")
    args = ap.parse_args(argv)

    schema = Schema()
    try:
        schema.load(args.schema)
        schema.build()
    except SchemaError as e:
        print(f"schemagen: error: {e}", file=sys.stderr)
        return 2

    try:
        outputs = {
            args.out / "Tables.h": emit_tables(schema, args.tables_ns),
            args.out / "Fields.h": emit_fields(schema, args.fields_ns),
            args.out / "Views.h": emit_views(schema, args.views_ns),
        }
        if args.json:
            outputs[args.json] = emit_json(schema)
    except SchemaError as e:
        print(f"schemagen: error: {e}", file=sys.stderr)
        return 2

    stale = []
    for path, text in outputs.items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == text:
            continue
        if args.check:
            stale.append(path)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"schemagen: wrote {path}")
    if stale:
        for p in stale:
            print(f"schemagen: stale: {p}", file=sys.stderr)
        print("schemagen: run the esper-schemagen target and commit the result", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
