#pragma once
#include <ESPER/RecordsCommon.h>
#include <ESPER/Vmad.h>

namespace esper {

#pragma pack(push, 1)

// CELL XCLC — exterior grid coords, 12 bytes (SSE form).
struct CellGrid {
    int32_t  x = 0;
    int32_t  y = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(CellGrid) == 12);

// REFR/ACHR DATA — placement, 24 bytes (position + rotation, radians).
struct Placement {
    float posX = 0.f, posY = 0.f, posZ = 0.f;
    float rotX = 0.f, rotY = 0.f, rotZ = 0.f;
};
static_assert(sizeof(Placement) == 24);

// REFR XESP — enable parent, 8 bytes.
struct EnableParent {
    uint32_t reference = 0;          // FormID
    uint32_t flags     = 0;
};
static_assert(sizeof(EnableParent) == 8);
inline constexpr FieldDef kEnableParentIds[] = { FormIdAt(0) };

// REFR XTEL — teleport destination, 32 bytes.
struct TeleportDest {
    uint32_t door = 0;               // FormID
    Placement placement;
    uint32_t flags = 0;
};
static_assert(sizeof(TeleportDest) == 32);
inline constexpr FieldDef kTeleportDestIds[] = { FormIdAt(0) };

// WRLD WCTR — fixed center cell, 4 bytes.
struct WorldCenter {
    int16_t x = 0;
    int16_t y = 0;
};
static_assert(sizeof(WorldCenter) == 4);

// WRLD DNAM — default land/water heights, 8 bytes.
struct WorldDefaults {
    float landHeight  = 0.f;
    float waterHeight = 0.f;
};
static_assert(sizeof(WorldDefaults) == 8);

// WRLD NAM0/NAM9 — object bounds corner, 8 bytes.
struct WorldCorner {
    float x = 0.f;
    float y = 0.f;
};
static_assert(sizeof(WorldCorner) == 8);

// LAND VHGT — height data: base offset + 33×33 row-delta bytes, 1096 bytes.
// World-space Z at vertex (r,c) is derived by accumulating int8 deltas:
// column 0 of each row continues from the previous row's column 0; within a
// row each delta adds to the previous vertex; the sum × 8 + offset × 8 is
// game units (the ×8 quantization is the format's).
struct LandHeights {
    float  offset = 0.f;
    int8_t deltas[33 * 33] = {};
    uint8_t pad[3] = {};
};
static_assert(sizeof(LandHeights) == 1096);

// LAND VNML / VCLR — 33×33 per-vertex byte triples, 3267 bytes.
struct LandVertexTriples {
    uint8_t v[33 * 33 * 3] = {};
};
static_assert(sizeof(LandVertexTriples) == 3267);

// LAND BTXT — base texture per quadrant, 8 bytes.
struct LandBaseTexture {
    uint32_t landTexture = 0;        // LTEX FormID
    uint8_t  quadrant = 0;           // 0=BL 1=BR 2=TL 3=TR
    uint8_t  pad = 0;
    uint16_t pad2 = 0;
};
static_assert(sizeof(LandBaseTexture) == 8);
inline constexpr FieldDef kLandBaseTextureIds[] = { FormIdAt(0) };

// LAND ATXT — additional texture layer header, 8 bytes.
struct LandAlphaTexture {
    uint32_t landTexture = 0;        // LTEX FormID
    uint8_t  quadrant = 0;
    uint8_t  pad = 0;
    uint16_t layer = 0;
};
static_assert(sizeof(LandAlphaTexture) == 8);
inline constexpr FieldDef kLandAlphaTextureIds[] = { FormIdAt(0) };

// LAND VTXT cell — one alpha-map sample, 8 bytes. `position` indexes the
// 17×17 quadrant grid (row-major).
struct LandAlphaCell {
    uint16_t position = 0;
    uint8_t  pad[2] = {};
    float    opacity = 0.f;
};
static_assert(sizeof(LandAlphaCell) == 8);

// STAT DNAM — 8 bytes: max angle + directional material.
struct StatData {
    float    maxAngle = 0.f;
    uint32_t material = 0;           // MATO FormID
};
static_assert(sizeof(StatData) == 8);
inline constexpr FieldDef kStatDataIds[] = { FormIdAt(0) };

// LTEX HNAM — havok friction/restitution, 2 bytes.
struct LtexHavok {
    uint8_t friction = 0;
    uint8_t restitution = 0;
};
static_assert(sizeof(LtexHavok) == 2);

#pragma pack(pop)

// VTXT payload: a whole array of LandAlphaCell in one subrecord.
struct LandAlphaCells {
    static bool Decode(std::vector<LandAlphaCell>& out, SubView s, const SchemaCtx&) {
        if (s.size % sizeof(LandAlphaCell) != 0) return false;
        const uint32_t n = s.size / static_cast<uint32_t>(sizeof(LandAlphaCell));
        out.resize(n);
        if (n) std::memcpy(out.data(), s.data, s.size);
        return true;
    }
    static void Encode(const std::vector<LandAlphaCell>& v, const SchemaCtx&,
                       ByteWriter& w) {
        if (!v.empty()) w.Bytes(v.data(), v.size() * sizeof(LandAlphaCell));
    }
};

// ── WRLD — worldspace ─────────────────────────────────────────────────────────
// The OFST offset table (frequently XXXX-extended) stays opaque.

struct WrldRec : RecordExtras {
    std::optional<std::string>          editorId;      // EDID
    std::optional<Lstring>              fullName;      // FULL
    std::optional<WorldCenter>          center;        // WCTR
    std::optional<FormRef>              interiorLighting; // LTMP
    std::optional<FormRef>              encounterZone; // XEZN
    std::optional<FormRef>              location;      // XLCN
    std::optional<FormRef>              climate;       // CNAM
    std::optional<FormRef>              water;         // NAM2
    std::optional<FormRef>              lodWaterType;  // NAM3
    std::optional<float>                lodWaterHeight;// NAM4
    std::optional<WorldDefaults>        defaults;      // DNAM
    std::optional<std::vector<uint8_t>> mapData;       // MNAM (size variants)
    std::optional<std::vector<uint8_t>> mapOffset;     // ONAM
    std::optional<float>                distantLodMult;// NAMA
    std::optional<uint8_t>              flags;         // DATA
    std::optional<WorldCorner>          boundsMin;     // NAM0
    std::optional<WorldCorner>          boundsMax;     // NAM9
    std::optional<FormRef>              parentWorld;   // WNAM
    std::optional<uint16_t>             parentFlags;   // PNAM
    std::optional<FormRef>              music;         // ZNAM
    std::optional<std::string>          hdLodDiffuse;  // TNAM
    std::optional<std::string>          hdLodNormal;   // UNAM
    std::optional<std::string>          waterEnvMap;   // XWEM
    std::optional<std::vector<uint8_t>> offsetData;    // OFST (opaque, XXXX-framed)
};

inline const auto kWrldSchema = MakeSchema<WrldRec>("WRLD",
    Field<&WrldRec::editorId, ZString>("EDID"),
    Field<&WrldRec::fullName, LString>("FULL"),
    Field<&WrldRec::center, Packed<WorldCenter>>("WCTR"),
    Field<&WrldRec::interiorLighting, FormRefCodec>("LTMP"),
    Field<&WrldRec::encounterZone, FormRefCodec>("XEZN"),
    Field<&WrldRec::location, FormRefCodec>("XLCN"),
    Field<&WrldRec::climate, FormRefCodec>("CNAM"),
    Field<&WrldRec::water, FormRefCodec>("NAM2"),
    Field<&WrldRec::lodWaterType, FormRefCodec>("NAM3"),
    Field<&WrldRec::lodWaterHeight, F32>("NAM4"),
    Field<&WrldRec::defaults, Packed<WorldDefaults>>("DNAM"),
    Field<&WrldRec::mapData, Blob>("MNAM"),
    Field<&WrldRec::mapOffset, Blob>("ONAM"),
    Field<&WrldRec::distantLodMult, F32>("NAMA"),
    Field<&WrldRec::flags, U8v>("DATA"),
    Field<&WrldRec::boundsMin, Packed<WorldCorner>>("NAM0"),
    Field<&WrldRec::boundsMax, Packed<WorldCorner>>("NAM9"),
    Field<&WrldRec::parentWorld, FormRefCodec>("WNAM"),
    Field<&WrldRec::parentFlags, U16v>("PNAM"),
    Field<&WrldRec::music, FormRefCodec>("ZNAM"),
    Field<&WrldRec::hdLodDiffuse, ZString>("TNAM"),
    Field<&WrldRec::hdLodNormal, ZString>("UNAM"),
    Field<&WrldRec::waterEnvMap, ZString>("XWEM"),
    Field<&WrldRec::offsetData, Blob>("OFST"));

// ── CELL ─────────────────────────────────────────────────────────────────────
// DATA has 1- and 2-byte variants; XCLL (lighting) has several sizes — both
// kept opaque-tolerant (DATA bound as u16, the u8 variant passes through;
// XCLL bound as Blob).

struct CellRec : RecordExtras {
    std::optional<std::string>          editorId;      // EDID
    std::optional<Lstring>              fullName;      // FULL
    std::optional<uint16_t>             flags;         // DATA (u16 variant)
    std::optional<CellGrid>             grid;          // XCLC (exterior only)
    std::optional<std::vector<uint8_t>> lighting;      // XCLL
    std::optional<std::vector<uint8_t>> occlusion;     // TVDT
    std::optional<std::vector<uint8_t>> maxHeight;     // MHDT
    std::optional<FormRef>              lightingTemplate; // LTMP
    std::optional<float>                waterHeight;   // XCLW
    std::optional<std::string>          waterNoise;    // XNAM
    std::optional<std::vector<FormRef>> regions;       // XCLR
    std::optional<FormRef>              location;      // XLCN
    std::optional<FormRef>              water;         // XCWT
    std::optional<FormRef>              owner;         // XOWN
    std::optional<FormRef>              lockList;      // XILL
    std::optional<std::string>          waterEnvMap;   // XWEM
    std::optional<FormRef>              climate;       // XCCM
    std::optional<FormRef>              acousticSpace; // XCAS
    std::optional<FormRef>              encounterZone; // XEZN
    std::optional<FormRef>              music;         // XCMO
    std::optional<FormRef>              imageSpace;    // XCIM

