#pragma once
#include <ESPER/ByteReader.h>
#include <ESPER/Schema.h>

#include <cstdint>
#include <string>
#include <vector>

namespace esper {

// VMAD — Papyrus script attachment data (documented layout; UESP
// "Mod File Format/VMAD"). This codec models the SCRIPTS section: script
// entries with typed property values, including the objFormat-1/-2 object
// encodings. Record types whose VMAD carries trailing FRAGMENT/alias sections
// (QUST, INFO, PACK, PERK, SCEN) leave bytes after the scripts — the codec
// REFUSES those payloads (→ unknown-passthrough, byte-faithful) until the
// fragment model lands with the dialog/quest wave.
//
// Fidelity notes: property "object" values keep their raw 8 bytes (both
// objFormats), bools keep their raw u8, strings are u16-length-prefixed with
// arbitrary bytes preserved. Any structural anomaly refuses the payload.

struct VmadObject {
    // Raw 8-byte object value. objFormat 2 (SSE default): [u16 unused]
    // [i16 alias][u32 formId]; objFormat 1: [u32 formId][i16 alias][u16 unused].
    uint8_t raw[8] = {};

    uint32_t FormId(int16_t objFormat) const {
        uint32_t id = 0;
        std::memcpy(&id, raw + (objFormat == 1 ? 0 : 4), 4);
        return id;
    }
    void SetFormId(int16_t objFormat, uint32_t id) {
        std::memcpy(raw + (objFormat == 1 ? 0 : 4), &id, 4);
    }
};

// Property types (uint8): 1 object, 2 string, 3 int32, 4 float, 5 bool,
// 11..15 = arrays of the same.
struct VmadProperty {
    std::string name;
    uint8_t     type   = 0;
    uint8_t     status = 1;

    std::vector<VmadObject>  objects;   // type 1 (exactly one) / 11
    std::vector<std::string> strings;   // type 2 / 12
    std::vector<int32_t>     ints;      // type 3 / 13
    std::vector<float>       floats;    // type 4 / 14
    std::vector<uint8_t>     bools;     // type 5 / 15 (raw u8 values)
};

struct VmadScript {
    std::string               name;
    uint8_t                   status = 0;
    std::vector<VmadProperty> properties;
};

struct Vmad {
    int16_t version   = 5;
    int16_t objFormat = 2;
    std::vector<VmadScript> scripts;
};

struct VmadCodec {
    static bool Decode(Vmad& out, SubView s, const SchemaCtx&);
    static void Encode(const Vmad& v, const SchemaCtx&, ByteWriter& w);
    static void VisitIds(Vmad& v, void (*fn)(uint32_t&, void*), void* ctx);
};

} // namespace esper
