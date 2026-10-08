#pragma once
#include <ESPER/Conditions.h>
#include <ESPER/RecordsCommon.h>

namespace esper {

// Wave 6 — world/system remainder. GMST's DATA is a bare typed value whose
// kind (int/float/string/bool) is encoded in the EDID's first character, so it
// binds as Blob (byte-faithful, no FormIDs). NAVM/NAVI geometry stays
// passthrough (opaque blobs, nothing consumes them). REGN/WTHR/CLMT parameter
// soup binds the identity + FormID edges and passes the rest through.

#pragma pack(push, 1)

// KYWD/CLFM CNAM — color, 4 bytes.
struct ColorRgba {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};
static_assert(sizeof(ColorRgba) == 4);

// LCTN — location; ACPR/LCEP-style entries stay passthrough (packed refs +
// cell coords with conditional FormIDs).

// MOVT SPED — movement speeds, 44 bytes (11 floats).
struct MovementSpeeds {
    float values[11] = {};
};
static_assert(sizeof(MovementSpeeds) == 44);

// GRAS DATA — grass data, 32 bytes.
struct GrassData {
    uint8_t  density = 0;
    uint8_t  minSlope = 0;
    uint8_t  maxSlope = 0;
    uint8_t  pad = 0;
    uint16_t distanceFromWater = 0;
    uint16_t pad2 = 0;
    uint32_t waterOp = 0;
    float    positionRange = 0.f;
    float    heightRange = 0.f;
    float    colorRange = 0.f;
    float    wavePeriod = 0.f;
    uint8_t  flags = 0;
    uint8_t  pad3[3] = {};
};
static_assert(sizeof(GrassData) == 32);

#pragma pack(pop)

// ── GMST / GLOB / KYWD / LCRT / AACT / CLFM / VTYP / EQUP / COLL ──────────────

struct GmstRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> data;     // DATA (typed by EDID prefix)
};

inline const auto kGmstSchema = MakeSchema<GmstRec>("GMST",
    Field<&GmstRec::editorId, ZString>("EDID"),
    Field<&GmstRec::data, Blob>("DATA"));

struct GlobRec : RecordExtras {
    std::optional<std::string> editorId; // EDID
    std::optional<uint8_t>     type;     // FNAM (s/l/f)
    std::optional<float>       value;    // FLTV
};

inline const auto kGlobSchema = MakeSchema<GlobRec>("GLOB",
    Field<&GlobRec::editorId, ZString>("EDID"),
    Field<&GlobRec::type, U8v>("FNAM"),
    Field<&GlobRec::value, F32>("FLTV"));

struct KywdRec : RecordExtras {
    std::optional<std::string> editorId; // EDID
    std::optional<ColorRgba>   color;    // CNAM
};

// KYWD / LCRT / AACT share the exact shape (EDID + optional CNAM color).
namespace schema_detail {
template <uint32_t /*tag*/>
inline auto MakeKeywordSchema(const char sig[5]) {
    return MakeSchema<KywdRec>(sig,
        Field<&KywdRec::editorId, ZString>("EDID"),
        Field<&KywdRec::color, Packed<ColorRgba>>("CNAM"));
}
} // namespace schema_detail

inline const auto kKywdSchema = schema_detail::MakeKeywordSchema<0>("KYWD");
inline const auto kLcrtSchema = schema_detail::MakeKeywordSchema<1>("LCRT");
inline const auto kAactSchema = schema_detail::MakeKeywordSchema<2>("AACT");

struct ClfmRec : RecordExtras {
    std::optional<std::string> editorId;  // EDID
    std::optional<Lstring>     fullName;  // FULL
    std::optional<ColorRgba>   color;     // CNAM
    std::optional<uint32_t>    playable;  // FNAM
};

inline const auto kClfmSchema = MakeSchema<ClfmRec>("CLFM",
    Field<&ClfmRec::editorId, ZString>("EDID"),
    Field<&ClfmRec::fullName, LString>("FULL"),
    Field<&ClfmRec::color, Packed<ColorRgba>>("CNAM"),
    Field<&ClfmRec::playable, U32>("FNAM"));

struct VtypRec : RecordExtras {
    std::optional<std::string> editorId; // EDID
    std::optional<uint8_t>     flags;    // DNAM
};

inline const auto kVtypSchema = MakeSchema<VtypRec>("VTYP",
    Field<&VtypRec::editorId, ZString>("EDID"),
    Field<&VtypRec::flags, U8v>("DNAM"));

struct EqupRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<FormRef>> slotParents; // PNAM (array)
    std::optional<std::vector<uint8_t>> data;     // DATA (flags)
};

