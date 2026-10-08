#pragma once
#include <ESPER/Conditions.h>
#include <ESPER/RecordsCommon.h>

namespace esper {

// Wave 5 — audio / visual / fx. Parameter blocks that carry no FormIDs bind
// as Blob (consumed + byte-faithful); blocks whose FormID positions vary by
// size or context (EXPL/PROJ DATA, DEBR entries, IMAD's keyframe sig soup)
// stay passthrough, which keeps remap refusal automatic for them.

#pragma pack(push, 1)

// IPCT DATA — impact data, 24 bytes.
struct ImpactData {
    float    effectDuration = 0.f;
    uint32_t orientation = 0;
    float    angleThreshold = 0.f;
    float    placementRadius = 0.f;
    uint32_t soundLevel = 0;
    uint8_t  flags = 0;
    uint8_t  impactResult = 0;
    uint16_t pad = 0;
};
static_assert(sizeof(ImpactData) == 24);

// IPDS PNAM — material → impact mapping, 8 bytes.
struct ImpactMapping {
    uint32_t material = 0;           // MATT FormID
    uint32_t impact   = 0;           // IPCT FormID
};
static_assert(sizeof(ImpactMapping) == 8);
inline constexpr FieldDef kImpactMappingIds[] = { FormIdAt(0), FormIdAt(4) };

// RFCT DATA — visual effect, 12 bytes.
struct VisualEffectData {
    uint32_t effectArt = 0;          // ARTO FormID
    uint32_t shader    = 0;          // EFSH FormID
    uint32_t flags     = 0;
};
static_assert(sizeof(VisualEffectData) == 12);
inline constexpr FieldDef kVisualEffectIds[] = { FormIdAt(0), FormIdAt(4) };

// ADDN DNAM — addon node flags, 4 bytes.
struct AddonFlags {
    uint16_t masterParticleCap = 0;
    uint16_t flags = 0;
};
static_assert(sizeof(AddonFlags) == 4);

// MUSC PNAM — priority/ducking, 4 bytes.
struct MusicPriority {
    uint16_t priority = 0;
    uint16_t ducking  = 0;           // dB ×100
};
static_assert(sizeof(MusicPriority) == 4);

#pragma pack(pop)

// ── SOUN / SNDR / SOPM ───────────────────────────────────────────────────────

struct SounRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::optional<ObjectBounds>         bounds;     // OBND
    std::optional<std::string>          soundFile;  // FNAM (legacy)
    std::optional<std::vector<uint8_t>> soundData;  // SNDD (legacy)
    std::optional<FormRef>              descriptor; // SDSC (SNDR)
};

inline const auto kSounSchema = MakeSchema<SounRec>("SOUN",
    Field<&SounRec::editorId, ZString>("EDID"),
    Field<&SounRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&SounRec::soundFile, ZString>("FNAM"),
    Field<&SounRec::soundData, Blob>("SNDD"),
    Field<&SounRec::descriptor, FormRefCodec>("SDSC"));

struct SndrRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::optional<uint32_t>             type;       // CNAM
    std::optional<FormRef>              category;   // GNAM (SNCT)
    std::optional<FormRef>              altSound;   // SNAM
    std::vector<std::string>            soundFiles; // ANAM
    std::optional<std::vector<uint8_t>> outputData; // ONAM
    std::optional<uint32_t>             flags;      // LNAM
    std::optional<std::vector<uint8_t>> values;     // BNAM (loop/rumble values)
    std::optional<FormRef>              outputModel;// ODTY? kept via passthrough
    ConditionList                       conds;      // CTDA/CIS1/CIS2
};