    bool IsExterior() const { return grid.has_value(); }
};

inline const auto kCellSchema = MakeSchema<CellRec>("CELL",
    Field<&CellRec::editorId, ZString>("EDID"),
    Field<&CellRec::fullName, LString>("FULL"),
    Field<&CellRec::flags, U16v>("DATA"),
    Field<&CellRec::grid, Packed<CellGrid>>("XCLC"),
    Field<&CellRec::lighting, Blob>("XCLL"),
    Field<&CellRec::occlusion, Blob>("TVDT"),
    Field<&CellRec::maxHeight, Blob>("MHDT"),
    Field<&CellRec::lightingTemplate, FormRefCodec>("LTMP"),
    Field<&CellRec::waterHeight, F32>("XCLW"),
    Field<&CellRec::waterNoise, ZString>("XNAM"),
    Field<&CellRec::regions, FormRefArray>("XCLR"),
    Field<&CellRec::location, FormRefCodec>("XLCN"),
    Field<&CellRec::water, FormRefCodec>("XCWT"),
    Field<&CellRec::owner, FormRefCodec>("XOWN"),
    Field<&CellRec::lockList, FormRefCodec>("XILL"),
    Field<&CellRec::waterEnvMap, ZString>("XWEM"),
    Field<&CellRec::climate, FormRefCodec>("XCCM"),
    Field<&CellRec::acousticSpace, FormRefCodec>("XCAS"),
    Field<&CellRec::encounterZone, FormRefCodec>("XEZN"),
    Field<&CellRec::music, FormRefCodec>("XCMO"),
    Field<&CellRec::imageSpace, FormRefCodec>("XCIM"));

// ── REFR — placed object reference ────────────────────────────────────────────
// The long tail of X--- subrecords stays passthrough; the editor's needs are
// base + placement + scale (+ enable parent / teleport for structure).

struct RefrRec : RecordExtras {
    std::optional<std::string>          editorId;      // EDID
    std::optional<Vmad>                 vmad;          // VMAD
    std::optional<FormRef>              base;          // NAME
    std::optional<TeleportDest>         teleport;      // XTEL
    std::optional<FormRef>              lockList;      // XILL? (kept: XLRT below)
    std::optional<EnableParent>         enableParent;  // XESP
    std::optional<FormRef>              owner;         // XOWN
    std::optional<FormRef>              emittedLight;  // XEMI
    std::optional<FormRef>              lightingRef;   // XLIG? — passthrough if odd
    std::optional<float>                scale;         // XSCL
    std::optional<Placement>            placement;     // DATA
};

inline const auto kRefrSchema = MakeSchema<RefrRec>("REFR",
    Field<&RefrRec::editorId, ZString>("EDID"),
    Field<&RefrRec::vmad, VmadCodec>("VMAD"),
    Field<&RefrRec::base, FormRefCodec>("NAME"),
    Field<&RefrRec::teleport, PackedIds<TeleportDest, kTeleportDestIds>>("XTEL"),
    Field<&RefrRec::enableParent, PackedIds<EnableParent, kEnableParentIds>>("XESP"),
    Field<&RefrRec::owner, FormRefCodec>("XOWN"),
    Field<&RefrRec::emittedLight, FormRefCodec>("XEMI"),
    Field<&RefrRec::scale, F32>("XSCL"),
    Field<&RefrRec::placement, Packed<Placement>>("DATA"));

// ── ACHR — placed actor ───────────────────────────────────────────────────────

struct AchrRec : RecordExtras {
    std::optional<std::string>          editorId;      // EDID
    std::optional<Vmad>                 vmad;          // VMAD
    std::optional<FormRef>              base;          // NAME (NPC_)
    std::optional<FormRef>              encounterZone; // XEZN
    std::optional<std::vector<uint8_t>> ragdollData;   // XRGD
    std::optional<std::vector<uint8_t>> ragdollBiped;  // XRGB
    std::optional<int32_t>              levelModifier; // XLCM
    std::optional<FormRef>              owner;         // XOWN
    std::optional<EnableParent>         enableParent;  // XESP
    std::optional<float>                scale;         // XSCL
    std::optional<Placement>            placement;     // DATA
};

struct I32Achr {
    static bool Decode(int32_t& out, SubView s, const SchemaCtx&) {
        if (s.size != 4) return false;
        std::memcpy(&out, s.data, 4);
        return true;
    }
    static void Encode(const int32_t& v, const SchemaCtx&, ByteWriter& w) {
        w.U32(static_cast<uint32_t>(v));
    }
};

inline const auto kAchrSchema = MakeSchema<AchrRec>("ACHR",
    Field<&AchrRec::editorId, ZString>("EDID"),
    Field<&AchrRec::vmad, VmadCodec>("VMAD"),
    Field<&AchrRec::base, FormRefCodec>("NAME"),
    Field<&AchrRec::encounterZone, FormRefCodec>("XEZN"),
    Field<&AchrRec::ragdollData, Blob>("XRGD"),
    Field<&AchrRec::ragdollBiped, Blob>("XRGB"),
    Field<&AchrRec::levelModifier, I32Achr>("XLCM"),
    Field<&AchrRec::owner, FormRefCodec>("XOWN"),
    Field<&AchrRec::enableParent, PackedIds<EnableParent, kEnableParentIds>>("XESP"),
    Field<&AchrRec::scale, F32>("XSCL"),
    Field<&AchrRec::placement, Packed<Placement>>("DATA"));

// ── LAND — terrain tile ───────────────────────────────────────────────────────
// BTXT/ATXT repeat per quadrant/layer; each ATXT is followed by its VTXT alpha
// cells — the pairing is positional via `layout`, exposed by the helpers in
// WorldIndex.

struct LandRec : RecordExtras {
    std::optional<uint32_t>             flags;         // DATA
    std::optional<LandVertexTriples>    normals;       // VNML
    std::optional<LandHeights>          heights;       // VHGT
    std::optional<LandVertexTriples>    colors;        // VCLR
    std::vector<LandBaseTexture>        baseTextures;  // BTXT
    std::vector<LandAlphaTexture>       alphaTextures; // ATXT
    std::vector<std::vector<LandAlphaCell>> alphaCells; // VTXT (parallel to alphaTextures)
};

inline const auto kLandSchema = MakeSchema<LandRec>("LAND",
    Field<&LandRec::flags, U32>("DATA"),
    Field<&LandRec::normals, Packed<LandVertexTriples>>("VNML"),
    Field<&LandRec::heights, Packed<LandHeights>>("VHGT"),
    Field<&LandRec::colors, Packed<LandVertexTriples>>("VCLR"),
    Repeated<&LandRec::baseTextures,
             PackedIds<LandBaseTexture, kLandBaseTextureIds>>("BTXT"),
    Repeated<&LandRec::alphaTextures,
             PackedIds<LandAlphaTexture, kLandAlphaTextureIds>>("ATXT"),
    Repeated<&LandRec::alphaCells, LandAlphaCells>("VTXT"));

// ── LTEX — land texture ───────────────────────────────────────────────────────

struct LtexRec : RecordExtras {
    std::optional<std::string> editorId;    // EDID
    std::optional<FormRef>     textureSet;  // TNAM (TXST)
    std::optional<FormRef>     material;    // MNAM
    std::optional<LtexHavok>   havok;       // HNAM
    std::optional<uint8_t>     specular;    // SNAM
    std::optional<std::vector<FormRef>> grasses; // GNAM? (repeated FormRef)
};

inline const auto kLtexSchema = MakeSchema<LtexRec>("LTEX",
    Field<&LtexRec::editorId, ZString>("EDID"),
    Field<&LtexRec::textureSet, FormRefCodec>("TNAM"),
    Field<&LtexRec::material, FormRefCodec>("MNAM"),
    Field<&LtexRec::havok, Packed<LtexHavok>>("HNAM"),
    Field<&LtexRec::specular, U8v>("SNAM"),
    Field<&LtexRec::grasses, FormRefArray>("GNAM"));

// ── Simple base-object records the ref resolver needs (MODL = model path) ────

struct StatRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::vector<uint8_t>> modelSwap;   // MODS
    std::optional<StatData>             data;        // DNAM
    std::optional<std::vector<uint8_t>> lodNames;    // MNAM
};