inline const auto kEqupSchema = MakeSchema<EqupRec>("EQUP",
    Field<&EqupRec::editorId, ZString>("EDID"),
    Field<&EqupRec::slotParents, FormRefArray>("PNAM"),
    Field<&EqupRec::data, Blob>("DATA"));

struct CollRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Lstring>              description; // DESC
    std::optional<std::vector<uint8_t>> data;        // BNAM/FNAM/etc via passthrough
    std::optional<uint32_t>             index;       // BNAM
    std::optional<ColorRgba>            debugColor;  // FNAM
    std::optional<uint32_t>             flags;       // GNAM
    std::optional<std::string>          name;        // MNAM
    std::optional<std::vector<FormRef>> collidesWith;// CNAM (interactables array)
};

inline const auto kCollSchema = MakeSchema<CollRec>("COLL",
    Field<&CollRec::editorId, ZString>("EDID"),
    Field<&CollRec::description, LStringDL>("DESC"),
    Field<&CollRec::index, U32>("BNAM"),
    Field<&CollRec::debugColor, Packed<ColorRgba>>("FNAM"),
    Field<&CollRec::flags, U32>("GNAM"),
    Field<&CollRec::name, ZString>("MNAM"),
    Field<&CollRec::collidesWith, FormRefArray>("CNAM"),
    Field<&CollRec::data, Blob>("INTV"));

// ── FLST / DOBJ ──────────────────────────────────────────────────────────────

struct FlstRec : RecordExtras {
    std::optional<std::string> editorId; // EDID
    std::vector<FormRef>       items;    // LNAM (repeated)
};

inline const auto kFlstSchema = MakeSchema<FlstRec>("FLST",
    Field<&FlstRec::editorId, ZString>("EDID"),
    Repeated<&FlstRec::items, FormRefCodec>("LNAM"));

// DOBJ DNAM pairs (keyword FourCC + FormID) stay passthrough — the FourCC
// half is not a FormID, so a blanket visitor would corrupt it.
struct DobjRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> pairs;    // DNAM
};

inline const auto kDobjSchema = MakeSchema<DobjRec>("DOBJ",
    Field<&DobjRec::editorId, ZString>("EDID"),
    Field<&DobjRec::pairs, Blob>("DNAM"));

// ── IDLE / IDLM / MOVT / MOVT ────────────────────────────────────────────────

struct IdleRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::optional<std::string>          animEvent;  // ENAM
    std::optional<std::vector<FormRef>> related;    // ANAM (parent + previous)
    std::optional<std::vector<uint8_t>> data;       // DATA (loop/flags)
    ConditionList                       conds;      // CTDA/CIS1/CIS2
};

inline const auto kIdleSchema = MakeSchema<IdleRec>("IDLE",
    Field<&IdleRec::editorId, ZString>("EDID"),
    Field<&IdleRec::animEvent, ZString>("ENAM"),
    Field<&IdleRec::related, FormRefArray>("ANAM"),
    Field<&IdleRec::data, Blob>("DATA"),
    CtdaRow<&IdleRec::conds>(),
    Cis1Row<&IdleRec::conds>(),
    Cis2Row<&IdleRec::conds>());

struct IdlmRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::optional<ObjectBounds>         bounds;     // OBND
    std::optional<uint8_t>              flags;      // IDLF
    std::optional<uint8_t>              animCount;  // IDLC
    std::optional<float>                timerSetting; // IDLT
    std::optional<std::vector<FormRef>> animations; // IDLA (array)
};

inline const auto kIdlmSchema = MakeSchema<IdlmRec>("IDLM",
    Field<&IdlmRec::editorId, ZString>("EDID"),
    Field<&IdlmRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&IdlmRec::flags, U8v>("IDLF"),
    Field<&IdlmRec::animCount, U8v>("IDLC"),
    Field<&IdlmRec::timerSetting, F32>("IDLT"),
    Field<&IdlmRec::animations, FormRefArray>("IDLA"));

struct MovtRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::string>          mnam;     // MNAM (name)
    std::optional<MovementSpeeds>       speeds;   // SPED
    std::optional<std::vector<uint8_t>> rotation; // INAM
};

inline const auto kMovtSchema = MakeSchema<MovtRec>("MOVT",
    Field<&MovtRec::editorId, ZString>("EDID"),
    Field<&MovtRec::mnam, ZString>("MNAM"),
    Field<&MovtRec::speeds, Packed<MovementSpeeds>>("SPED"),
    Field<&MovtRec::rotation, Blob>("INAM"));

// ── GRAS ─────────────────────────────────────────────────────────────────────

struct GrasRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<ObjectBounds>         bounds;    // OBND
    std::optional<std::string>          model;     // MODL
    std::optional<std::vector<uint8_t>> modelData; // MODT
    std::optional<GrassData>            data;      // DATA
};

