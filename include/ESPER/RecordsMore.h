#pragma once
#include <ESPER/Conditions.h>
#include <ESPER/RecordsCommon.h>
#include <ESPER/Vmad.h>

namespace esper {

// Wave 7 — the remaining top-level record roster. Clean identity + FormID
// edges are typed; DATA blocks whose FormID positions vary by size/context,
// and opaque geometry (NAVM/NAVI/BPTD/PGRE/PHZD), stay Blob/passthrough so the
// container's byte-fidelity holds and remap refusal stays automatic. This
// closes the vanilla + AE CC record-type roster: every signature that appears
// is either typed here or in an earlier wave, and none is unhandled.

#pragma pack(push, 1)

// DUAL DATA — dual-cast, 24 bytes (6 FormIDs / flags).
struct DualData {
    uint32_t projectile   = 0;   // FormID
    uint32_t explosion    = 0;   // FormID
    uint32_t effectShader = 0;   // FormID
    uint32_t hitEffectArt = 0;   // FormID
    uint32_t impactData   = 0;   // FormID
    uint32_t inheritFlags = 0;
};
static_assert(sizeof(DualData) == 24);
inline constexpr FieldDef kDualDataIds[] = {
    FormIdAt(0), FormIdAt(4), FormIdAt(8), FormIdAt(12), FormIdAt(16) };

// HAZD DATA — hazard, 40 bytes.
struct HazardData {
    uint32_t limit = 0;
    float    radius = 0.f;
    float    lifetime = 0.f;
    float    imageSpaceRadius = 0.f;
    float    targetInterval = 0.f;
    uint32_t flags = 0;
    uint32_t spell = 0;          // FormID
    uint32_t light = 0;          // FormID
    uint32_t impactDataSet = 0;  // FormID
    uint32_t sound = 0;          // FormID
};
static_assert(sizeof(HazardData) == 40);
inline constexpr FieldDef kHazardDataIds[] = {
    FormIdAt(24), FormIdAt(28), FormIdAt(32), FormIdAt(36) };

#pragma pack(pop)

// ── ANIO / APPA / TACT / FLOR (modeled objects with edges) ───────────────────

struct AnioRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::string>          unloadEvent; // BNAM
};

inline const auto kAnioSchema = MakeSchema<AnioRec>("ANIO",
    Field<&AnioRec::editorId, ZString>("EDID"),
    Field<&AnioRec::model, ZString>("MODL"),
    Field<&AnioRec::modelData, Blob>("MODT"),
    Field<&AnioRec::unloadEvent, ZString>("BNAM"));

struct AppaRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::string>          icon;        // ICON
    std::optional<FormRef>              pickupSound; // YNAM
    std::optional<FormRef>              dropSound;   // ZNAM
    std::optional<uint32_t>             quality;     // QUAL
    std::optional<Lstring>              description; // DESC
    std::optional<ItemValueWeight>      valueWeight; // DATA
};

inline const auto kAppaSchema = MakeSchema<AppaRec>("APPA",
    Field<&AppaRec::editorId, ZString>("EDID"),
    Field<&AppaRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&AppaRec::fullName, LString>("FULL"),
    Field<&AppaRec::model, ZString>("MODL"),
    Field<&AppaRec::modelData, Blob>("MODT"),
    Field<&AppaRec::icon, ZString>("ICON"),
    Field<&AppaRec::pickupSound, FormRefCodec>("YNAM"),
    Field<&AppaRec::dropSound, FormRefCodec>("ZNAM"),
    Field<&AppaRec::quality, U32>("QUAL"),
    Field<&AppaRec::description, LStringDL>("DESC"),
    Field<&AppaRec::valueWeight, Packed<ItemValueWeight>>("DATA"));

struct TactRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Vmad>                 vmad;        // VMAD
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::vector<uint8_t>> modelSwap;   // MODS
    std::optional<FormRef>              pickupSound; // YNAM? kept as edge
    std::optional<uint32_t>             flags;       // FNAM
    std::optional<FormRef>              voiceType;   // VNAM
    std::optional<FormRef>              loopSound;   // SNAM
};

