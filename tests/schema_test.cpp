// ── esper schema-engine self-test ───────────────────────────────────────────
// Drives esper::Record through tests/schema (a test-only schema that uses every
// language construct) and through the real schema's records. Every case
// checks the fidelity contract: decode → encode reproduces the payload, both
// verbatim and with every modeled subrecord forcibly re-encoded.

#include <ESPER.h>

#include "generated/Fields.h"
#include "generated/Tables.h"
#include "generated/Views.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                         \
    } while (0)

using Bytes = std::vector<uint8_t>;
namespace tf = esper_test::fields;
namespace tv = esper_test::view;
const esper::schema::SchemaSet& TestSchema() { return esper_test::tables::kSchema; }

uint32_t Sig(const char* s) {
    uint32_t v = 0;
    std::memcpy(&v, s, 4);
    return v;
}

struct Payload {
    esper::ByteWriter w;
    Payload& Sub(const char* sig, const Bytes& data) {
        esper::EmitSubrecord(w, Sig(sig), data.data(), data.size());
        return *this;
    }
    Payload& Z(const char* sig, const std::string& s) {
        Bytes b(s.begin(), s.end());
        b.push_back(0);
        return Sub(sig, b);
    }
    const Bytes& bytes() const { return w.buf; }
};

Bytes U16(uint16_t v) { return { uint8_t(v), uint8_t(v >> 8) }; }
Bytes U32(uint32_t v) { return { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) }; }
Bytes F32(float f) { uint32_t b; std::memcpy(&b, &f, 4); return U32(b); }
Bytes Cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::vector<uint32_t> SigsOf(const Bytes& payload) {
    std::vector<uint32_t> out;
    esper::SubIterator it(payload);
    esper::SubView s;
    while (it.Next(s)) out.push_back(s.sig);
    return out;
}

bool Decode(const Bytes& payload, esper::Record& rec, bool localized = false,
            uint32_t sig = Sig("TSTA"), const esper::schema::SchemaSet* set = nullptr) {
    esper::DecodeCtx ctx;
    ctx.localized = localized;
    std::string err;
    const bool ok = esper::Record::Decode(set ? *set : TestSchema(), sig, payload, ctx, rec, &err);
    if (!ok) std::printf("  decode error: %s\n", err.c_str());
    return ok;
}

// The fidelity contract, both ways.
void CheckRoundTrip(const esper::Record& rec, const Bytes& payload) {
    CHECK(rec.Encode(false) == payload);
    CHECK(rec.Encode(true) == payload);
}

Bytes FullAlpha() {
    Payload p;
    p.Z("EDID", "Alpha")
     .Z("FULL", "Alpha Name")
     .Sub("VERS", Cat({ U32(7), F32(2.5f), F32(0.25f), U32(0x01000801) }))
     .Sub("OUTR", Cat({ U16(3), U32(uint32_t(-9)), U32(0x01000802) }))
     .Sub("FIXD", Cat({ U16(1), U16(2), U16(3) }))
     .Sub("PRFX", Cat({ U16(2), U32(10), U32(20) }))
     .Sub("WSTR", Cat({ U16(4), Bytes{ 'w', 'i', 'd', 'e' } }))
     .Sub("KSIZ", U32(2))
     .Sub("KWDA", Cat({ U32(0x01000803), U32(0x01000804) }))
     .Sub("REFS", U32(0x01000805))
     .Sub("REFS", U32(0x01000806))
     .Sub("HASH", Bytes{ 9, 9, 9 })
     .Sub("FXSZ", Bytes{ 1, 2, 3, 4 })
     // layer 1: index + texture + two presets
     .Sub("LIDX", U16(0))
     .Z("LTEX", "tex0")
     .Sub("PCOL", U32(0x01000807)).Sub("PVAL", F32(0.5f))
     .Sub("PCOL", U32(0x01000808)).Sub("PVAL", F32(0.75f))
     // layer 2: no index (its first member is optional) + one preset
     .Z("LTEX", "tex1")
     .Sub("PCOL", U32(0x01000809)).Sub("PVAL", F32(1.0f))
     .Z("PRA1", "pairA").Sub("PRA2", Bytes{ 5 })
     .Z("PRB1", "pairB").Sub("PRB2", Bytes{ 6 })
     .Sub("DATA", Cat({ U32(uint32_t(-100)), F32(3.5f) }));
    return p.bytes();
}