inline const auto kGrasSchema = MakeSchema<GrasRec>("GRAS",
    Field<&GrasRec::editorId, ZString>("EDID"),
    Field<&GrasRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&GrasRec::model, ZString>("MODL"),
    Field<&GrasRec::modelData, Blob>("MODT"),
    Field<&GrasRec::data, Packed<GrassData>>("DATA"));

// ── ECZN / LCTN / WOOP-adjacent ──────────────────────────────────────────────

struct EcznRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> data;     // DATA (owner+location FormIDs, size-variant)
};

// ECZN DATA embeds two FormIDs but has a legacy short variant → passthrough.
inline const auto kEcznSchema = MakeSchema<EcznRec>("ECZN",
    Field<&EcznRec::editorId, ZString>("EDID"),
    Field<&EcznRec::data, Blob>("DATA"));

struct LctnRec : RecordExtras {
    std::optional<std::string> editorId;      // EDID
    std::optional<Lstring>     fullName;      // FULL
    std::optional<FormRef>     parentLocation;// PNAM
    std::optional<FormRef>     music;         // NAM1
    std::optional<FormRef>     unreported;    // FNAM
    std::optional<FormRef>     worldLocMarker;// MNAM
    std::optional<float>       worldLocRadius;// RNAM
};

inline const auto kLctnSchema = MakeSchema<LctnRec>("LCTN",
    Field<&LctnRec::editorId, ZString>("EDID"),
    Field<&LctnRec::fullName, LString>("FULL"),
    Field<&LctnRec::parentLocation, FormRefCodec>("PNAM"),
    Field<&LctnRec::music, FormRefCodec>("NAM1"),
    Field<&LctnRec::unreported, FormRefCodec>("FNAM"),
    Field<&LctnRec::worldLocMarker, FormRefCodec>("MNAM"),
    Field<&LctnRec::worldLocRadius, F32>("RNAM"));

// ── WTHR / CLMT / WATR / LGTM (identity + FormID edges; params passthrough) ──

struct ClmtRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::vector<std::vector<uint8_t>>   weatherList; // WLST (repeated triples)
    std::optional<std::string>          sunTexture;  // FNAM
    std::optional<std::string>          sunGlare;    // GNAM
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::vector<uint8_t>> timing;      // TNAM
};

struct ClmtBlob {
    static bool Decode(std::vector<uint8_t>& out, SubView s, const SchemaCtx&) {
        out.assign(s.data, s.data + s.size);
        return true;
    }
    static void Encode(const std::vector<uint8_t>& v, const SchemaCtx&, ByteWriter& w) {
        if (!v.empty()) w.Bytes(v.data(), v.size());
    }
};

// WLST holds FormIDs but interleaved with chance ints → passthrough (Blob).
inline const auto kClmtSchema = MakeSchema<ClmtRec>("CLMT",
    Field<&ClmtRec::editorId, ZString>("EDID"),
    Repeated<&ClmtRec::weatherList, ClmtBlob>("WLST"),
    Field<&ClmtRec::sunTexture, ZString>("FNAM"),
    Field<&ClmtRec::sunGlare, ZString>("GNAM"),
    Field<&ClmtRec::model, ZString>("MODL"),
    Field<&ClmtRec::modelData, Blob>("MODT"),
    Field<&ClmtRec::timing, Blob>("TNAM"));

struct WatrRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<Lstring>              fullName;  // FULL
    std::vector<std::string>            noiseTextures; // NNAM
    std::optional<uint8_t>              opacity;   // ANAM
    std::optional<uint8_t>              flags;     // FNAM
    std::optional<FormRef>              material;  // TNAM? kept for edges
    std::optional<FormRef>              openSound; // SNAM
    std::optional<FormRef>              spell;     // XNAM
    std::optional<FormRef>              imageSpace;// INAM
    std::optional<std::vector<uint8_t>> data;      // DNAM (param block)
};

inline const auto kWatrSchema = MakeSchema<WatrRec>("WATR",
    Field<&WatrRec::editorId, ZString>("EDID"),
    Field<&WatrRec::fullName, LString>("FULL"),
    Repeated<&WatrRec::noiseTextures, ZString>("NNAM"),
    Field<&WatrRec::opacity, U8v>("ANAM"),
    Field<&WatrRec::flags, U8v>("FNAM"),
    Field<&WatrRec::openSound, FormRefCodec>("SNAM"),
    Field<&WatrRec::spell, FormRefCodec>("XNAM"),
    Field<&WatrRec::imageSpace, FormRefCodec>("INAM"),
    Field<&WatrRec::data, Blob>("DNAM"));

} // namespace esper