inline const auto kSndrSchema = MakeSchema<SndrRec>("SNDR",
    Field<&SndrRec::editorId, ZString>("EDID"),
    Field<&SndrRec::type, U32>("CNAM"),
    Field<&SndrRec::category, FormRefCodec>("GNAM"),
    Field<&SndrRec::altSound, FormRefCodec>("SNAM"),
    Repeated<&SndrRec::soundFiles, ZString>("ANAM"),
    Field<&SndrRec::outputData, Blob>("ONAM"),
    Field<&SndrRec::flags, U32>("LNAM"),
    Field<&SndrRec::values, Blob>("BNAM"),
    CtdaRow<&SndrRec::conds>(),
    Cis1Row<&SndrRec::conds>(),
    Cis2Row<&SndrRec::conds>());

struct SopmRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<std::vector<uint8_t>> data;        // NAM1
    std::optional<uint32_t>             type;        // MNAM
    std::optional<std::vector<uint8_t>> channels;    // ONAM (output channels)
    std::optional<std::vector<uint8_t>> attenuation; // ANAM
};

inline const auto kSopmSchema = MakeSchema<SopmRec>("SOPM",
    Field<&SopmRec::editorId, ZString>("EDID"),
    Field<&SopmRec::data, Blob>("NAM1"),
    Field<&SopmRec::type, U32>("MNAM"),
    Field<&SopmRec::channels, Blob>("ONAM"),
    Field<&SopmRec::attenuation, Blob>("ANAM"));

// ── MUSC / MUST / REVB ───────────────────────────────────────────────────────

struct MuscRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<uint32_t>             flags;     // FNAM
    std::optional<MusicPriority>        priority;  // PNAM
    std::optional<float>                fade;      // WNAM
    std::optional<std::vector<FormRef>> tracks;    // TNAM (array in one sub)
};

inline const auto kMuscSchema = MakeSchema<MuscRec>("MUSC",
    Field<&MuscRec::editorId, ZString>("EDID"),
    Field<&MuscRec::flags, U32>("FNAM"),
    Field<&MuscRec::priority, Packed<MusicPriority>>("PNAM"),
    Field<&MuscRec::fade, F32>("WNAM"),
    Field<&MuscRec::tracks, FormRefArray>("TNAM"));

struct MustRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::optional<uint32_t>             type;       // CNAM
    std::optional<float>                duration;   // FLTV
    std::optional<float>                fadeOut;    // DNAM
    std::optional<std::string>          trackPath;  // ANAM
    std::optional<std::string>          finalePath; // BNAM
    std::optional<std::vector<uint8_t>> cuePoints;  // FNAM (f32 array)
    std::optional<std::vector<uint8_t>> loopData;   // LNAM
    std::optional<uint32_t>             conditionCount; // CITC
    std::optional<std::vector<FormRef>> subTracks;  // SNAM (array in one sub)
    ConditionList                       conds;      // CTDA/CIS1/CIS2
};

inline const auto kMustSchema = MakeSchema<MustRec>("MUST",
    Field<&MustRec::editorId, ZString>("EDID"),
    Field<&MustRec::type, U32>("CNAM"),
    Field<&MustRec::duration, F32>("FLTV"),
    Field<&MustRec::fadeOut, F32>("DNAM"),
    Field<&MustRec::trackPath, ZString>("ANAM"),
    Field<&MustRec::finalePath, ZString>("BNAM"),
    Field<&MustRec::cuePoints, Blob>("FNAM"),
    Field<&MustRec::loopData, Blob>("LNAM"),
    Field<&MustRec::conditionCount, U32>("CITC"),
    Field<&MustRec::subTracks, FormRefArray>("SNAM"),
    CtdaRow<&MustRec::conds>(),
    Cis1Row<&MustRec::conds>(),
    Cis2Row<&MustRec::conds>());

struct RevbRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> data;     // DATA (reverb params)
};

inline const auto kRevbSchema = MakeSchema<RevbRec>("REVB",
    Field<&RevbRec::editorId, ZString>("EDID"),
    Field<&RevbRec::data, Blob>("DATA"));

// ── IMGS (image space) — parameter blocks only, no FormIDs ───────────────────