void TestDecodeAndRead() {
    std::printf("T1 decode, read, round-trip\n");
    const Bytes payload = FullAlpha();
    esper::Record rec;
    CHECK(Decode(payload, rec));
    CHECK(rec.RawCount() == 0);
    CheckRoundTrip(rec, payload);

    CHECK(rec.Get(tf::TSTA::editorId) == std::string("Alpha"));
    CHECK(rec.Get(tf::TSTA::fullName)->text == "Alpha Name");
    CHECK(rec.Get(tf::TSTA::versioned::flags) == 7u);
    CHECK(rec.Get(tf::TSTA::versioned::radius) == 2.5f);
    CHECK(rec.Get(tf::TSTA::versioned::target)->raw == 0x01000801u);
    CHECK(rec.Get(tf::TSTA::outer::inner::a) == 3);
    CHECK(rec.Get(tf::TSTA::outer::inner::b) == -9);
    CHECK(rec.Get(tf::TSTA::outer::ref)->raw == 0x01000802u);
    CHECK((rec.Get(tf::TSTA::fixed) == std::vector<uint16_t>{ 1, 2, 3 }));
    CHECK((rec.Get(tf::TSTA::prefixed) == std::vector<uint32_t>{ 10, 20 }));
    CHECK(rec.Get(tf::TSTA::wide) == std::string("wide"));
    CHECK(rec.Get(tf::TSTA::keywords)->size() == 2);
    CHECK(rec.GetAll(tf::TSTA::refs).size() == 2);
    CHECK((rec.Get(tf::TSTA::hash) == Bytes{ 9, 9, 9 }));
    CHECK(rec.Get(tf::TSTA::data::value) == -100);
    CHECK(rec.Get(tf::TSTA::data::weight) == 3.5f);
    CHECK(rec.Get(tf::TSTA::first::first) == std::string("pairA"));
    CHECK(rec.Get(tf::TSTA::second::first) == std::string("pairB"));   // remapped sigs
    CHECK(rec.Get(tf::TSTA::second::second) == uint8_t(6));
}

void TestOrdinalGroups() {
    std::printf("T2 groups: ordinal rule, optional first member, nesting\n");
    esper::Record rec;
    CHECK(Decode(FullAlpha(), rec));
    const auto layers = rec.Groups(tf::TSTA::layers::self);
    CHECK(layers.size() == 2);
    if (layers.size() != 2) return;
    CHECK(layers[0].Get<uint16_t>(esper::RelPath{ { 0 }, 1, 1 }) == uint16_t(0));
    CHECK(layers[0].Get<std::string>(esper::RelPath{ { 1 }, 1, 1 }) == std::string("tex0"));
    CHECK(layers[0].Groups(esper::RelPath{ { 2 }, 1, 1 }).size() == 2);
    CHECK(!layers[1].Get<uint16_t>(esper::RelPath{ { 0 }, 1, 1 }).has_value());
    CHECK(layers[1].Get<std::string>(esper::RelPath{ { 1 }, 1, 1 }) == std::string("tex1"));
    CHECK(layers[1].Groups(esper::RelPath{ { 2 }, 1, 1 }).size() == 1);

    // Through handles: the first instance on each level.
    CHECK(rec.Get(tf::TSTA::layers::texture) == std::string("tex0"));
    CHECK(rec.Get(tf::TSTA::layers::presets::value) == 0.5f);

    // The same singular member twice in a row starts a new instance.
    Payload p;
    p.Z("LTEX", "a").Z("LTEX", "b").Sub("PCOL", U32(1)).Sub("PCOL", U32(2));
    esper::Record r2;
    CHECK(Decode(p.bytes(), r2));
    CHECK(r2.RawCount() == 0);
    CheckRoundTrip(r2, p.bytes());
    const auto two = r2.Groups(tf::TSTA::layers::self);
    CHECK(two.size() == 2);
    if (two.size() == 2) {
        CHECK(two[0].Get<std::string>(esper::RelPath{ { 1 }, 1, 1 }) == std::string("a"));
        CHECK(two[1].Get<std::string>(esper::RelPath{ { 1 }, 1, 1 }) == std::string("b"));
        CHECK(two[1].Groups(esper::RelPath{ { 2 }, 1, 1 }).size() == 2);   // PCOL, PCOL
    }

    // A singular group may not repeat: its second occurrence stays raw.
    Payload q;
    q.Z("PRA1", "x").Z("PRA1", "y");
    esper::Record r3;
    CHECK(Decode(q.bytes(), r3));
    CHECK(r3.RawCount() == 1);
    CheckRoundTrip(r3, q.bytes());
}