inline const auto kTactSchema = MakeSchema<TactRec>("TACT",
    Field<&TactRec::editorId, ZString>("EDID"),
    Field<&TactRec::vmad, VmadCodec>("VMAD"),
    Field<&TactRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&TactRec::fullName, LString>("FULL"),
    Field<&TactRec::model, ZString>("MODL"),
    Field<&TactRec::modelData, Blob>("MODT"),
    Field<&TactRec::modelSwap, Blob>("MODS"),
    Field<&TactRec::flags, U32>("FNAM"),
    Field<&TactRec::voiceType, FormRefCodec>("VNAM"),
    Field<&TactRec::loopSound, FormRefCodec>("SNAM"));

struct FlorRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Vmad>                 vmad;        // VMAD
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::vector<uint8_t>> modelSwap;   // MODS
    std::optional<uint32_t>             keywordCount;// KSIZ
    std::optional<std::vector<FormRef>> keywords;    // KWDA
    std::optional<Lstring>              activateText;// RNAM
    std::optional<uint16_t>             flags;       // FNAM
    std::optional<FormRef>              ingredient;  // PFIG
    std::optional<FormRef>              harvestSound;// SNAM
    std::optional<std::vector<uint8_t>> seasons;     // PFPC
};

inline const auto kFlorSchema = MakeSchema<FlorRec>("FLOR",
    Field<&FlorRec::editorId, ZString>("EDID"),
    Field<&FlorRec::vmad, VmadCodec>("VMAD"),
    Field<&FlorRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&FlorRec::fullName, LString>("FULL"),
    Field<&FlorRec::model, ZString>("MODL"),
    Field<&FlorRec::modelData, Blob>("MODT"),
    Field<&FlorRec::modelSwap, Blob>("MODS"),
    Field<&FlorRec::keywordCount, U32>("KSIZ"),
    Field<&FlorRec::keywords, FormRefArray>("KWDA"),
    Field<&FlorRec::activateText, LString>("RNAM"),
    Field<&FlorRec::flags, U16v>("FNAM"),
    Field<&FlorRec::ingredient, FormRefCodec>("PFIG"),
    Field<&FlorRec::harvestSound, FormRefCodec>("SNAM"),
    Field<&FlorRec::seasons, Blob>("PFPC"));

// ── COBJ — constructible object ──────────────────────────────────────────────

struct CobjRec : RecordExtras {
    std::optional<std::string>  editorId;    // EDID
    std::optional<uint32_t>     itemCount;   // COCT
    std::vector<ContainerEntry> components;  // CNTO
    std::optional<FormRef>      createdObject; // CNAM
    std::optional<FormRef>      workbench;   // BNAM (keyword)
    std::optional<uint16_t>     outputCount; // NAM1
    ConditionList               conds;       // CTDA/CIS1/CIS2
};

inline const auto kCobjSchema = MakeSchema<CobjRec>("COBJ",
    Field<&CobjRec::editorId, ZString>("EDID"),
    Field<&CobjRec::itemCount, U32>("COCT"),
    Repeated<&CobjRec::components,
             PackedIds<ContainerEntry, kContainerEntryIds>>("CNTO"),
    Field<&CobjRec::createdObject, FormRefCodec>("CNAM"),
    Field<&CobjRec::workbench, FormRefCodec>("BNAM"),
    Field<&CobjRec::outputCount, U16v>("NAM1"),
    CtdaRow<&CobjRec::conds>(),
    Cis1Row<&CobjRec::conds>(),
    Cis2Row<&CobjRec::conds>());

// ── ASTP / MATT / SNCT / DUAL / HAZD ─────────────────────────────────────────

struct AstpRec : RecordExtras {
    std::optional<std::string> editorId;   // EDID
    std::optional<std::string> maleParent; // MPRT
    std::optional<std::string> femaleParent; // FPRT
    std::optional<std::string> maleChild;  // MCHT
    std::optional<std::string> femaleChild;// FCHT
    std::optional<uint32_t>    flags;      // DATA
};