struct ImgsRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> enam;     // ENAM
    std::optional<std::vector<uint8_t>> hdr;      // HNAM
    std::optional<std::vector<uint8_t>> cinematic;// CNAM
    std::optional<std::vector<uint8_t>> tint;     // TNAM
    std::optional<std::vector<uint8_t>> dof;      // DNAM
};

inline const auto kImgsSchema = MakeSchema<ImgsRec>("IMGS",
    Field<&ImgsRec::editorId, ZString>("EDID"),
    Field<&ImgsRec::enam, Blob>("ENAM"),
    Field<&ImgsRec::hdr, Blob>("HNAM"),
    Field<&ImgsRec::cinematic, Blob>("CNAM"),
    Field<&ImgsRec::tint, Blob>("TNAM"),
    Field<&ImgsRec::dof, Blob>("DNAM"));

// IMAD's keyframe sections use dozens of context-dependent signatures —
// EDID-only shell; the rest rides through as passthrough.
struct ImadRec : RecordExtras {
    std::optional<std::string> editorId;  // EDID
};

inline const auto kImadSchema = MakeSchema<ImadRec>("IMAD",
    Field<&ImadRec::editorId, ZString>("EDID"));

// ── EFSH / EXPL / PROJ ───────────────────────────────────────────────────────

struct EfshRec : RecordExtras {
    std::optional<std::string>          editorId;        // EDID
    std::optional<std::string>          fillTexture;     // ICON
    std::optional<std::string>          particleTexture; // ICO2
    std::optional<std::string>          holesTexture;    // NAM7
    std::optional<std::string>          membranePalette; // NAM8
    std::optional<std::string>          particlePalette; // NAM9
    std::optional<std::vector<uint8_t>> data;            // DATA (param block)
};

inline const auto kEfshSchema = MakeSchema<EfshRec>("EFSH",
    Field<&EfshRec::editorId, ZString>("EDID"),
    Field<&EfshRec::fillTexture, ZString>("ICON"),
    Field<&EfshRec::particleTexture, ZString>("ICO2"),
    Field<&EfshRec::holesTexture, ZString>("NAM7"),
    Field<&EfshRec::membranePalette, ZString>("NAM8"),
    Field<&EfshRec::particlePalette, ZString>("NAM9"),
    Field<&EfshRec::data, Blob>("DATA"));

// EXPL/PROJ DATA blocks have size variants with embedded FormIDs → passthrough.
struct ExplRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::optional<ObjectBounds>         bounds;     // OBND
    std::optional<Lstring>              fullName;   // FULL
    std::optional<std::string>          model;      // MODL
    std::optional<std::vector<uint8_t>> modelData;  // MODT
    std::optional<FormRef>              enchantment;// EITM
    std::optional<FormRef>              imageSpaceMod; // MNAM
};

inline const auto kExplSchema = MakeSchema<ExplRec>("EXPL",
    Field<&ExplRec::editorId, ZString>("EDID"),
    Field<&ExplRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ExplRec::fullName, LString>("FULL"),
    Field<&ExplRec::model, ZString>("MODL"),
    Field<&ExplRec::modelData, Blob>("MODT"),
    Field<&ExplRec::enchantment, FormRefCodec>("EITM"),
    Field<&ExplRec::imageSpaceMod, FormRefCodec>("MNAM"));

struct ProjRec : RecordExtras {
    std::optional<std::string>          editorId;     // EDID
    std::optional<ObjectBounds>         bounds;       // OBND
    std::optional<Lstring>              fullName;     // FULL
    std::optional<std::string>          model;        // MODL
    std::optional<std::vector<uint8_t>> modelData;    // MODT
    std::optional<std::string>          muzzleModel;  // NAM1
    std::optional<std::vector<uint8_t>> muzzleData;   // NAM2
    std::optional<uint32_t>             soundLevel;   // VNAM
};

