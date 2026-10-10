#!/usr/bin/env python3
"""schemagen — compile ESPER's YAML schema into schema.json + C++ headers.

    schemagen.py --schema schema --out include/ESPER/generated --json schema.json
    schemagen.py ... --check        # exit 1 if any output is stale

Emits, into --out:
    Tables.h   constexpr tables the engine interprets (esper::schema::SchemaSet)
    Views.h    typed views (thin accessors over esper::Cursor)

The descriptor language is specified in docs/schema-language.md. This file is
the reference implementation of its validation rules: anything it accepts the
engine can interpret, and anything outside the current language version is
rejected by name.
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

LANGUAGE_VERSION = "0.5"

SCALAR_KINDS = {
    "u8": ("U8", "uint8_t"), "u16": ("U16", "uint16_t"), "u32": ("U32", "uint32_t"),
    "u64": ("U64", "uint64_t"), "i8": ("I8", "int8_t"), "i16": ("I16", "int16_t"),
    "i32": ("I32", "int32_t"), "i64": ("I64", "int64_t"), "f32": ("F32", "float"),
    "formid": ("FormId", "::esper::FormId"), "sig4": ("Sig4", "uint32_t"),
    "zstring": ("ZString", "std::string"), "wstring": ("WString", "std::string"),
    "lstring": ("LString", "::esper::LocalizedString"),
    "bytes": ("Bytes", "std::vector<uint8_t>"),
    "marker": ("Bytes", "std::vector<uint8_t>"),   # zero-size, presence-only
}
FIXED_SIZE = {"marker": 0, "u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4, "f32": 4,
              "formid": 4, "sig4": 4, "u64": 8, "i64": 8}
INTEGER_KINDS = {"U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "FormId", "Sig4"}
STR_TABLES = {"strings": "Strings", "dlstrings": "DLStrings", "ilstrings": "ILStrings"}
FLAG_FORMIDS_NONE = 0x01
FLAG_REMAP_UNSAFE = 0x02

# Constructs specified by the design but not in this language version: reject by name.
NOT_YET = {"enum": "enum hints", "flags": "flag hints", "unit": "unit hints"}
# Options that shape a value's type (allowed wherever a value is described).
VALUE_OPTS = {"type", "fields", "union", "if", "table", "size", "formIds", "to", "remapUnsafe"}

RESERVED = {"record", "cursor", "self"}


class SchemaError(Exception):
    pass


def fail(where: str, msg: str):
    raise SchemaError(f"{where}: {msg}")


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
    if name in RESERVED:
        fail(where, f"name {name!r} is reserved")
    return name


def check_keys(where: str, d: dict, allowed: set[str]):
    for k in d:
        if k in NOT_YET:
            fail(where, f"'{k}' ({NOT_YET[k]}) is not supported in language {LANGUAGE_VERSION}")
        if k not in allowed:
            fail(where, f"unknown key '{k}' (allowed: {', '.join(sorted(allowed))})")


def as_int(where: str, v) -> int:
    if isinstance(v, bool) or not isinstance(v, int):
        fail(where, f"expected an integer, got {v!r}")
    return v


# ── Model ─────────────────────────────────────────────────────────────────────

@dataclass
class Case:
    name: str | None
    type: int | None            # None = empty
    default: bool = False
    values: list = field(default_factory=list)   # ints (already resolved)
    range: tuple | None = None                   # (min, max)
    spec_when: object = None                     # as written, for json


@dataclass
class TypeNode:
    kind: str                  # C++ Kind enumerator
    spec: str                  # canonical spelling, for dedup + json
    len: str = "None"
    table: str = "None"
    flags: int = 0
    count: int = 0
    ref: int = 0               # Array: element type
    links: tuple = ()
    name: str | None = None    # named struct/union (view type)
    fixed_size: int | None = None
    has_break: bool = False
    rest: bool = False         # consumes the rest of the subrecord
    cpp: str = ""              # value type for handles
    struct_fields: list = field(default_factory=list)   # (name|None, type idx, is_break)
    # union
    selector: str = "None"
    sel_field: str | None = None
    sel_mask: int = 0
    lookup: int = 0
    sel_json: object = None
    cases: list = field(default_factory=list)
    is_if: bool = False        # `if:` sugar: one case + empty default


@dataclass
class Member:
    name: str
    is_group: bool
    sig: str = ""
    type: int = 0
    group: int = 0
    repeat: bool = False
    optional: bool = False
    count_of: object = None


@dataclass
class Group:
    name: str                  # view type name
    members: list
    ordered: bool
    entries: list = field(default_factory=list)   # (sig, member index)
    open_sig: str | None = None                   # delimited: the opening sig (first member)
    end_sig: str | None = None                    # delimited: the closing sig (last member)


class Schema:
    def __init__(self):
        self.types: list[TypeNode] = []
        self.type_index: dict[str, int] = {}
        self.groups: list[Group] = []
        self.records: list[tuple[str, str, int]] = []   # (sig, name, root group)
        self.struct_defs: dict[str, dict] = {}
        self.group_defs: dict[str, dict] = {}
        self.table_defs: dict[str, dict] = {}
        self.record_defs: list[tuple[str, dict]] = []
        self.manifest: dict = {}
        self.named: dict[str, int] = {}          # named struct/union → type index
        self.building: set[str] = set()
        self.scopes: list[list[str]] = []        # field names visible to selectors
        self.all_field_names: set[str] = set()
        self.lookups: list[tuple[str, str]] = [] # (table, column)
        self.value_ids: dict[str, int] = {}      # interned lookup values
        self.pending_field_refs: list[tuple[str, str]] = []
        self.pending_member_refs: list[tuple[str, str]] = []
        self.all_member_names: set[str] = set()

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
                kinds = [k for k in ("record", "struct", "group", "manifest", "table", "enum") if k in doc]
                if len(kinds) != 1:
                    fail(rel, "a descriptor must have exactly one of record/struct/group/table/manifest")
                kind = kinds[0]
                if kind == "enum":
                    fail(rel, f"'enum' descriptors are not supported in language {LANGUAGE_VERSION}")
                if kind == "manifest":
                    if self.manifest:
                        fail(rel, "more than one manifest")
                    check_keys(rel, doc, {"manifest", "schemaVersion"})
                    self.manifest = doc
                elif kind in ("struct", "group", "table"):
                    name = doc[kind]
                    pattern = r"[a-z][A-Za-z0-9]*" if kind == "table" else r"[A-Z][A-Za-z0-9]*"
                    if not isinstance(name, str) or not re.fullmatch(pattern, name):
                        fail(rel, f"{kind} name {name!r} must be "
                                  f"{'camelCase' if kind == 'table' else 'PascalCase'}")
                    store = {"struct": self.struct_defs, "group": self.group_defs,
                             "table": self.table_defs}[kind]
                    if name in store:
                        fail(rel, f"{kind} {name} defined twice")
                    store[name] = dict(doc, _where=rel)
                else:
                    check_sig(rel, doc["record"])
                    self.record_defs.append((rel, doc))
        if not self.manifest or not isinstance(self.manifest.get("schemaVersion"), str):
            fail(str(root), "a manifest with schemaVersion: \"x.y.z\" is required")
        for name, t in self.table_defs.items():
            self.check_table(name, t)

    def check_table(self, name: str, t: dict):
        where = t["_where"]
        check_keys(where, t, {"table", "key", "columns", "rows", "_where"})
        cols = t.get("columns")
        if not isinstance(cols, list) or not cols or t.get("key") not in cols:
            fail(where, "a table needs columns: [...] including its key column")
        rows = t.get("rows")
        if not isinstance(rows, list) or not rows:
            fail(where, "a table needs rows")
        ki = cols.index(t["key"])
        keys = set()
        for i, r in enumerate(rows):
            if not isinstance(r, list) or len(r) != len(cols):
                fail(where, f"row {i} must have {len(cols)} values")
            k = as_int(f"{where} row {i}", r[ki])
            if k in keys:
                fail(where, f"duplicate key {k}")
            keys.add(k)

    # ── types ────────────────────────────────────────────────────────────────

    def intern(self, node: TypeNode, unique: bool = False) -> int:
        key = f"{node.spec}|{node.len}|{node.table}|{node.flags}|{node.links}"
        if not unique and key in self.type_index:
            return self.type_index[key]
        self.types.append(node)
        idx = len(self.types) - 1
        if not unique:
            self.type_index[key] = idx
        return idx

    def with_flags(self, tidx: int, flags: int) -> int:
        t = self.types[tidx]
        if t.flags & flags == flags:
            return tidx
        if t.kind in ("Struct", "Union"):
            fail("types", "remapUnsafe applies to scalar, string, bytes or array types")
        clone = TypeNode(**{**t.__dict__, "flags": t.flags | flags, "struct_fields": list(t.struct_fields),
                            "cases": list(t.cases)})
        return self.intern(clone)

    def scalar(self, where: str, base: str, opts: dict) -> int:
        kind, cpp = SCALAR_KINDS[base]
        node = TypeNode(kind=kind, spec=base, cpp=cpp, fixed_size=FIXED_SIZE.get(base))
        if base == "marker":
            for k in ("size", "formIds"):
                if k in opts:
                    fail(where, f"'{k}' doesn't apply to marker")
            node.len, node.count, node.flags = "Fixed", 0, FLAG_FORMIDS_NONE
            return self.intern(node)
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
                node.len, node.rest = "Rest", True
            fids = opts.get("formIds")
            if fids not in (None, "none"):
                fail(where, f"formIds may only be 'none', got {fids!r}")
            if fids == "none":
                node.flags = FLAG_FORMIDS_NONE
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
        if et.rest:
            fail(where, "an array element can't consume the rest of the subrecord")
        node = TypeNode(kind="Array", spec="", ref=elem, cpp=f"std::vector<{et.cpp}>")
        if arr == "":
            node.len, node.rest = "Rest", True
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
        check_keys(where, d, {"struct", "size", "fields", "union", "_where"})
        if ("fields" in d) == ("union" in d):
            fail(where, f"struct {name} needs exactly one of 'fields' or 'union'")
        if "union" in d:
            idx = self.build_union(where, d["union"], name, named=True)
        else:
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

    def value_type(self, where: str, d: dict, inline_name: str) -> int:
        """The type a field / member / case describes (type, fields or union; +if, +remapUnsafe)."""
        shapes = [k for k in ("type", "fields", "union") if k in d]
        if len(shapes) != 1:
            fail(where, "describe a value with exactly one of 'type', 'fields' or 'union'")
        if "union" in d:
            t = self.build_union(where, d["union"], inline_name)
        elif "fields" in d:
            t = self.build_struct(where, d["fields"], spec=f"inline:{inline_name}", name=inline_name)
        else:
            t = self.resolve_type(where, d["type"], d)
        if d.get("remapUnsafe"):
            if d["remapUnsafe"] is not True:
                fail(where, "remapUnsafe must be 'true'")
            t = self.with_flags(t, FLAG_REMAP_UNSAFE)
        if "if" in d:
            t = self.wrap_if(where, d["if"], t, inline_name)
        return t

    def build_struct(self, where: str, fields, spec: str, name: str | None) -> int:
        if not isinstance(fields, list) or not fields:
            fail(where, "a struct needs a non-empty fields list")
        node = TypeNode(kind="Struct", spec=spec, name=name)
        total, seen, open_ended = 0, set(), False
        self.scopes.append([])
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
            check_keys(fw, f, {"name"} | VALUE_OPTS)
            fname = check_name(fw, f.get("name"))
            if fname in seen:
                fail(fw, f"duplicate field name {fname}")
            seen.add(fname)
            if open_ended:
                fail(fw, "only the last field may be variable-length (rest arrays/bytes)")
            ftype = self.value_type(fw, f, (name or "Anon") + pascal(fname))
            ft = self.types[ftype]
            if ft.has_break:
                fail(fw, "a struct with a break can only be a subrecord's top-level type")
            if ft.rest:
                open_ended = True
            node.struct_fields.append((fname, ftype, False))
            self.scopes[-1].append(fname)
            self.all_field_names.add(fname)
            total = None if (total is None or ft.fixed_size is None) else total + ft.fixed_size
        self.scopes.pop()
        node.fixed_size = None if node.has_break else total
        node.rest = open_ended
        node.cpp = "::esper::Value"
        return self.intern(node, unique=name is not None and spec.startswith("inline:"))

    # ── unions ───────────────────────────────────────────────────────────────

    def field_ref(self, where: str, fname):
        if not isinstance(fname, str):
            fail(where, f"a selector field must be a field name, got {fname!r}")
        visible = any(fname in scope for scope in self.scopes)
        if not visible:
            # Named structs may select on a field of the struct that contains
            # them (VMAD's objFormat); resolved at runtime by name, checked to
            # exist somewhere in the schema after the build.
            self.pending_field_refs.append((where, fname))
        return fname

    def build_union(self, where: str, u, name: str, named: bool = False) -> int:
        if not isinstance(u, dict):
            fail(where, "union must be a mapping with 'by' and 'cases'")
        check_keys(where, u, {"by", "cases", "default"})
        node = TypeNode(kind="Union", spec=f"union:{where}:{name}", name=name if named else None,
                        cpp="::esper::Value")
        by = u.get("by")
        if by in ("size", "formVersion", "record"):
            node.selector = {"size": "Size", "formVersion": "FormVersion", "record": "Record"}[by]
            node.sel_json = by
        elif isinstance(by, dict) and "field" in by:
            check_keys(where, by, {"field", "mask"})
            node.selector = "Field"
            node.sel_field = self.field_ref(where, by["field"])
            node.sel_mask = as_int(where, by.get("mask", 0))
            node.sel_json = dict(by)
        elif isinstance(by, dict) and "member" in by:
            check_keys(where, by, {"member"})
            if not isinstance(by["member"], str):
                fail(where, "a member selector names a subrecord member")
            node.selector = "Member"
            node.sel_field = by["member"]
            node.sel_json = dict(by)
            self.pending_member_refs.append((where, by["member"]))
        elif isinstance(by, dict) and "lookup" in by:
            check_keys(where, by, {"lookup"})
            lk = by["lookup"]
            if not isinstance(lk, dict):
                fail(where, "lookup must be { table, key, column }")
            check_keys(where, lk, {"table", "key", "column"})
            table = lk.get("table")
            if table not in self.table_defs:
                fail(where, f"unknown table {table!r}")
            cols = self.table_defs[table]["columns"]
            if lk.get("column") not in cols or lk.get("column") == self.table_defs[table]["key"]:
                fail(where, f"table {table} has no value column {lk.get('column')!r}")
            node.selector = "Lookup"
            node.sel_field = self.field_ref(where, lk.get("key"))
            pair = (table, lk["column"])
            if pair not in self.lookups:
                self.lookups.append(pair)
            node.lookup = self.lookups.index(pair)
            node.sel_json = {"lookup": dict(lk)}
        else:
            fail(where, "union 'by' must be size, formVersion, record, { field[, mask] }, "
                        "{ member } or { lookup: { table, key, column } }")

        cases = u.get("cases")
        if not isinstance(cases, list):
            fail(where, "union needs a cases list (it may be empty when a default is given)")
        names = set()
        for i, c in enumerate(cases):
            cw = f"{where} case {i}"
            if not isinstance(c, dict) or "when" not in c:
                fail(cw, "a case needs 'when'")
            check_keys(cw, c, {"when", "name"} | VALUE_OPTS)
            case = Case(name=None, type=None, spec_when=c["when"])
            if "name" in c:
                case.name = check_name(cw, c["name"])
                if case.name in names:
                    fail(cw, f"duplicate case name {case.name}")
                names.add(case.name)
            self.case_when(cw, node, c["when"], case)
            if any(k in c for k in ("type", "fields", "union")):
                case.type = self.value_type(cw, c, name + pascal(case.name or f"case{i}"))
            node.cases.append(case)
        if "default" in u:
            d = u["default"] or {}
            if not isinstance(d, dict):
                fail(where, "default must be a mapping (empty for no payload)")
            check_keys(f"{where} default", d, {"name"} | VALUE_OPTS)
            case = Case(name=None, type=None, default=True)
            if "name" in d:
                case.name = check_name(f"{where} default", d["name"])
            if any(k in d for k in ("type", "fields", "union")):
                case.type = self.value_type(f"{where} default", d, name + pascal(case.name or "default"))
            node.cases.append(case)
        if not node.cases:
            fail(where, "a union needs at least one case or a default")
        payloads = [self.types[c.type] if c.type is not None else None for c in node.cases]
        sizes = {p.fixed_size if p else 0 for p in payloads}
        node.fixed_size = sizes.pop() if len(sizes) == 1 and None not in sizes else None
        node.rest = any(p and p.rest for p in payloads)
        node.has_break = any(p and p.has_break for p in payloads)
        return self.intern(node, unique=True)

    def case_when(self, where: str, node: TypeNode, when, case: Case):
        if node.selector == "Record":
            items = when if isinstance(when, list) else [when]
            case.values = [sig_int(check_sig(where, s)) for s in items]
            return
        if node.selector == "Lookup":
            items = when if isinstance(when, list) else [when]
            table, column = self.lookups[node.lookup]
            allowed = self.column_values(table, column)
            for s in items:
                if not isinstance(s, str) or s not in allowed:
                    fail(where, f"{s!r} is not a value of {table}.{column} "
                                f"(values: {', '.join(sorted(allowed))})")
            case.values = [self.value_id(s) for s in items]
            return
        if isinstance(when, dict):
            check_keys(where, when, {"min", "max"})
            lo = as_int(where, when.get("min", -(2 ** 63)))
            hi = as_int(where, when.get("max", 2 ** 63 - 1))
            if lo > hi:
                fail(where, "min > max")
            case.range = (lo, hi)
            return
        items = when if isinstance(when, list) else [when]
        case.values = [as_int(where, v) & 0xFFFFFFFFFFFFFFFF for v in items]

    def wrap_if(self, where: str, cond, tidx: int, name: str) -> int:
        if not isinstance(cond, dict) or "field" not in cond:
            fail(where, "if needs { field, ... } with min/max, equals or in")
        check_keys(where, cond, {"field", "mask", "min", "max", "equals", "in"})
        node = TypeNode(kind="Union", spec=f"if:{where}:{name}", cpp="::esper::Value", is_if=True)
        node.selector = "Field"
        node.sel_field = self.field_ref(where, cond["field"])
        node.sel_mask = as_int(where, cond.get("mask", 0))
        node.sel_json = {"field": cond["field"], **({"mask": cond["mask"]} if "mask" in cond else {})}
        case = Case(name=None, type=tidx, spec_when={k: v for k, v in cond.items() if k not in ("field", "mask")})
        if "equals" in cond:
            case.values = [as_int(where, cond["equals"])]
        elif "in" in cond:
            if not isinstance(cond["in"], list):
                fail(where, "'in' must be a list")
            case.values = [as_int(where, v) for v in cond["in"]]
        elif "min" in cond or "max" in cond:
            case.range = (as_int(where, cond.get("min", -(2 ** 63))),
                          as_int(where, cond.get("max", 2 ** 63 - 1)))
        else:
            fail(where, "if needs min/max, equals or in")
        node.cases = [case, Case(name=None, type=None, default=True)]
        t = self.types[tidx]
        node.rest = t.rest
        node.has_break = t.has_break
        return self.intern(node, unique=True)

    def column_values(self, table: str, column: str) -> set[str]:
        t = self.table_defs[table]
        ci = t["columns"].index(column)
        return {str(r[ci]) for r in t["rows"]}

    def value_id(self, s: str) -> int:
        if s not in self.value_ids:
            self.value_ids[s] = len(self.value_ids) + 1
        return self.value_ids[s]

    # ── groups ───────────────────────────────────────────────────────────────

    def build_group(self, where: str, members, view_name: str, ordered: bool,
                    remap: dict[str, str], open_sig=None, end_sig=None) -> int:
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
                    check_keys(gd["_where"], gd, {"group", "members", "open", "end", "_where"})
                    mname = check_name(mw, m.get("name"))
                    sigs = m.get("sigs", {}) or {}
                    if not isinstance(sigs, dict):
                        fail(mw, "'sigs' must map original signatures to new ones")
                    sub = {check_sig(mw, k): check_sig(mw, v) for k, v in sigs.items()}
                    used = self.group_sigs(gd["members"])
                    for k in sub:
                        if k not in used:
                            fail(mw, f"sigs remaps {k}, which group {gname} doesn't use")
                    merged = {}
                    for s0 in used:          # the use's own remap first, then any enclosing one
                        s1 = sub.get(s0, s0)
                        merged[s0] = remap.get(s1, s1)
                    child = self.build_group(gd["_where"], gd["members"], gname, True, merged,
                                             gd.get("open"), gd.get("end"))
                else:
                    check_keys(mw, m, {"group", "members", "repeat", "optional", "open", "end"})
                    mname = check_name(mw, m.get("group"))
                    child = self.build_group(mw, m.get("members"), view_name + pascal(mname),
                                             True, remap, m.get("open"), m.get("end"))
                member = Member(name=mname, is_group=True, group=child,
                                repeat=bool(m.get("repeat", False)),
                                optional=bool(m.get("optional", False)))
            else:
                check_keys(mw, m, {"sig", "name", "repeat", "optional", "countOf"} | VALUE_OPTS)
                sig = check_sig(mw, m.get("sig"))
                sig = remap.get(sig, sig)
                mname = check_name(mw, m.get("name"))
                tidx = self.value_type(mw, m, view_name + pascal(mname))
                self.all_member_names.add(mname)
                member = Member(name=mname, is_group=False, sig=sig, type=tidx,
                                repeat=bool(m.get("repeat", False)),
                                optional=bool(m.get("optional", False)),
                                count_of=m.get("countOf"))
            if member.name in names:
                fail(mw, f"duplicate member name {member.name}")
            names.add(member.name)
            group.members.append(member)

        for mem in group.members:                       # derived counts
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

        if open_sig is not None or end_sig is not None:  # delimited group
            self.delimit(where, group, open_sig, end_sig, remap)

        seen: dict[str, int] = {}                        # entry signatures
        for i, mem in enumerate(group.members):
            if (group.open_sig and group.open_sig == group.end_sig and i == len(group.members) - 1
                    and not mem.is_group and mem.sig == group.end_sig):
                continue                                 # the same sig opens and ends: resolved by position
            if not mem.is_group:
                sigs = [mem.sig]
            elif self.groups[mem.group].open_sig:       # a delimited child is entered by its opener only
                sigs = [self.groups[mem.group].open_sig]
            else:
                sigs = [s for s, _ in self.groups[mem.group].entries]
            for s in sigs:
                if s in seen:
                    a, b = group.members[seen[s]].name, mem.name
                    fail(where, f"signature {s} is ambiguous in this scope ({a} vs {b})")
                seen[s] = i
                group.entries.append((s, i))
        return gidx

    def delimit(self, where: str, group: Group, open_sig, end_sig, remap: dict[str, str]):
        if open_sig is None:
            fail(where, "'end' needs 'open': a delimited group is entered by its opener")
        open_sig = remap.get(check_sig(where, open_sig), open_sig)
        first = group.members[0]
        if first.is_group or first.sig != open_sig or first.repeat:
            fail(where, f"open: {open_sig} must be the group's first member, singular")
        group.open_sig = open_sig
        group.ordered = False                       # unordered inside, like a record root
        if end_sig is not None:
            end_sig = remap.get(check_sig(where, end_sig), end_sig)
            last = group.members[-1]
            if last.is_group or last.sig != end_sig or last.repeat or len(group.members) < 2:
                fail(where, f"end: {end_sig} must be the group's last member, singular")
            group.end_sig = end_sig

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
        for name in sorted(self.struct_defs):        # every named struct, even unused
            self.named_struct(name)
        for where, fname in self.pending_field_refs:
            if fname not in self.all_field_names:
                fail(where, f"selector field {fname!r} isn't a field anywhere in the schema")
        for where, mname in self.pending_member_refs:
            if mname not in self.all_member_names:
                fail(where, f"selector member {mname!r} isn't a subrecord member anywhere in the schema")
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
            if t.kind == "Struct" and t.name:
                claim(t.name, f"struct {t.name}")
        for g in self.groups:
            if g.ordered:
                claim(g.name, f"group {g.name}")
        for g in self.groups:                        # accessor == class name → constructor clash
            for m in g.members:
                if pascal(m.name) == g.name:
                    fail("views", f"member '{m.name}' of {g.name} would generate accessor {g.name}(), "
                                  f"which clashes with the view class; rename the member or the type")
        for t in self.types:
            if t.kind == "Struct" and t.name:
                for fname, _, brk in t.struct_fields:
                    if not brk and pascal(fname) == t.name:
                        fail("views", f"field '{fname}' of struct {t.name} would generate accessor "
                                      f"{t.name}(), which clashes with the view class")


# ── Emission ──────────────────────────────────────────────────────────────────

HEADER = "// GENERATED by tools/schemagen from schema/ — do not edit. Regenerate with\n" \
         "// `cmake --build <dir> --target esper-schemagen` (or tools/schemagen/schemagen.py).\n"


def c_str(s: str | None) -> str:
    return "nullptr" if s is None else json.dumps(s)


def flatten(schema: Schema):
    """Assign flat table indices."""
    fields, links, cases, case_values = [], [], [], []
    type_rows = []
    for t in schema.types:
        row = dict(kind=t.kind, len=t.len, table=t.table, flags=t.flags, count=t.count,
                   ref=t.ref, refCount=0, linkFirst=0, linkCount=0, name=t.name,
                   selector=t.selector, selField=t.sel_field, selMask=t.sel_mask, lookup=t.lookup)
        if t.kind == "Struct":
            row["ref"] = len(fields)
            row["refCount"] = len(t.struct_fields)
            fields.extend(t.struct_fields)
        if t.kind == "Union":
            row["ref"] = len(cases)
            row["refCount"] = len(t.cases)
            for c in t.cases:
                cases.append(dict(name=c.name, type=c.type if c.type is not None else 0,
                                  empty=c.type is None, default=c.default,
                                  isRange=c.range is not None,
                                  min=c.range[0] if c.range else 0, max=c.range[1] if c.range else 0,
                                  firstValue=len(case_values), valueCount=len(c.values)))
                case_values.extend(c.values)
        if t.links:
            row["linkFirst"] = len(links)
            row["linkCount"] = len(t.links)
            links.extend(t.links)
        type_rows.append(row)
    members, entries, group_rows = [], [], []
    for g in schema.groups:
        group_rows.append(dict(name=g.name, firstMember=len(members), memberCount=len(g.members),
                               firstEntry=len(entries), entryCount=len(g.entries),
                               ordered=g.ordered, open=g.open_sig, end=g.end_sig))
        members.extend(g.members)
        entries.extend(g.entries)
    lookups, lookup_rows = [], []
    for table, column in schema.lookups:
        t = schema.table_defs[table]
        ki, ci = t["columns"].index(t["key"]), t["columns"].index(column)
        rows = sorted((int(r[ki]), schema.value_id(str(r[ci]))) for r in t["rows"])
        lookups.append(dict(name=f"{table}.{column}", firstRow=len(lookup_rows), rowCount=len(rows)))
        lookup_rows.extend(rows)
    return dict(types=type_rows, fields=fields, links=links, members=members, entries=entries,
                groups=group_rows, cases=cases, caseValues=case_values, lookups=lookups,
                lookupRows=lookup_rows)


def i64(v: int) -> str:
    if v == -(2 ** 63):
        return "INT64_MIN"
    if v == 2 ** 63 - 1:
        return "INT64_MAX"
    return f"{v}LL"


def emit_tables(schema: Schema, ns: str) -> str:
    f = flatten(schema)
    out = ["#pragma once", HEADER, "#include <ESPER/SchemaTypes.h>", "", "#include <cstdint>", "",
           f"namespace {ns} {{", "", "using namespace ::esper::schema;", ""]

    def arr(ctype: str, name: str, rows: list[str], empty: str):
        out.append(f"inline constexpr {ctype} {name}[] = {{")
        out.extend(rows if rows else [f"    {empty},"])
        out.append("};")

    arr("TypeDef", "kTypes", [
        f"    {{ Kind::{r['kind']}, LenKind::{r['len']}, StrTable::{r['table']}, {r['flags']}, "
        f"{r['count']}, {r['ref']}, {r['refCount']}, {r['linkFirst']}, {r['linkCount']}, "
        f"{c_str(r['name'])}, Selector::{r['selector']}, {c_str(r['selField'])}, "
        f"{r['selMask']}ULL, {r['lookup']} }},  // {i}: {schema.types[i].spec}"
        for i, r in enumerate(f["types"])], "{}")
    arr("FieldDef", "kFields", [f"    {{ {c_str(n)}, {t}, {'true' if b else 'false'} }},"
                                for n, t, b in f["fields"]], "{ nullptr, 0, false }")
    out.append("inline constexpr uint32_t kLinks[] = {")
    out.append("    " + (", ".join(f"SigOf(\"{s}\")" for s in f["links"]) if f["links"] else "0") + ",")
    out.append("};")
    arr("MemberDef", "kMembers", [
        f"    {{ {c_str(m.name)}, {f'SigOf(\"{m.sig}\")' if m.sig else '0'}, {m.type}, {m.group}, "
        f"{'true' if m.is_group else 'false'}, {'true' if m.repeat else 'false'}, "
        f"{m.count_of if isinstance(m.count_of, int) else -1} }},"
        for m in f["members"]], "{}")
    arr("EntryDef", "kEntries", [f"    {{ SigOf(\"{s}\"), {mi} }}," for s, mi in f["entries"]], "{}")
    def sig_or_zero(sig):
        return f'SigOf("{sig}")' if sig else "0"

    arr("GroupDef", "kGroups", [
        f"    {{ {c_str(g['name'])}, {g['firstMember']}, {g['memberCount']}, {g['firstEntry']}, "
        f"{g['entryCount']}, {'true' if g['ordered'] else 'false'}, "
        f"{sig_or_zero(g['open'])}, {sig_or_zero(g['end'])} }},  // {i}"
        for i, g in enumerate(f["groups"])], "{}")
    arr("RecordDef", "kRecords", [f"    {{ SigOf(\"{sig}\"), {c_str(name)}, {root} }},"
                                  for sig, name, root in schema.records], "{}")
    arr("CaseDef", "kCases", [
        f"    {{ {c_str(c['name'])}, {c['type']}, {'true' if c['empty'] else 'false'}, "
        f"{'true' if c['default'] else 'false'}, {'true' if c['isRange'] else 'false'}, "
        f"{i64(c['min'])}, {i64(c['max'])}, {c['firstValue']}, {c['valueCount']} }},"
        for c in f["cases"]], "{}")
    out.append("inline constexpr uint64_t kCaseValues[] = {")
    out.append("    " + (", ".join(f"{v}ULL" for v in f["caseValues"]) if f["caseValues"] else "0") + ",")
    out.append("};")
    arr("LookupDef", "kLookups", [f"    {{ {c_str(l['name'])}, {l['firstRow']}, {l['rowCount']} }},"
                                  for l in f["lookups"]], "{}")
    rows = f["lookupRows"]
    out.append("inline constexpr LookupRow kLookupRows[] = {")
    if rows:
        for k in range(0, len(rows), 8):
            out.append("    " + " ".join(f"{{ {a}, {b} }}," for a, b in rows[k:k + 8]))
    else:
        out.append("    {},")
    out.append("};")
    out.append("")
    out.append("inline constexpr SchemaSet kSchema{")
    out.append("    kTypes,")
    out.append(f"    std::span<const FieldDef>(kFields, {len(f['fields'])}),")
    out.append(f"    std::span<const uint32_t>(kLinks, {len(f['links'])}),")
    out.append("    kMembers, kEntries, kGroups, kRecords,")
    out.append(f"    std::span<const CaseDef>(kCases, {len(f['cases'])}),")
    out.append(f"    std::span<const uint64_t>(kCaseValues, {len(f['caseValues'])}),")
    out.append(f"    std::span<const LookupDef>(kLookups, {len(f['lookups'])}),")
    out.append(f"    std::span<const LookupRow>(kLookupRows, {len(rows)}),")
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


def case_label(c: Case, i: int) -> str:
    return c.name or ("default" if c.default else f"case{i}")


def emit_views(schema: Schema, ns: str) -> str:
    """Views are accessors only: every call goes through esper::Cursor."""
    by_sig = {sig: name for sig, name, _ in schema.records}

    def q(name: str) -> str:                     # accessors may share a view type's name
        return f"::{ns}::{name}"

    out = ["#pragma once", HEADER, "#include <ESPER/Record.h>", "",
           "#include <cstdint>", "#include <optional>", "#include <string>", "#include <vector>", "",
           f"namespace {ns} {{", ""]
    struct_types = [i for i, t in enumerate(schema.types) if t.kind == "Struct" and t.name]
    names = sorted({name for _, name, _ in schema.records} |
                   {schema.types[i].name for i in struct_types} |
                   {g.name for g in schema.groups if g.ordered})
    for n in names:
        out.append(f"class {n};")
    out.append("")

    def link_view(t: TypeNode) -> str | None:
        if t.kind == "FormId" and len(t.links) == 1 and t.links[0] in by_sig:
            return q(by_sig[t.links[0]])
        return None

    def vt_of(t: TypeNode) -> str:
        lv = link_view(t)
        if lv:
            return f"::esper::Link<{lv}>"
        if t.kind == "Array":
            elv = link_view(schema.types[t.ref])
            if elv:
                return f"std::vector<::esper::Link<{elv}>>"
        return t.cpp

    deferred: list[str] = []
    emitted: set[str] = set()

    def path_expr(kind: str, base, extra: list[int]) -> str:
        """kind 'lit': base = (idx list, member depth); 'struct': base = value index in s_."""
        if kind == "lit":
            idx, md = base
            return f"::esper::RelPath{relpath_init(idx + extra, md)}"
        e = f"s_.Field({base})"
        for x in extra:
            e = f"::esper::Extend({e}, {x})"
        return e

    def accessors(body: list[str], cls: str, acc: str, tidx: int, kind: str, base, cursor: str,
                  comment: str | None, repeat: bool = False, extra: list[int] | None = None):
        """Accessors for one value (member or struct field), recursing through unions."""
        extra = extra or []
        t = schema.types[tidx]
        pe = path_expr(kind, base, extra)
        if comment:
            body.append(f"    // {comment}")
        if t.kind == "Union" and t.is_if and not repeat:
            accessors(body, cls, acc, t.cases[0].type, kind, base, cursor, None, False, extra + [0])
            return
        if t.kind == "Union" and not repeat:
            # Which case was decoded: a named enum (switch) plus Is<Case>() (if).
            labels = [pascal(case_label(c, i)) for i, c in enumerate(t.cases)]
            body.append(f"    enum class {acc}Kind : uint16_t {{ " +
                        ", ".join(f"{lab} = {i}" for i, lab in enumerate(labels)) + " };")
            body.append(f"    std::optional<{acc}Kind> {acc}Case() const {{ auto v = {cursor}.Get<::esper::Value>({pe}); "
                        f"if (!v) return std::nullopt; return static_cast<{acc}Kind>(v->bits); }}")
            for i, c in enumerate(t.cases):
                body.append(f"    bool {acc}Is{labels[i]}() const {{ return {acc}Case() == {acc}Kind::{labels[i]}; }}")
                if c.name and c.type is not None:
                    accessors(body, cls, acc + pascal(c.name), c.type, kind, base, cursor, None, False, extra + [i])
            return
        if t.spec == "marker" and not repeat:
            body.append(f"    bool {acc}() const {{ return {cursor}.Has({pe}); }}")
            body.append(f"    void Set{acc}(bool on) const {{ if (on) {cursor}.Set<std::vector<uint8_t>>({pe}, {{}}); else {cursor}.Clear({pe}); }}")
            return
        if t.kind == "Struct":
            sv = q(t.name)
            if kind == "lit":
                body.append(f"    {sv} {acc}() const;")
                deferred.append(f"inline {sv} {cls}::{acc}() const {{ return {sv}(::esper::StructCursor{{ c_, {pe} }}); }}")
            else:
                body.append(f"    {sv} {acc}() const {{ return {sv}(::esper::StructCursor{{ s_.at, {pe} }}); }}")
            if kind == "lit" and not extra:
                body.append(f"    bool Has{acc}() const {{ return {cursor}.Has({pe}); }}")
            return
        if t.kind == "Union":                         # repeated union member: generic values
            body.append(f"    std::vector<::esper::Value> {acc}() const {{ return {cursor}.GetAll<::esper::Value>({pe}); }}")
            return
        vt = vt_of(t)
        if repeat:
            body.append(f"    std::vector<{vt}> {acc}() const {{ return {cursor}.GetAll<{vt}>({pe}); }}")
            body.append(f"    void Set{acc}(const std::vector<{vt}>& v) const {{ {cursor}.SetAll<{vt}>({pe}, v); }}")
            return
        body.append(f"    std::optional<{vt}> {acc}() const {{ return {cursor}.Get<{vt}>({pe}); }}")
        body.append(f"    void Set{acc}(const {vt}& v) const {{ {cursor}.Set<{vt}>({pe}, v); }}")

    def emit_struct_view(tidx: int):
        t = schema.types[tidx]
        if t.name in emitted:
            return
        emitted.add(t.name)
        for _, ft, _ in value_fields(schema, tidx):
            for dep in struct_deps(ft):
                emit_struct_view(dep)
        body = [f"class {t.name} {{", "public:",
                f"    explicit {t.name}(::esper::StructCursor s) : s_(s) {{}}",
                "    bool IsPresent() const { return s_.Has(); }"]
        for fname, ft, vi in value_fields(schema, tidx):
            accessors(body, t.name, pascal(fname), ft, "struct", vi, "s_.at", None)
        body += ["private:", "    ::esper::StructCursor s_;", "};", ""]
        out.extend(body)

    def struct_deps(tidx: int) -> list[int]:
        """Named structs reachable without crossing another struct (through unions/ifs)."""
        t = schema.types[tidx]
        if t.kind == "Struct" and t.name:
            return [tidx]
        if t.kind == "Union":
            return [d for c in t.cases if c.type is not None for d in struct_deps(c.type)]
        return []

    for tidx in struct_types:
        emit_struct_view(tidx)

    def emit_scope_view(gidx: int, cls: str, is_record: bool, sig: str | None):
        g = schema.groups[gidx]
        for m in g.members:
            if m.is_group and schema.groups[m.group].name not in emitted:
                emitted.add(schema.groups[m.group].name)
                emit_scope_view(m.group, schema.groups[m.group].name, False, None)
        body = [f"class {cls} {{", "public:"]
        if is_record:
            body.append(f"    static constexpr uint32_t kSig = ::esper::schema::SigOf(\"{sig}\");")
        # Record views are made with As<View>(record), which checks the type;
        # a Cursor is already a position in a record of the right type.
        body.append(f"    explicit {cls}(::esper::Cursor c) : c_(c) {{}}")
        body.append("    ::esper::Record& record() const { return *c_.rec; }")
        body.append("    ::esper::Cursor cursor() const { return c_; }")
        for i, m in enumerate(g.members):
            acc = pascal(m.name)
            lit = f"::esper::RelPath{relpath_init([i], 1)}"
            if m.is_group:
                gv = q(schema.groups[m.group].name)
                body.append(f"    // group {m.name}")
                if m.repeat:
                    body.append(f"    std::vector<{gv}> {acc}() const {{ std::vector<{gv}> o; for (auto c : c_.Groups({lit})) o.emplace_back(c); return o; }}")
                    body.append(f"    {gv} Add{acc}() const {{ return {gv}(c_.AddGroup({lit})); }}")
                else:
                    body.append(f"    std::optional<{gv}> {acc}() const {{ auto g = c_.Groups({lit}); if (g.empty()) return std::nullopt; return {gv}(g.front()); }}")
                    body.append(f"    {gv} Ensure{acc}() const {{ return {gv}(c_.EnsureGroup({lit})); }}")
                continue
            accessors(body, cls, acc, m.type, "lit", ([i], 1), "c_",
                      m.sig + (" (repeated)" if m.repeat else ""), m.repeat)
            body.append(f"    void Clear{acc}() const {{ c_.Clear({lit}); }}")
        body += ["private:", "    ::esper::Cursor c_;", "};", ""]
        out.extend(body)

    for sig, name, root in schema.records:
        emit_scope_view(root, name, True, sig)
    out.extend(deferred)
    out += ["",
            "// The typed view of a record, or nullopt when it's another record type:",
            "//   if (auto armor = As<Armor>(rec)) armor->SetEditorId(\"X\");",
            "template <class View>",
            "std::optional<View> As(::esper::Record& r) {",
            "    if (r.Sig() != View::kSig) return std::nullopt;",
            "    return View(::esper::Cursor{ &r, 0 });",
            "}"]
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
        if t.kind == "Union":
            cases = []
            for c in t.cases:
                cd = {}
                if c.default:
                    cd["default"] = True
                else:
                    cd["when"] = c.spec_when
                if c.name:
                    cd["name"] = c.name
                if c.type is not None:
                    cd["type"] = type_json(c.type)
                cases.append(cd)
            d = {"kind": "union", "by": t.sel_json, "cases": cases}
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
        if base == "marker":
            return d
        if t.kind == "LString":
            d["table"] = t.table.lower()
        if t.kind == "Bytes":
            if t.len == "Fixed":
                d["size"] = t.count
            if t.flags & FLAG_FORMIDS_NONE:
                d["formIds"] = "none"
        if t.flags & FLAG_REMAP_UNSAFE:
            d["remapUnsafe"] = True
        if t.links:
            d["to"] = list(t.links)
        return d

    def scope_json(gidx: int):
        g = schema.groups[gidx]
        ms = []
        for m in g.members:
            if m.is_group:
                cg = schema.groups[m.group]
                d = {"name": m.name, "group": cg.name, "members": scope_json(m.group)}
                if cg.open_sig:
                    d["open"] = cg.open_sig
                if cg.end_sig:
                    d["end"] = cg.end_sig
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
        "tables": {name: {"key": t["key"], "columns": t["columns"], "rows": t["rows"]}
                   for name, t in sorted(schema.table_defs.items())},
        "records": [{"sig": sig, "name": name, "members": scope_json(root)}
                    for sig, name, root in schema.records],
    }
    return json.dumps(doc, indent=2) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--schema", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path, help="directory for Tables.h/Views.h")
    ap.add_argument("--json", type=Path, help="path for the schema.json bundle")
    ap.add_argument("--tables-ns", default="esper::schema::tables")
    ap.add_argument("--views-ns", default="esper::view")
    ap.add_argument("--check", action="store_true", help="verify outputs are up to date")
    args = ap.parse_args(argv)

    schema = Schema()
    try:
        schema.load(args.schema)
        schema.build()
        outputs = {
            args.out / "Tables.h": emit_tables(schema, args.tables_ns),
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