void TestBreak() {
    std::printf("T3 break: truncated form round-trips; extending writes the full form\n");
    Payload p;
    p.Z("EDID", "B").Sub("VERS", Cat({ U32(1), F32(1.5f) }));     // ends at the break
    esper::Record rec;
    CHECK(Decode(p.bytes(), rec));
    CHECK(rec.RawCount() == 0);
    CheckRoundTrip(rec, p.bytes());
    CHECK(rec.Get(tf::TSTA::versioned::radius) == 1.5f);
    CHECK(!rec.Get(tf::TSTA::versioned::falloff).has_value());

    rec.Set(tf::TSTA::versioned::falloff, 9.0f);
    Payload want;
    want.Z("EDID", "B").Sub("VERS", Cat({ U32(1), F32(1.5f), F32(9.0f), U32(0) }));
    CHECK(rec.Encode() == want.bytes());

    // A size that is neither the short nor the full form is refused.
    Payload bad;
    bad.Sub("VERS", Cat({ U32(1), F32(1.5f), F32(2.0f) }));
    esper::Record r2;
    CHECK(Decode(bad.bytes(), r2));
    CHECK(r2.RawCount() == 1);
    CheckRoundTrip(r2, bad.bytes());
}

void TestRefusal() {
    std::printf("T4 refusal: unreproducible subrecords stay raw, in place\n");
    Payload p;
    p.Sub("EDID", Bytes{ 'x', 'y' })                       // no terminator
     .Sub("OUTR", Bytes(9, 0))                             // wrong size
     .Z("FULL", "ok")
     .Z("FULL", "dup")                                     // second singular occurrence
     .Sub("ZZZZ", Bytes{ 1, 2 })                           // not in the schema
     .Sub("WSTR", Cat({ U16(10), Bytes{ 'a' } }))          // length overruns
     .Sub("PRFX", Cat({ U16(1), U32(1), U32(2) }));        // trailing bytes
    esper::Record rec;
    CHECK(Decode(p.bytes(), rec));
    CHECK(rec.RawCount() == 6);
    CheckRoundTrip(rec, p.bytes());
    CHECK(rec.Get(tf::TSTA::fullName)->text == "ok");
    CHECK(!rec.Get(tf::TSTA::editorId).has_value());
    std::string why;
    CHECK(!rec.RemapSafe(&why));
    CHECK(why.find("raw subrecord") != std::string::npos);
}

void TestLocalized() {
    std::printf("T5 lstring under the localized flag\n");
    Payload p;
    p.Sub("FULL", U32(0x1234));
    esper::Record rec;
    CHECK(Decode(p.bytes(), rec, /*localized=*/true));
    CHECK(rec.RawCount() == 0);
    CHECK(rec.Get(tf::TSTA::fullName)->id == 0x1234u);
    CheckRoundTrip(rec, p.bytes());
    // The same bytes in a non-localized plugin are not a valid zstring.
    esper::Record r2;
    CHECK(Decode(p.bytes(), r2, /*localized=*/false));
    CHECK(r2.RawCount() == 1);
}

void TestVerbatimUnlessModified() {
    std::printf("T6 verbatim unless modified\n");
    // A NaN with a payload must survive forced re-encode bit-exactly.
    Payload p;
    p.Z("EDID", "N").Sub("DATA", Cat({ U32(1), U32(0x7FC01234) }));
    esper::Record rec;
    CHECK(Decode(p.bytes(), rec));
    CheckRoundTrip(rec, p.bytes());

    // Setting a value to what it already is changes nothing.
    rec.Set(tf::TSTA::editorId, std::string("N"));
    CHECK(rec.Encode() == p.bytes());

    // Editing one field re-encodes only that subrecord.
    rec.Set(tf::TSTA::editorId, std::string("M"));
    Payload want;
    want.Z("EDID", "M").Sub("DATA", Cat({ U32(1), U32(0x7FC01234) }));
    CHECK(rec.Encode() == want.bytes());
}