inline const auto kProjSchema = MakeSchema<ProjRec>("PROJ",
    Field<&ProjRec::editorId, ZString>("EDID"),
    Field<&ProjRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ProjRec::fullName, LString>("FULL"),
    Field<&ProjRec::model, ZString>("MODL"),
    Field<&ProjRec::modelData, Blob>("MODT"),
    Field<&ProjRec::muzzleModel, ZString>("NAM1"),
    Field<&ProjRec::muzzleData, Blob>("NAM2"),
    Field<&ProjRec::soundLevel, U32>("VNAM"));

// ── IPCT / IPDS ──────────────────────────────────────────────────────────────

struct IpctRec : RecordExtras {
    std::optional<std::string>          editorId;     // EDID
    std::optional<std::string>          model;        // MODL
    std::optional<std::vector<uint8_t>> modelData;    // MODT
    std::optional<ImpactData>           data;         // DATA
    std::optional<DecalData>            decal;        // DODT
    std::optional<FormRef>              textureSet;   // DNAM
    std::optional<FormRef>              secondaryTextureSet; // ENAM
    std::optional<FormRef>              sound1;       // SNAM
    std::optional<FormRef>              sound2;       // NAM1
    std::optional<FormRef>              hazard;       // NAM2
};

inline const auto kIpctSchema = MakeSchema<IpctRec>("IPCT",
    Field<&IpctRec::editorId, ZString>("EDID"),
    Field<&IpctRec::model, ZString>("MODL"),
    Field<&IpctRec::modelData, Blob>("MODT"),
    Field<&IpctRec::data, Packed<ImpactData>>("DATA"),
    Field<&IpctRec::decal, Packed<DecalData>>("DODT"),
    Field<&IpctRec::textureSet, FormRefCodec>("DNAM"),
    Field<&IpctRec::secondaryTextureSet, FormRefCodec>("ENAM"),
    Field<&IpctRec::sound1, FormRefCodec>("SNAM"),
    Field<&IpctRec::sound2, FormRefCodec>("NAM1"),
    Field<&IpctRec::hazard, FormRefCodec>("NAM2"));

struct IpdsRec : RecordExtras {
    std::optional<std::string> editorId;   // EDID
    std::vector<ImpactMapping> mappings;   // PNAM
};

inline const auto kIpdsSchema = MakeSchema<IpdsRec>("IPDS",
    Field<&IpdsRec::editorId, ZString>("EDID"),
    Repeated<&IpdsRec::mappings,
             PackedIds<ImpactMapping, kImpactMappingIds>>("PNAM"));

// ── CAMS / CPTH ──────────────────────────────────────────────────────────────

struct CamsRec : RecordExtras {
    std::optional<std::string>          editorId;      // EDID
    std::optional<std::string>          model;         // MODL
    std::optional<std::vector<uint8_t>> modelData;     // MODT
    std::optional<std::vector<uint8_t>> data;          // DATA (floats/enums)
    std::optional<FormRef>              imageSpaceMod; // MNAM
};

inline const auto kCamsSchema = MakeSchema<CamsRec>("CAMS",
    Field<&CamsRec::editorId, ZString>("EDID"),
    Field<&CamsRec::model, ZString>("MODL"),
    Field<&CamsRec::modelData, Blob>("MODT"),
    Field<&CamsRec::data, Blob>("DATA"),
    Field<&CamsRec::imageSpaceMod, FormRefCodec>("MNAM"));

struct CpthRec : RecordExtras {
    std::optional<std::string>          editorId;     // EDID
    std::optional<std::vector<FormRef>> relatedPaths; // ANAM (parent + previous)
    std::optional<uint8_t>              zoom;         // DATA
    std::vector<FormRef>                cameraShots;  // SNAM
    ConditionList                       conds;        // CTDA/CIS1/CIS2
};

inline const auto kCpthSchema = MakeSchema<CpthRec>("CPTH",
    Field<&CpthRec::editorId, ZString>("EDID"),
    Field<&CpthRec::relatedPaths, FormRefArray>("ANAM"),
    Field<&CpthRec::zoom, U8v>("DATA"),
    Repeated<&CpthRec::cameraShots, FormRefCodec>("SNAM"),
    CtdaRow<&CpthRec::conds>(),
    Cis1Row<&CpthRec::conds>(),
    Cis2Row<&CpthRec::conds>());