inline const auto kStatSchema = MakeSchema<StatRec>("STAT",
    Field<&StatRec::editorId, ZString>("EDID"),
    Field<&StatRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&StatRec::model, ZString>("MODL"),
    Field<&StatRec::modelData, Blob>("MODT"),
    Field<&StatRec::modelSwap, Blob>("MODS"),
    Field<&StatRec::data, PackedIds<StatData, kStatDataIds>>("DNAM"),
    Field<&StatRec::lodNames, Blob>("MNAM"));

// A generic "modeled object" shape shared by MSTT / TREE / ACTI / CONT / DOOR /
// LIGH / FURN for the fields the editor's ref resolver needs (EDID + MODL).
// Everything else passes through; deeper typed schemas come with their waves.
struct ModeledRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::vector<uint8_t>> modelSwap;   // MODS
};

inline const auto kMsttSchema = MakeSchema<ModeledRec>("MSTT",
    Field<&ModeledRec::editorId, ZString>("EDID"),
    Field<&ModeledRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ModeledRec::fullName, LString>("FULL"),
    Field<&ModeledRec::model, ZString>("MODL"),
    Field<&ModeledRec::modelData, Blob>("MODT"),
    Field<&ModeledRec::modelSwap, Blob>("MODS"));

inline const auto kTreeSchema = MakeSchema<ModeledRec>("TREE",
    Field<&ModeledRec::editorId, ZString>("EDID"),
    Field<&ModeledRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ModeledRec::fullName, LString>("FULL"),
    Field<&ModeledRec::model, ZString>("MODL"),
    Field<&ModeledRec::modelData, Blob>("MODT"),
    Field<&ModeledRec::modelSwap, Blob>("MODS"));