inline const auto kAstpSchema = MakeSchema<AstpRec>("ASTP",
    Field<&AstpRec::editorId, ZString>("EDID"),
    Field<&AstpRec::maleParent, ZString>("MPRT"),
    Field<&AstpRec::femaleParent, ZString>("FPRT"),
    Field<&AstpRec::maleChild, ZString>("MCHT"),
    Field<&AstpRec::femaleChild, ZString>("FCHT"),
    Field<&AstpRec::flags, U32>("DATA"));

struct MattRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<FormRef>              parent;      // PNAM
    std::optional<std::string>          name;        // MNAM
    std::optional<std::vector<uint8_t>> displayColor;// CNAM (3 floats)
    std::optional<float>                buoyancy;    // BNAM
    std::optional<uint32_t>             flags;       // FNAM
    std::optional<FormRef>              havokImpact; // HNAM
};

inline const auto kMattSchema = MakeSchema<MattRec>("MATT",
    Field<&MattRec::editorId, ZString>("EDID"),
    Field<&MattRec::parent, FormRefCodec>("PNAM"),
    Field<&MattRec::name, ZString>("MNAM"),
    Field<&MattRec::displayColor, Blob>("CNAM"),
    Field<&MattRec::buoyancy, F32>("BNAM"),
    Field<&MattRec::flags, U32>("FNAM"),
    Field<&MattRec::havokImpact, FormRefCodec>("HNAM"));

struct SnctRec : RecordExtras {
    std::optional<std::string> editorId; // EDID
    std::optional<Lstring>     fullName; // FULL
    std::optional<uint32_t>    flags;    // FNAM
    std::optional<FormRef>     parent;   // PNAM
    std::optional<uint16_t>    staticVol;// VNAM
    std::optional<uint16_t>    defaultMenuVol; // UNAM
};

inline const auto kSnctSchema = MakeSchema<SnctRec>("SNCT",
    Field<&SnctRec::editorId, ZString>("EDID"),
    Field<&SnctRec::fullName, LString>("FULL"),
    Field<&SnctRec::flags, U32>("FNAM"),
    Field<&SnctRec::parent, FormRefCodec>("PNAM"),
    Field<&SnctRec::staticVol, U16v>("VNAM"),
    Field<&SnctRec::defaultMenuVol, U16v>("UNAM"));

struct DualRec : RecordExtras {
    std::optional<std::string>  editorId; // EDID
    std::optional<ObjectBounds> bounds;   // OBND
    std::optional<DualData>     data;     // DATA
};

inline const auto kDualSchema = MakeSchema<DualRec>("DUAL",
    Field<&DualRec::editorId, ZString>("EDID"),
    Field<&DualRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&DualRec::data, PackedIds<DualData, kDualDataIds>>("DATA"));

struct HazdRec : RecordExtras {
    std::optional<std::string>          editorId;      // EDID
    std::optional<ObjectBounds>         bounds;        // OBND
    std::optional<Lstring>              fullName;      // FULL
    std::optional<std::string>          model;         // MODL
    std::optional<std::vector<uint8_t>> modelData;     // MODT
    std::optional<FormRef>              imageSpaceMod; // MNAM
    std::optional<HazardData>           data;          // DATA
};

inline const auto kHazdSchema = MakeSchema<HazdRec>("HAZD",
    Field<&HazdRec::editorId, ZString>("EDID"),
    Field<&HazdRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&HazdRec::fullName, LString>("FULL"),
    Field<&HazdRec::model, ZString>("MODL"),
    Field<&HazdRec::modelData, Blob>("MODT"),
    Field<&HazdRec::imageSpaceMod, FormRefCodec>("MNAM"),
    Field<&HazdRec::data, PackedIds<HazardData, kHazardDataIds>>("DATA"));

// ── CSTY / LGTM / SPGD (parameter blocks; no FormIDs) ────────────────────────