// ── DEBR / RFCT / ADDN / ARTO / MATO ─────────────────────────────────────────

// DEBR DATA entries mix a count byte, an inline path, and flags — passthrough.
struct DebrRec : RecordExtras {
    std::optional<std::string> editorId;  // EDID
};

inline const auto kDebrSchema = MakeSchema<DebrRec>("DEBR",
    Field<&DebrRec::editorId, ZString>("EDID"));

struct RfctRec : RecordExtras {
    std::optional<std::string>      editorId; // EDID
    std::optional<VisualEffectData> data;     // DATA
};

inline const auto kRfctSchema = MakeSchema<RfctRec>("RFCT",
    Field<&RfctRec::editorId, ZString>("EDID"),
    Field<&RfctRec::data, PackedIds<VisualEffectData, kVisualEffectIds>>("DATA"));

struct AddnRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<ObjectBounds>         bounds;    // OBND
    std::optional<std::string>          model;     // MODL
    std::optional<std::vector<uint8_t>> modelData; // MODT
    std::optional<int32_t>              nodeIndex; // DATA
    std::optional<FormRef>              sound;     // SNAM
    std::optional<AddonFlags>           flags;     // DNAM
};

struct I32Fx {
    static bool Decode(int32_t& out, SubView s, const SchemaCtx&) {
        if (s.size != 4) return false;
        std::memcpy(&out, s.data, 4);
        return true;
    }
    static void Encode(const int32_t& v, const SchemaCtx&, ByteWriter& w) {
        w.U32(static_cast<uint32_t>(v));
    }
};

inline const auto kAddnSchema = MakeSchema<AddnRec>("ADDN",
    Field<&AddnRec::editorId, ZString>("EDID"),
    Field<&AddnRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&AddnRec::model, ZString>("MODL"),
    Field<&AddnRec::modelData, Blob>("MODT"),
    Field<&AddnRec::nodeIndex, I32Fx>("DATA"),
    Field<&AddnRec::sound, FormRefCodec>("SNAM"),
    Field<&AddnRec::flags, Packed<AddonFlags>>("DNAM"));

struct ArtoRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<ObjectBounds>         bounds;    // OBND
    std::optional<std::string>          model;     // MODL
    std::optional<std::vector<uint8_t>> modelData; // MODT
    std::optional<std::vector<uint8_t>> modelSwap; // MODS
    std::optional<uint32_t>             artType;   // DNAM
};

inline const auto kArtoSchema = MakeSchema<ArtoRec>("ARTO",
    Field<&ArtoRec::editorId, ZString>("EDID"),
    Field<&ArtoRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ArtoRec::model, ZString>("MODL"),
    Field<&ArtoRec::modelData, Blob>("MODT"),
    Field<&ArtoRec::modelSwap, Blob>("MODS"),
    Field<&ArtoRec::artType, U32>("DNAM"));

struct MatoRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::optional<std::string>          model;      // MODL
    std::vector<std::vector<uint8_t>>   properties; // DNAM (repeated blocks)
    std::optional<std::vector<uint8_t>> data;       // DATA (directional material)
};

struct MatoBlob {
    static bool Decode(std::vector<uint8_t>& out, SubView s, const SchemaCtx&) {
        out.assign(s.data, s.data + s.size);
        return true;
    }
    static void Encode(const std::vector<uint8_t>& v, const SchemaCtx&, ByteWriter& w) {
        if (!v.empty()) w.Bytes(v.data(), v.size());
    }
};

inline const auto kMatoSchema = MakeSchema<MatoRec>("MATO",
    Field<&MatoRec::editorId, ZString>("EDID"),
    Field<&MatoRec::model, ZString>("MODL"),
    Repeated<&MatoRec::properties, MatoBlob>("DNAM"),
    Field<&MatoRec::data, Blob>("DATA"));

} // namespace esper
