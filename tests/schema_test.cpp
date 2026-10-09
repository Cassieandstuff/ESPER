// ── esper schema-engine self-test ───────────────────────────────────────────
// Drives esper::Record through tests/schema (a test-only schema that uses every
// language-0.1 construct) and through the real schema's records. Every case
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
    TestRealSchema();
    if (g_failures == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL (%d check(s) failed)\n", g_failures);
    return 1;
}