struct CstyRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> general;  // CSGD
    std::optional<std::vector<uint8_t>> melee;    // CSMD
    std::optional<std::vector<uint8_t>> closeRange; // CSME
    std::optional<std::vector<uint8_t>> flight;   // CSFL
    std::optional<std::vector<uint8_t>> data;     // DATA
};

inline const auto kCstySchema = MakeSchema<CstyRec>("CSTY",
    Field<&CstyRec::editorId, ZString>("EDID"),
    Field<&CstyRec::general, Blob>("CSGD"),
    Field<&CstyRec::melee, Blob>("CSMD"),
    Field<&CstyRec::closeRange, Blob>("CSME"),
    Field<&CstyRec::flight, Blob>("CSFL"),
    Field<&CstyRec::data, Blob>("DATA"));

struct LgtmRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<std::vector<uint8_t>> data;      // DATA
    std::optional<std::vector<uint8_t>> ambient;   // DALC
};

inline const auto kLgtmSchema = MakeSchema<LgtmRec>("LGTM",
    Field<&LgtmRec::editorId, ZString>("EDID"),
    Field<&LgtmRec::data, Blob>("DATA"),
    Field<&LgtmRec::ambient, Blob>("DALC"));

struct SpgdRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> data;     // DATA
    std::optional<std::string>          particleTexture; // ICON
};

inline const auto kSpgdSchema = MakeSchema<SpgdRec>("SPGD",
    Field<&SpgdRec::editorId, ZString>("EDID"),
    Field<&SpgdRec::data, Blob>("DATA"),
    Field<&SpgdRec::particleTexture, ZString>("ICON"));

// ── FSTP / FSTS / LSCR / MESG ────────────────────────────────────────────────

struct FstpRec : RecordExtras {
    std::optional<std::string> editorId; // EDID
    std::optional<FormRef>     impactSet;// DATA (IPDS)
    std::optional<std::string> tag;      // ANAM
};

inline const auto kFstpSchema = MakeSchema<FstpRec>("FSTP",
    Field<&FstpRec::editorId, ZString>("EDID"),
    Field<&FstpRec::impactSet, FormRefCodec>("DATA"),
    Field<&FstpRec::tag, ZString>("ANAM"));

struct FstsRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> counts;   // XCNT (5 u32)
    std::optional<std::vector<FormRef>> footsteps;// DATA (FSTP array)
};

inline const auto kFstsSchema = MakeSchema<FstsRec>("FSTS",
    Field<&FstsRec::editorId, ZString>("EDID"),
    Field<&FstsRec::counts, Blob>("XCNT"),
    Field<&FstsRec::footsteps, FormRefArray>("DATA"));

struct LscrRec : RecordExtras {
    std::optional<std::string> editorId;   // EDID
    std::optional<FormRef>     staticNif;  // NNAM
    std::optional<Lstring>     description;// DESC
    std::optional<std::vector<uint8_t>> rotationOffset; // RNAM/ONAM/XNAM via passthrough
    ConditionList              conds;      // CTDA/CIS1/CIS2
};

inline const auto kLscrSchema = MakeSchema<LscrRec>("LSCR",
    Field<&LscrRec::editorId, ZString>("EDID"),
    Field<&LscrRec::staticNif, FormRefCodec>("NNAM"),
    Field<&LscrRec::description, LStringDL>("DESC"),
    CtdaRow<&LscrRec::conds>(),
    Cis1Row<&LscrRec::conds>(),
    Cis2Row<&LscrRec::conds>());

struct MesgRec : RecordExtras {
    std::optional<std::string> editorId;    // EDID
    std::optional<Lstring>     description; // DESC
    std::optional<Lstring>     fullName;    // FULL
    std::optional<FormRef>     quest;       // QNAM
    std::optional<uint32_t>    flags;       // DNAM
    std::optional<uint32_t>    displayTime; // TNAM
    std::vector<Lstring>       buttonText;  // ITXT
    ConditionList              conds;       // CTDA/CIS1/CIS2 (button conditions)
};