void TestCreateAndOrder() {
    std::printf("T7 create: new subrecords land in schema order; derived counts\n");
    esper::Record rec;
    CHECK(esper::Record::Create(TestSchema(), Sig("TSTA"), {}, rec));
    // Set in reverse schema order.
    rec.Set(tf::TSTA::data::weight, 2.0f);
    rec.Set(tf::TSTA::keywords, std::vector<esper::FormId>{ { 1 }, { 2 }, { 3 } });
    rec.Set(tf::TSTA::editorId, std::string("New"));
    const Bytes out = rec.Encode();
    CHECK((SigsOf(out) == std::vector<uint32_t>{ Sig("EDID"), Sig("KSIZ"), Sig("KWDA"), Sig("DATA") }));
    CHECK(rec.Get(tf::TSTA::keywordCount) == 3u);

    esper::Record back;
    CHECK(Decode(out, back));
    CHECK(back.RawCount() == 0);
    CHECK(back.Get(tf::TSTA::data::weight) == 2.0f);
    CHECK(back.Get(tf::TSTA::data::value) == 0);

    rec.Clear(tf::TSTA::keywords);
    CHECK(!rec.Get(tf::TSTA::keywordCount).has_value());
    CHECK((SigsOf(rec.Encode()) == std::vector<uint32_t>{ Sig("EDID"), Sig("DATA") }));
}

void TestRepeated() {
    std::printf("T8 repeated members: SetAll keeps unchanged occurrences verbatim\n");
    esper::Record rec;
    const Bytes payload = FullAlpha();
    CHECK(Decode(payload, rec));
    rec.SetAll(tf::TSTA::refs, std::vector<esper::FormId>{ { 0x01000805 }, { 0x01000806 } });
    CHECK(rec.Encode() == payload);
    rec.SetAll(tf::TSTA::refs, std::vector<esper::FormId>{ { 0x01000805 }, { 7 }, { 8 } });
    const auto refs = rec.GetAll(tf::TSTA::refs);
    CHECK(refs.size() == 3 && refs[1].raw == 7u && refs[2].raw == 8u);
    rec.SetAll(tf::TSTA::refs, std::vector<esper::FormId>{});
    CHECK(rec.GetAll(tf::TSTA::refs).empty());
    esper::Record back;
    CHECK(Decode(rec.Encode(), back));
    CHECK(back.RawCount() == 0);
    CHECK(back.GetAll(tf::TSTA::refs).empty());
}

void TestFormIds() {
    std::printf("T9 FormID visitation, remap safety, link targets\n");
    esper::Record rec;
    CHECK(Decode(FullAlpha(), rec));
    CHECK(rec.RemapSafe());                     // HASH/FXSZ are declared FormID-free

    std::vector<uint32_t> seen;
    size_t toTstb = 0;
    rec.ForEachFormId([&](uint32_t id, std::span<const uint32_t> to) {
        seen.push_back(id);
        if (to.size() == 1 && to[0] == Sig("TSTB")) ++toTstb;
    });
    CHECK(seen.size() == 9);
    CHECK(toTstb == 3);                         // VERS.target + two KWDA entries

    CHECK(rec.VisitFormIds([](uint32_t& id) { if ((id >> 24) == 1) id = (id & 0xFFFFFF) | 0x02000000; }));
    size_t remapped = 0;
    rec.ForEachFormId([&](uint32_t id, std::span<const uint32_t>) { remapped += (id >> 24) == 2; });
    CHECK(remapped == 9);
    esper::Record back;
    CHECK(Decode(rec.Encode(), back));
    CHECK(back.Get(tf::TSTA::outer::ref)->raw == 0x02000802u);
    CHECK(back.Get(tf::TSTA::layers::presets::color)->raw == 0x02000807u);

    // Opaque bytes not declared FormID-free make the record refuse.
    Payload p;
    p.Sub("REFS", U32(0x01000001)).Sub("BLOB", Bytes{ 1, 2, 3, 4 });
    esper::Record r2;
    CHECK(Decode(p.bytes(), r2));
    std::string why;
    CHECK(!r2.VisitFormIds([](uint32_t& id) { id = 0; }, &why));
    CHECK(why.find("opaque bytes in BLOB") != std::string::npos);
    CHECK(r2.Encode() == p.bytes());            // refused: untouched
}

