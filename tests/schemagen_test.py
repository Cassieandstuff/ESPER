"""Validation tests for tools/schemagen: every rule the spec says the generator
enforces must reject a bad descriptor with a message naming the problem."""

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "schemagen"))
import schemagen  # noqa: E402

MANIFEST = 'manifest: t\nschemaVersion: "0.0.1"\n'


def build(files: dict[str, str]):
    """Load + build a schema from {relative path: yaml text}."""
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "manifest.yaml").write_text(MANIFEST, encoding="utf-8")
        for rel, text in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(textwrap.dedent(text), encoding="utf-8")
        s = schemagen.Schema()
        s.load(root)
        s.build()
        return s


def record(members: str, extra: dict[str, str] | None = None):
    files = {"records/TEST.yaml": "record: TEST\nname: Test\nmembers:\n" + textwrap.indent(textwrap.dedent(members), "  ")}
    files.update(extra or {})
    return build(files)


class Rejects(unittest.TestCase):
    def rejects(self, fragment: str, members: str, extra=None):
        with self.assertRaises(schemagen.SchemaError) as cm:
            record(members, extra)
        self.assertIn(fragment, str(cm.exception))

    def test_ambiguous_sig_in_scope(self):
        self.rejects("ambiguous", """
            - { sig: EDID, name: a, type: zstring }
            - { sig: EDID, name: b, type: zstring }
        """)

    def test_ambiguous_via_group_entry(self):
        self.rejects("ambiguous", """
            - { sig: MODL, name: a, type: zstring }
            - group: g
              members:
                - { sig: MODL, name: path, type: zstring }
        """)

    def test_struct_size_mismatch(self):
        self.rejects("sum to 4", "- { sig: DATA, name: d, type: S }",
                     {"structs/S.yaml": "struct: S\nsize: 6\nfields:\n  - { name: a, type: u32 }\n"})

    def test_unknown_type(self):
        self.rejects("unknown type", "- { sig: DATA, name: d, type: Nope }")

    def test_open_must_be_first_member(self):
        self.rejects("must be the group's first member", """
            - group: g
              open: NAM0
              members:
                - { sig: NAM1, name: a, type: u8 }
                - { sig: NAM0, name: b, type: u8 }
        """)

    def test_end_must_be_last_member(self):
        self.rejects("must be the group's last member", """
            - group: g
              open: NAM0
              end: NAM9
              members:
                - { sig: NAM0, name: a, type: u8 }
                - { sig: NAM9, name: z, type: marker }
                - { sig: NAM1, name: b, type: u8 }
        """)

    def test_end_needs_open(self):
        self.rejects("'end' needs 'open'", """
            - group: g
              end: NAM9
              members:
                - { sig: NAM0, name: a, type: u8 }
                - { sig: NAM9, name: z, type: marker }
        """)

    def test_shared_member_sigs_need_delimiters(self):
        self.rejects("ambiguous", """
            - group: a
              members:
                - { sig: ALST, name: id, type: u32 }
                - { sig: ALID, name: name, type: zstring }
            - group: b
              members:
                - { sig: ALLS, name: id, type: u32 }
                - { sig: ALID, name: name, type: zstring }
        """)

    def test_union_needs_a_case(self):
        self.rejects("at least one case", """
            - sig: DATA
              name: d
              union: { by: size, cases: [] }
        """)

    def test_union_bad_selector(self):
        self.rejects("union 'by' must be", """
            - sig: DATA
              name: d
              union: { by: banana, cases: [ { when: 1, type: u8 } ] }
        """)

    def test_lookup_unknown_table(self):
        self.rejects("unknown table", """
            - sig: DATA
              name: d
              fields:
                - { name: k, type: u16 }
                - name: v
                  union: { by: { lookup: { table: nope, key: k, column: c } }, cases: [ { when: x, type: u8 } ] }
        """)

    def test_lookup_value_must_exist(self):
        self.rejects("is not a value of", """
            - sig: DATA
              name: d
              fields:
                - { name: k, type: u16 }
                - name: v
                  union: { by: { lookup: { table: t, key: k, column: c } }, cases: [ { when: zzz, type: u8 } ] }
        """, {"tables/t.yaml": "table: t\nkey: k\ncolumns: [k, c]\nrows:\n  - [1, form]\n"})

    def test_selector_field_must_exist(self):
        self.rejects("isn't a field anywhere", """
            - sig: DATA
              name: d
              fields:
                - { name: a, type: u8 }
                - { name: b, type: u8, if: { field: nope, min: 1 } }
        """)

    def test_selector_member_must_exist(self):
        self.rejects("isn't a subrecord member anywhere", """
            - sig: EPFD
              name: d
              union: { by: { member: nope }, cases: [ { when: 1, type: f32 } ] }
        """)

    def test_if_needs_a_condition(self):
        self.rejects("min/max, equals or in", """
            - sig: DATA
              name: d
              fields:
                - { name: a, type: u8 }
                - { name: b, type: u8, if: { field: a } }
        """)

    def test_remap_unsafe_only_on_leaf_types(self):
        self.rejects("remapUnsafe applies to", "- { sig: DATA, name: d, type: S, remapUnsafe: true }",
                     {"structs/S.yaml": "struct: S\nfields:\n  - { name: a, type: u8 }\n"})

    def test_bad_signature(self):
        self.rejects("4-character", "- { sig: TOOLONG, name: a, type: u8 }")

    def test_numeric_signature_needs_quotes(self):
        self.rejects("quote it", "- { sig: 1234, name: a, type: u8 }")

    def test_remap_of_unused_sig(self):
        self.rejects("doesn't use", "- { use: G, name: g, sigs: { XXXX: YYYY } }",
                     {"groups/G.yaml": "group: G\nmembers:\n  - { sig: MODL, name: path, type: zstring }\n"})

    def test_count_of_unknown_member(self):
        self.rejects("countOf names unknown", "- { sig: KSIZ, name: n, type: u32, countOf: nope }")

    def test_count_of_needs_integer(self):
        self.rejects("must be u8, u16 or u32", """
            - { sig: KSIZ, name: n, type: f32, countOf: k }
            - { sig: KWDA, name: k, type: "formid[]" }
        """)

    def test_rest_field_must_be_last(self):
        self.rejects("only the last field", """
            - sig: DATA
              name: d
              fields:
                - { name: a, type: bytes }
                - { name: b, type: u8 }
        """)

    def test_break_struct_not_an_array_element(self):
        self.rejects("can't be an array element", '- { sig: DATA, name: d, type: "V[]" }',
                     {"structs/V.yaml": "struct: V\nfields:\n  - { name: a, type: u8 }\n  - { break: true }\n  - { name: b, type: u8 }\n"})

    def test_break_struct_not_nested(self):
        self.rejects("top-level type", """
            - sig: DATA
              name: d
              fields:
                - { name: v, type: V }
        """, {"structs/V.yaml": "struct: V\nfields:\n  - { name: a, type: u8 }\n  - { break: true }\n  - { name: b, type: u8 }\n"})

    def test_field_count_arrays_not_in_language_01(self):
        self.rejects("counts from a field", '- { sig: DATA, name: d, type: "u8[count: n]" }')

    def test_reserved_names(self):
        self.rejects("reserved", "- { sig: EDID, name: self, type: zstring }")

    def test_formids_only_none(self):
        self.rejects("may only be 'none'", "- { sig: BLOB, name: b, type: bytes, formIds: some }")

    def test_self_containing_struct(self):
        self.rejects("contains itself", "- { sig: DATA, name: d, type: S }",
                     {"structs/S.yaml": "struct: S\nfields:\n  - { name: s, type: S }\n"})