inline const auto kMesgSchema = MakeSchema<MesgRec>("MESG",
    Field<&MesgRec::editorId, ZString>("EDID"),
    Field<&MesgRec::description, LStringDL>("DESC"),
    Field<&MesgRec::fullName, LString>("FULL"),
    Field<&MesgRec::quest, FormRefCodec>("QNAM"),
    Field<&MesgRec::flags, U32>("DNAM"),
    Field<&MesgRec::displayTime, U32>("TNAM"),
    Repeated<&MesgRec::buttonText, LString>("ITXT"),
    CtdaRow<&MesgRec::conds>(),
    Cis1Row<&MesgRec::conds>(),
    Cis2Row<&MesgRec::conds>());

// ── Story manager nodes SMBN / SMQN / SMEN ───────────────────────────────────

struct SmNodeRec : RecordExtras {
    std::optional<std::string> editorId;    // EDID
    std::optional<FormRef>     parent;      // PNAM
    std::optional<FormRef>     child;       // SNAM
    std::optional<uint32_t>    conditionCount; // CITC
    std::optional<uint32_t>    flags;       // DNAM
    std::optional<uint32_t>    maxAcrossType; // XNAM
    std::optional<FormRef>     quest;       // NNAM? / QNAM (SMQN)
    ConditionList              conds;       // CTDA/CIS1/CIS2
};

namespace schema_detail {
template <uint32_t /*tag*/>
inline auto MakeSmSchema(const char sig[5]) {
    return MakeSchema<SmNodeRec>(sig,
        Field<&SmNodeRec::editorId, ZString>("EDID"),
        Field<&SmNodeRec::parent, FormRefCodec>("PNAM"),
        Field<&SmNodeRec::child, FormRefCodec>("SNAM"),
        Field<&SmNodeRec::conditionCount, U32>("CITC"),
        Field<&SmNodeRec::flags, U32>("DNAM"),
        Field<&SmNodeRec::maxAcrossType, U32>("XNAM"),
        Field<&SmNodeRec::quest, FormRefCodec>("QNAM"),
        CtdaRow<&SmNodeRec::conds>(),
        Cis1Row<&SmNodeRec::conds>(),
        Cis2Row<&SmNodeRec::conds>());
}
} // namespace schema_detail

inline const auto kSmbnSchema = schema_detail::MakeSmSchema<0>("SMBN");
inline const auto kSmqnSchema = schema_detail::MakeSmSchema<1>("SMQN");
inline const auto kSmenSchema = schema_detail::MakeSmSchema<2>("SMEN");

// ── RELA / REGN / PWAT / WTHR (identity + clean edges; params passthrough) ───

struct RelaRec : RecordExtras {
    std::optional<std::string>          editorId; // EDID
    std::optional<std::vector<uint8_t>> data;     // DATA (parent/child FormIDs interleaved with rank/flags)
};

// RELA DATA interleaves two FormIDs with rank/flag bytes and an association
// type FormID; the layout has a legacy variant, so it stays passthrough.
inline const auto kRelaSchema = MakeSchema<RelaRec>("RELA",
    Field<&RelaRec::editorId, ZString>("EDID"),
    Field<&RelaRec::data, Blob>("DATA"));

struct RegnRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<std::vector<uint8_t>> color;     // RCLR
    std::optional<FormRef>              worldspace;// WNAM
    std::optional<std::vector<uint8_t>> areaData;  // RPLI/RPLD via passthrough
};

inline const auto kRegnSchema = MakeSchema<RegnRec>("REGN",
    Field<&RegnRec::editorId, ZString>("EDID"),
    Field<&RegnRec::color, Blob>("RCLR"),
    Field<&RegnRec::worldspace, FormRefCodec>("WNAM"),
    Field<&RegnRec::areaData, Blob>("RPLI"));

struct PwatRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<ObjectBounds>         bounds;    // OBND
    std::optional<std::string>          model;     // MODL
    std::optional<std::vector<uint8_t>> modelData; // MODT
    std::optional<std::vector<uint8_t>> data;      // DNAM (flags + water FormID; size-variant)
};