void TestPathsAndViews() {
    std::printf("T10 string paths and typed views\n");
    esper::Record rec;
    const Bytes payload = FullAlpha();
    CHECK(Decode(payload, rec));
    const auto p = rec.Find("outer/inner/b");
    CHECK(p.has_value());
    if (p) CHECK(rec.GetValue(*p) && int32_t(rec.GetValue(*p)->bits) == -9);
    CHECK(!rec.Find("outer/nope").has_value());
    CHECK(rec.Find("layers/presets/value").has_value());

    tv::TestAlpha a(rec);
    CHECK(a.EditorId() == std::string("Alpha"));
    CHECK(a.Versioned().Radius() == 2.5f);
    CHECK(a.Outer().Inner().B() == -9);
    CHECK(a.Second()->First() == std::string("pairB"));
    const auto kw = a.Keywords();
    CHECK(kw && kw->size() == 2 && (*kw)[0].id.raw == 0x01000803u);    // Link<TestBeta>
    const auto layers = a.Layers();
    CHECK(layers.size() == 2);
    if (layers.size() == 2) {
        CHECK(layers[0].Presets().size() == 2);
        CHECK(layers[0].Presets()[1].Value() == 0.75f);
        CHECK(!layers[1].Index().has_value());
    }

    // Adding a layer places it after the existing ones, before the Pair groups.
    auto added = a.AddLayers();
    added.SetTexture("tex2");
    added.AddPresets().SetValue(4.0f);
    const auto sigs = SigsOf(rec.Encode());
    const auto pra1 = std::find(sigs.begin(), sigs.end(), Sig("PRA1"));
    CHECK(pra1 != sigs.end() && pra1 - sigs.begin() >= 3 &&
          *(pra1 - 2) == Sig("LTEX") && *(pra1 - 1) == Sig("PVAL"));
    esper::Record back;
    CHECK(Decode(rec.Encode(), back));
    CHECK(back.RawCount() == 0);
    CHECK(tv::TestAlpha(back).Layers().size() == 3);

    a.Outer().Inner().SetA(42);
    CHECK(rec.Get(tf::TSTA::outer::inner::a) == 42);
}

void TestXxxx() {
    std::printf("T11 XXXX-framed subrecords\n");
    Payload p;
    p.Sub("HASH", Bytes(70000, 7));            // > 0xFFFF: emitted with an XXXX pair
    esper::Record rec;
    CHECK(Decode(p.bytes(), rec));
    CHECK(rec.RawCount() == 0);
    CheckRoundTrip(rec, p.bytes());
}

void TestMarker() {
    std::printf("T13 marker: presence-only, non-empty refused\n");
    Payload p;
    p.Z("EDID", "M").Sub("MARK", {});
    esper::Record rec;
    CHECK(Decode(p.bytes(), rec));
    CHECK(rec.RawCount() == 0);
    CheckRoundTrip(rec, p.bytes());
    tv::TestAlpha a(rec);
    CHECK(a.Flagged());
    a.SetFlagged(false);
    CHECK(!a.Flagged());
    CHECK((SigsOf(rec.Encode()) == std::vector<uint32_t>{ Sig("EDID") }));
    a.SetFlagged(true);
    CHECK(rec.Encode() == p.bytes());

    Payload bad;
    bad.Sub("MARK", Bytes{ 1 });
    esper::Record r2;
    CHECK(Decode(bad.bytes(), r2));
    CHECK(r2.RawCount() == 1);
    CheckRoundTrip(r2, bad.bytes());
}

bool DecodeU(const Bytes& payload, esper::Record& rec, uint16_t formVersion = 44) {
    esper::DecodeCtx ctx;
    ctx.formVersion = formVersion;
    std::string err;
    const bool ok = esper::Record::Decode(TestSchema(), Sig("TSTU"), payload, ctx, rec, &err);
    if (!ok) std::printf("  decode error: %s\n", err.c_str());
    return ok;
}

Bytes U8(uint8_t v) { return { v }; }

