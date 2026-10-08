#include <ESPER/Vmad.h>

namespace esper {

namespace {

bool ReadWString(ByteReader& r, std::string& out) {
    const uint16_t len = r.U16();
    if (!r.ok) return false;
    const uint8_t* bytes = r.Bytes(len);
    if (!bytes) return false;
    out.assign(reinterpret_cast<const char*>(bytes), len);
    return true;
}

bool ReadPropertyValue(ByteReader& r, VmadProperty& p) {
    auto readObjects = [&](uint32_t n) -> bool {
        p.objects.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* b = r.Bytes(8);
            if (!b) return false;
            std::memcpy(p.objects[i].raw, b, 8);
        }
        return true;
    };
    switch (p.type) {
    case 1:  return readObjects(1);
    case 2:  { p.strings.resize(1); return ReadWString(r, p.strings[0]); }
    case 3:  { p.ints.resize(1);   p.ints[0] = static_cast<int32_t>(r.U32()); return r.ok; }
    case 4:  { p.floats.resize(1); p.floats[0] = r.F32(); return r.ok; }
    case 5:  { p.bools.resize(1);  p.bools[0] = r.U8();  return r.ok; }
    case 11: { const uint32_t n = r.U32(); return r.ok && readObjects(n); }
    case 12: {
        const uint32_t n = r.U32();
        if (!r.ok) return false;
        p.strings.resize(n);
        for (uint32_t i = 0; i < n; ++i)
            if (!ReadWString(r, p.strings[i])) return false;
        return true;
    }
    case 13: {
        const uint32_t n = r.U32();
        if (!r.ok) return false;
        p.ints.resize(n);
        for (uint32_t i = 0; i < n; ++i) p.ints[i] = static_cast<int32_t>(r.U32());
        return r.ok;
    }
    case 14: {
        const uint32_t n = r.U32();
        if (!r.ok) return false;
        p.floats.resize(n);
        for (uint32_t i = 0; i < n; ++i) p.floats[i] = r.F32();
        return r.ok;
    }
    case 15: {
        const uint32_t n = r.U32();
        if (!r.ok) return false;
        p.bools.resize(n);
        for (uint32_t i = 0; i < n; ++i) p.bools[i] = r.U8();
        return r.ok;
    }
    default: return false;   // unknown property type → refuse whole VMAD
    }
}

void WriteWString(ByteWriter& w, const std::string& s) {
    w.U16(static_cast<uint16_t>(s.size()));
    if (!s.empty()) w.Bytes(s.data(), s.size());
}

void WritePropertyValue(ByteWriter& w, const VmadProperty& p) {
    switch (p.type) {
    case 1:  w.Bytes(p.objects[0].raw, 8); break;
    case 2:  WriteWString(w, p.strings[0]); break;
    case 3:  w.U32(static_cast<uint32_t>(p.ints[0])); break;
    case 4:  w.F32(p.floats[0]); break;
    case 5:  w.U8(p.bools[0]); break;
    case 11:
        w.U32(static_cast<uint32_t>(p.objects.size()));
        for (const VmadObject& o : p.objects) w.Bytes(o.raw, 8);
        break;
    case 12:
        w.U32(static_cast<uint32_t>(p.strings.size()));
        for (const std::string& s : p.strings) WriteWString(w, s);
        break;
    case 13:
        w.U32(static_cast<uint32_t>(p.ints.size()));
        for (int32_t v : p.ints) w.U32(static_cast<uint32_t>(v));
        break;
    case 14:
        w.U32(static_cast<uint32_t>(p.floats.size()));
        for (float v : p.floats) w.F32(v);
        break;
    case 15:
        w.U32(static_cast<uint32_t>(p.bools.size()));
        for (uint8_t v : p.bools) w.U8(v);
        break;
    default: break;
    }
}

} // namespace

bool VmadCodec::Decode(Vmad& out, SubView s, const SchemaCtx&) {
    out = Vmad{};
    ByteReader r(s.data, s.data + s.size);

    out.version   = static_cast<int16_t>(r.U16());
    out.objFormat = static_cast<int16_t>(r.U16());
    if (!r.ok) return false;
    if (out.version < 2 || out.version > 5) return false;
    if (out.objFormat != 1 && out.objFormat != 2) return false;

    const uint16_t scriptCount = r.U16();
    if (!r.ok) return false;

    out.scripts.resize(scriptCount);
    for (VmadScript& sc : out.scripts) {
        if (!ReadWString(r, sc.name)) return false;
        if (out.version >= 4) {
            sc.status = r.U8();
            if (!r.ok) return false;
        }
        const uint16_t propCount = r.U16();
        if (!r.ok) return false;
        sc.properties.resize(propCount);
        for (VmadProperty& p : sc.properties) {
            if (!ReadWString(r, p.name)) return false;
            p.type = r.U8();
            if (out.version >= 4) p.status = r.U8();
            if (!r.ok) return false;
            if (!ReadPropertyValue(r, p)) return false;
        }
    }

    // Trailing bytes mean a fragment/alias section (QUST, INFO, PACK, PERK,
    // SCEN) this codec does not model yet — refuse so the payload passes
    // through verbatim.
    return r.Remaining() == 0;
}

void VmadCodec::Encode(const Vmad& v, const SchemaCtx&, ByteWriter& w) {
    w.U16(static_cast<uint16_t>(v.version));
    w.U16(static_cast<uint16_t>(v.objFormat));
    w.U16(static_cast<uint16_t>(v.scripts.size()));
    for (const VmadScript& sc : v.scripts) {
        WriteWString(w, sc.name);
        if (v.version >= 4) w.U8(sc.status);
        w.U16(static_cast<uint16_t>(sc.properties.size()));
        for (const VmadProperty& p : sc.properties) {
            WriteWString(w, p.name);
            w.U8(p.type);
            if (v.version >= 4) w.U8(p.status);
            WritePropertyValue(w, p);
        }
    }
}

void VmadCodec::VisitIds(Vmad& v, void (*fn)(uint32_t&, void*), void* ctx) {
    for (VmadScript& sc : v.scripts)
        for (VmadProperty& p : sc.properties)
            for (VmadObject& o : p.objects) {
                uint32_t id = o.FormId(v.objFormat);
                fn(id, ctx);
                o.SetFormId(v.objFormat, id);
            }
}

} // namespace esper