class Accepts(unittest.TestCase):
    def test_delimited_groups_may_share_member_sigs(self):
        s = record("""
            - group: refAlias
              open: ALST
              end: ALED
              members:
                - { sig: ALST, name: id, type: u32 }
                - { sig: ALID, name: name, type: zstring }
                - { sig: ALED, name: end, type: marker }
            - group: locAlias
              open: ALLS
              end: ALED
              members:
                - { sig: ALLS, name: id, type: u32 }
                - { sig: ALID, name: name, type: zstring }
                - { sig: ALED, name: end, type: marker }
        """)
        root = s.groups[s.records[0][2]]
        self.assertEqual(sorted(sig for sig, _ in root.entries), ["ALLS", "ALST"])

    def test_remapped_groups_do_not_collide(self):
        s = record("""
            - { use: G, name: a }
            - { use: G, name: b, sigs: { MODL: MOD2 } }
        """, {"groups/G.yaml": "group: G\nmembers:\n  - { sig: MODL, name: path, type: zstring }\n"})
        self.assertEqual(len(s.records), 1)

    def test_outputs_are_deterministic(self):
        files = {"records/TEST.yaml": "record: TEST\nname: Test\nmembers:\n  - { sig: EDID, name: e, type: zstring }\n"}
        a, b = build(files), build(files)
        self.assertEqual(schemagen.emit_tables(a, "n"), schemagen.emit_tables(b, "n"))
        self.assertEqual(schemagen.emit_json(a), schemagen.emit_json(b))


if __name__ == "__main__":
    unittest.main()