void TestUnions() {
    std::printf("T14 unions: every selector, if-fields, named unions, refusal\n");
    // size
    {
        Payload a, b, c;
        a.Sub("SIZE", U8(7));
        b.Sub("SIZE", U16(0x1234));
        c.Sub("SIZE", Bytes{ 1, 2, 3 });                       // no case for 3 bytes
        esper::Record ra, rb, rc;
        CHECK(DecodeU(a.bytes(), ra) && DecodeU(b.bytes(), rb) && DecodeU(c.bytes(), rc));
        CHECK(tv::TestUnions(ra).SizedSmall() == uint8_t(7));
        CHECK(!tv::TestUnions(ra).SizedWide().has_value());
        CHECK(tv::TestUnions(rb).SizedWide() == uint16_t(0x1234));
        CHECK(rc.RawCount() == 1);
        CheckRoundTrip(ra, a.bytes());
        CheckRoundTrip(rb, b.bytes());
        CheckRoundTrip(rc, c.bytes());
        // Setting the other case switches the union (and the encoded size).
        tv::TestUnions(ra).SetSizedWide(0xBEEF);
        Payload want;
        want.Sub("SIZE", U16(0xBEEF));
        CHECK(ra.Encode() == want.bytes());
    }
    // lookup + masked field: FormID-ness of the param comes from the table
    {
        auto cond = [](uint8_t op, uint16_t fn, uint32_t param) {
            Payload p;
            p.Sub("COND", Cat({ U8(op), U16(fn), U32(param) }));
            return p.bytes();
        };
        esper::Record form, alias, plain, unknown;
        CHECK(DecodeU(cond(0, 1, 0x01000ABC), form));
        CHECK(DecodeU(cond(2, 1, 7), alias));
        CHECK(DecodeU(cond(0, 2, 99), plain));
        CHECK(DecodeU(cond(0, 9, 0x01000ABC), unknown));
        tv::TestUnions vf(form), va(alias), vp(plain), vu(unknown);
        CHECK(vf.Cond().ParamFormRef()->raw == 0x01000ABCu);
        CHECK(va.Cond().ParamFormAlias() == 7);
        CHECK(vp.Cond().ParamPlain() == 99u);
        CHECK(vu.Cond().ParamUnknown() == 0x01000ABCu);
        size_t ids = 0;
        form.ForEachFormId([&](uint32_t, std::span<const uint32_t>) { ++ids; });
        CHECK(ids == 1);
        ids = 0;
        alias.ForEachFormId([&](uint32_t, std::span<const uint32_t>) { ++ids; });
        CHECK(ids == 0);                                         // alias index: not a FormID
        CHECK(form.RemapSafe() && alias.RemapSafe() && plain.RemapSafe());
        std::string why;
        CHECK(!unknown.RemapSafe(&why));                         // unknown function: refuse
        for (auto* r : { &form, &alias, &plain, &unknown }) CHECK(r->RawCount() == 0);
        CheckRoundTrip(form, cond(0, 1, 0x01000ABC));
        CheckRoundTrip(unknown, cond(0, 9, 0x01000ABC));
    }
    // if-fields on an ancestor, counted arrays of union-bearing elements
    {
        Payload v3, v4, bad;
        v3.Sub("VER2", Cat({ U16(3), U16(2),
                             U8(1), U32(0x01000001),
                             U8(2), U16(2), Bytes{ 'h', 'i' } }));
        v4.Sub("VER2", Cat({ U16(4), U8(9), U16(1),
                             U8(3), U32(uint32_t(-5)), U8(1) }));
        bad.Sub("VER2", Cat({ U16(3), U16(1), U8(8), U32(0) }));    // kind 8: no case
        esper::Record r3, r4, rb;
        CHECK(DecodeU(v3.bytes(), r3) && DecodeU(v4.bytes(), r4) && DecodeU(bad.bytes(), rb));
        CHECK(r3.RawCount() == 0 && r4.RawCount() == 0 && rb.RawCount() == 1);
        CheckRoundTrip(r3, v3.bytes());
        CheckRoundTrip(r4, v4.bytes());
        CheckRoundTrip(rb, bad.bytes());
        CHECK(!tv::TestUnions(r3).Versioned().Status().has_value());
        CHECK(tv::TestUnions(r4).Versioned().Status() == uint8_t(9));
        const auto items = r3.Get(tf::TSTU::versioned::items);
        CHECK(items && items->size() == 2);
        size_t ids = 0;
        r3.ForEachFormId([&](uint32_t id, std::span<const uint32_t>) { ids += id == 0x01000001u; });
        CHECK(ids == 1);
    }
    // record and form-version selectors
    {
        Payload p;
        p.Sub("RECS", U32(5)).Sub("FVER", U32(6));
        esper::Record modern, legacy;
        CHECK(DecodeU(p.bytes(), modern, 44));
        CHECK(tv::TestUnions(modern).PerRecordMine() == 5u);
        CHECK(tv::TestUnions(modern).ByFormVersionModern() == 6u);
        CHECK(DecodeU(p.bytes(), legacy, 43));                 // 4 bytes but legacy wants 2
        CHECK(legacy.RawCount() == 1);
        CheckRoundTrip(modern, p.bytes());
        CheckRoundTrip(legacy, p.bytes());
    }
    // named union selecting on an enclosing field
    {
        Payload a, b;
        a.Sub("OBJS", Cat({ U16(1), U32(0x01000777), U32(0) }));
        b.Sub("OBJS", Cat({ U16(2), U32(0), U32(0x01000777) }));
        esper::Record ra, rb;
        CHECK(DecodeU(a.bytes(), ra) && DecodeU(b.bytes(), rb));
        CHECK(tv::TestUnions(ra).Obj().TargetV1().Id()->raw == 0x01000777u);
        CHECK(tv::TestUnions(rb).Obj().TargetV2().Id()->raw == 0x01000777u);
        CHECK(!tv::TestUnions(rb).Obj().TargetV1().Id().has_value());
        CHECK(rb.VisitFormIds([](uint32_t& id) { id = 0x02000777; }));
        Payload want;
        want.Sub("OBJS", Cat({ U16(2), U32(0), U32(0x02000777) }));
        CHECK(rb.Encode() == want.bytes());
    }
}