inline const auto kActiSchema = MakeSchema<ModeledRec>("ACTI",
    Field<&ModeledRec::editorId, ZString>("EDID"),
    Field<&ModeledRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ModeledRec::fullName, LString>("FULL"),
    Field<&ModeledRec::model, ZString>("MODL"),
    Field<&ModeledRec::modelData, Blob>("MODT"),
    Field<&ModeledRec::modelSwap, Blob>("MODS"));

inline const auto kContSchema = MakeSchema<ModeledRec>("CONT",
    Field<&ModeledRec::editorId, ZString>("EDID"),
    Field<&ModeledRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ModeledRec::fullName, LString>("FULL"),
    Field<&ModeledRec::model, ZString>("MODL"),
    Field<&ModeledRec::modelData, Blob>("MODT"),
    Field<&ModeledRec::modelSwap, Blob>("MODS"));

inline const auto kDoorSchema = MakeSchema<ModeledRec>("DOOR",
    Field<&ModeledRec::editorId, ZString>("EDID"),
    Field<&ModeledRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ModeledRec::fullName, LString>("FULL"),
    Field<&ModeledRec::model, ZString>("MODL"),
    Field<&ModeledRec::modelData, Blob>("MODT"),
    Field<&ModeledRec::modelSwap, Blob>("MODS"));

inline const auto kLighSchema = MakeSchema<ModeledRec>("LIGH",
    Field<&ModeledRec::editorId, ZString>("EDID"),
    Field<&ModeledRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ModeledRec::fullName, LString>("FULL"),
    Field<&ModeledRec::model, ZString>("MODL"),
    Field<&ModeledRec::modelData, Blob>("MODT"),
    Field<&ModeledRec::modelSwap, Blob>("MODS"));

inline const auto kFurnSchema = MakeSchema<ModeledRec>("FURN",
    Field<&ModeledRec::editorId, ZString>("EDID"),
    Field<&ModeledRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ModeledRec::fullName, LString>("FULL"),
    Field<&ModeledRec::model, ZString>("MODL"),
    Field<&ModeledRec::modelData, Blob>("MODT"),
    Field<&ModeledRec::modelSwap, Blob>("MODS"));

} // namespace esper