inline const auto kPwatSchema = MakeSchema<PwatRec>("PWAT",
    Field<&PwatRec::editorId, ZString>("EDID"),
    Field<&PwatRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&PwatRec::model, ZString>("MODL"),
    Field<&PwatRec::modelData, Blob>("MODT"),
    Field<&PwatRec::data, Blob>("DNAM"));

struct WthrRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::vector<std::string>            cloudTextures; // 00TX..
    std::optional<std::vector<uint8_t>> data;       // DATA / DNAM param blocks
    std::optional<FormRef>              sunGlare;    // GNAM? kept as edge
    std::optional<FormRef>              precipArt;   // MNAM (SPGD)
    std::optional<FormRef>              auroraModel; // NAM0? via edge
    std::optional<FormRef>              sunImageSpace; // IMSP-ish edge
};

// WTHR is param-heavy with dozens of context-typed subrecords; the identity +
// a couple of clean FormID edges are bound, the rest passes through.
inline const auto kWthrSchema = MakeSchema<WthrRec>("WTHR",
    Field<&WthrRec::editorId, ZString>("EDID"),
    Field<&WthrRec::sunGlare, FormRefCodec>("GNAM"),
    Field<&WthrRec::precipArt, FormRefCodec>("MNAM"));

// ── Opaque-geometry shells: NAVM / NAVI / PGRE / PHZD / BPTD ──────────────────
// These carry large engine-only blobs (nav geometry, ragdoll part trees) that
// nothing in the editor consumes. EDID + clean edges are bound where present;
// geometry rides through as passthrough (so remap refuses — correct, since the
// blobs contain FormIDs the shell can't see).

struct NavmRec : RecordExtras {
    std::optional<std::vector<uint8_t>> geometry; // NVNM
};

inline const auto kNavmSchema = MakeSchema<NavmRec>("NAVM",
    Field<&NavmRec::geometry, Blob>("NVNM"));

struct NaviRec : RecordExtras {
    std::optional<uint32_t> version; // NVER
};

inline const auto kNaviSchema = MakeSchema<NaviRec>("NAVI",
    Field<&NaviRec::version, U32>("NVER"));

struct PlacedFxRec : RecordExtras {
    std::optional<std::string> editorId; // EDID
    std::optional<Vmad>        vmad;     // VMAD
    std::optional<FormRef>     base;     // NAME
    std::optional<float>       scale;    // XSCL
};

// PGRE (placed grenade) / PHZD (placed hazard) share the placed-object edges.
namespace schema_detail {
template <uint32_t /*tag*/>
inline auto MakePlacedFxSchema(const char sig[5]) {
    return MakeSchema<PlacedFxRec>(sig,
        Field<&PlacedFxRec::editorId, ZString>("EDID"),
        Field<&PlacedFxRec::vmad, VmadCodec>("VMAD"),
        Field<&PlacedFxRec::base, FormRefCodec>("NAME"),
        Field<&PlacedFxRec::scale, F32>("XSCL"));
}
} // namespace schema_detail

inline const auto kPgreSchema = schema_detail::MakePlacedFxSchema<0>("PGRE");
inline const auto kPhzdSchema = schema_detail::MakePlacedFxSchema<1>("PHZD");

struct BptdRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<std::string>          model;     // MODL
    std::optional<std::vector<uint8_t>> modelData; // MODT
};

inline const auto kBptdSchema = MakeSchema<BptdRec>("BPTD",
    Field<&BptdRec::editorId, ZString>("EDID"),
    Field<&BptdRec::model, ZString>("MODL"),
    Field<&BptdRec::modelData, Blob>("MODT"));

// VOLI — volumetric lighting (added by the AE update to the DLC masters). Its
// parameter block carries no references the editor needs; EDID is bound and
// the rest passes through, which keeps it in the typed roster and byte-faithful.
struct VoliRec : RecordExtras {
    std::optional<std::string> editorId; // EDID
};

inline const auto kVoliSchema = MakeSchema<VoliRec>("VOLI",
    Field<&VoliRec::editorId, ZString>("EDID"));

} // namespace esper