void TestDelimited() {
    std::printf("T15 delimited groups: open/end markers, shared member sigs, reopening\n");
    auto decode = [](const Bytes& payload, esper::Record& rec) {
        std::string err;
        const bool ok = esper::Record::Decode(TestSchema(), Sig("TSTD"), payload, {}, rec, &err);
        if (!ok) std::printf("  decode error: %s\n", err.c_str());
        return ok;
    };
    Payload p;
    p.Z("EDID", "Q")
     .Sub("ALST", U32(1)).Z("ALID", "a").Sub("FNAM", U32(5))
         .Sub("CTDA", U32(7)).Z("CIS1", "x").Sub("CTDA", U32(8)).Sub("ALED", {})
     .Sub("FNAM", U32(0xAA))                                                   // root again: ALED closed the alias
     .Sub("ALLS", U32(2)).Sub("FNAM", U32(6)).Z("ALID", "b").Sub("ALED", {})   // members out of order
     .Sub("ALST", U32(3))                                                      // no ALED: closed by the next opener
     .Sub("ALST", U32(4)).Sub("ALED", {})
     .Sub("INDX", U16(10)).Z("CNAM", "s1").Sub("INDX", U16(20)).Z("CNAM", "s2");
    esper::Record rec;
    CHECK(decode(p.bytes(), rec));
    CHECK(rec.RawCount() == 0);
    CheckRoundTrip(rec, p.bytes());
    tv::TestDelimited d(rec);
    CHECK(d.RootFlags() == 0xAAu);
    const auto refs = d.RefAliases();
    CHECK(refs.size() == 3);
    if (refs.size() == 3) {
        CHECK(refs[0].Id() == 1u && refs[0].Name() == std::string("a") && refs[0].Flags() == 5u);
        CHECK(refs[0].Conditions().size() == 2);
        CHECK(refs[0].End());
        CHECK(refs[1].Id() == 3u && !refs[1].End());
        CHECK(refs[2].Id() == 4u && refs[2].End());
    }
    const auto locs = d.LocAliases();
    CHECK(locs.size() == 1 && locs[0].Id() == 2u && locs[0].Name() == std::string("b") && locs[0].Flags() == 6u);
    const auto stages = d.Stages();
    CHECK(stages.size() == 2 && stages[1].Index() == uint16_t(20) && stages[1].Text() == std::string("s2"));

    // A root sig inside an open alias closes it; alias members after that have
    // nowhere to go (they're entered only through ALST) and stay raw.
    Payload q;
    q.Sub("ALST", U32(1)).Sub("ROOT", Bytes{ 9 }).Z("ALID", "late");
    esper::Record r2;
    CHECK(decode(q.bytes(), r2));
    CHECK(r2.RawCount() == 1);
    CHECK(tv::TestDelimited(r2).RootByte() == uint8_t(9));
    CheckRoundTrip(r2, q.bytes());

    // Built through the API, an instance encodes opener first and end last,
    // and reads back as one instance.
    esper::Record r3;
    CHECK(esper::Record::Create(TestSchema(), Sig("TSTD"), {}, r3));
    auto alias = tv::TestDelimited(r3).AddRefAliases();
    alias.SetEnd(true);
    alias.SetName("n");
    alias.SetId(9);
    CHECK((SigsOf(r3.Encode()) == std::vector<uint32_t>{ Sig("ALST"), Sig("ALID"), Sig("ALED") }));
    esper::Record back;
    CHECK(decode(r3.Encode(), back));
    CHECK(back.RawCount() == 0 && tv::TestDelimited(back).RefAliases().size() == 1);
}

void TestSiblings() {
    std::printf("T16 member selectors and same-sig open/end\n");
    auto decode = [](const Bytes& payload, esper::Record& rec) {
        std::string err;
        const bool ok = esper::Record::Decode(TestSchema(), Sig("TSTE"), payload, {}, rec, &err);
        if (!ok) std::printf("  decode error: %s\n", err.c_str());
        return ok;
    };
    Payload p;
    p.Sub("PRKE", Bytes{ 0 }).Sub("EPFT", Bytes{ 1 }).Sub("EPFD", F32(1.5f)).Sub("PRKF", {})
     .Sub("PRKE", Bytes{ 1 }).Sub("EPFT", Bytes{ 4 }).Sub("EPFD", U32(0x01000801)).Sub("PRKF", {})
     .Sub("PRKE", Bytes{ 2 }).Sub("EPFT", Bytes{ 6 }).Z("EPFD", "hello").Sub("PRKF", {})
     .Sub("PRKE", Bytes{ 3 }).Sub("EPFT", Bytes{ 9 }).Sub("EPFD", U32(5)).Sub("PRKF", {})      // no case: raw
     .Sub("PRKE", Bytes{ 4 }).Sub("EPFD", U32(5)).Sub("PRKF", {})                             // no EPFT: raw
     .Sub("HNAM", {}).Z("NAM0", "a").Sub("HNAM", {})
     .Sub("HNAM", {}).Z("NAM0", "b").Sub("HNAM", {});
    esper::Record rec;
    CHECK(decode(p.bytes(), rec));
    CHECK(rec.RawCount() == 2);
    CheckRoundTrip(rec, p.bytes());
    tv::TestSiblings t(rec);
    const auto entries = t.Entries();
    CHECK(entries.size() == 5);
    if (entries.size() == 5) {
        CHECK(entries[0].FunctionDataValue() == 1.5f);
        CHECK(entries[1].FunctionDataSpell() && entries[1].FunctionDataSpell()->id.raw == 0x01000801u);
        CHECK(entries[2].FunctionDataText() == std::string("hello"));
        CHECK(!entries[3].FunctionDataCase().has_value());
        for (const auto& e : entries) CHECK(e.End());
    }
    size_t ids = 0;
    rec.ForEachFormId([&](uint32_t, std::span<const uint32_t>) { ++ids; });
    CHECK(ids == 1);
    const auto phases = t.Phases();
    CHECK(phases.size() == 2);
    if (phases.size() == 2) {
        CHECK(phases[0].Start() && phases[0].End() && phases[0].Name() == std::string("a"));
        CHECK(phases[1].Start() && phases[1].End() && phases[1].Name() == std::string("b"));
    }
}

void TestRealSchema() {
    std::printf("T12 real schema: ARMO model slots, views, old-path parity\n");
    Payload p;
    p.Z("EDID", "ArmorX")
     .Sub("OBND", Bytes(12, 1))
     .Z("MOD2", "male.nif").Sub("MO2T", Bytes{ 1, 2, 3 })
     .Z("ICON", "m.dds")
     .Z("MOD4", "female.nif")
     .Sub("BOD2", Cat({ U32(4), U32(1) }))
     .Sub("KSIZ", U32(1)).Sub("KWDA", U32(0x00012345))
     .Sub("MODL", U32(0x00000A01)).Sub("MODL", U32(0x00000A02))
     .Sub("DATA", Cat({ U32(25), F32(6.0f) }))
     .Sub("DNAM", U32(2000));
    esper::Record rec;
    CHECK(Decode(p.bytes(), rec, false, Sig("ARMO"), &esper::schema::Main()));
    CHECK(rec.RawCount() == 0);
    CheckRoundTrip(rec, p.bytes());

    esper::view::Armor armor(rec);
    CHECK(armor.EditorId() == std::string("ArmorX"));
    CHECK(armor.MaleModel() && armor.MaleModel()->Path() == std::string("male.nif"));
    CHECK(armor.FemaleModel() && armor.FemaleModel()->Path() == std::string("female.nif"));
    CHECK(armor.Armatures().size() == 2);
    CHECK(armor.ValueWeight().Weight() == 6.0f);
    CHECK(armor.Biped().ArmorType() == 1u);
    CHECK(armor.Keywords() && armor.Keywords()->size() == 1);

    // The typed path decodes the same payload to the same values and bytes.
    esper::ArmoRec old;
    CHECK(esper::DecodeRecord(esper::kArmoSchema, p.bytes(), esper::SchemaCtx{}, old));
    CHECK(old.unknown.empty());
    CHECK(old.maleModel == std::string("male.nif"));
    CHECK(old.armatures.size() == 2);
}

} // namespace

int main() {
    TestDecodeAndRead();
    TestOrdinalGroups();
    TestBreak();
    TestRefusal();
    TestLocalized();
    TestVerbatimUnlessModified();
    TestCreateAndOrder();
    TestRepeated();
    TestFormIds();
    TestPathsAndViews();
    TestXxxx();
    TestMarker();
    TestUnions();
    TestDelimited();
    TestSiblings();
    TestRealSchema();
    if (g_failures == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL (%d check(s) failed)\n", g_failures);
    return 1;
}
