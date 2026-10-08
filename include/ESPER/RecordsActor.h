#pragma once
#include <ESPER/RecordsCommon.h>
#include <ESPER/Vmad.h>

namespace esper {

#pragma pack(push, 1)

// NPC_ PRKR — perk entry, 8 bytes.
struct PerkEntry {
    uint32_t perk = 0;               // FormID
    uint8_t  rank = 0;
    uint8_t  pad[3] = { 0, 0, 0 };
};
static_assert(sizeof(PerkEntry) == 8);
inline constexpr FieldDef kPerkEntryIds[] = { FormIdAt(0) };

// NPC_ DNAM — player-style skills/stats block, 52 bytes.
struct NpcSkills {
    uint8_t  skillValues[18] = {};
    uint8_t  skillOffsets[18] = {};
    uint16_t health = 0;
    uint16_t magicka = 0;
    uint16_t stamina = 0;
    uint16_t pad = 0;
    float    lightingLevel = 0.f;
    uint16_t attackAngle = 0;   // far-away model distance? kept raw
    uint16_t pad2 = 0;
};
static_assert(sizeof(NpcSkills) == 52);

// NPC_ NAM9 — face morph sliders, 19 floats.
struct FaceMorphs {
    float values[19] = {};
};
static_assert(sizeof(FaceMorphs) == 76);

// NPC_ NAMA — face part selections, 16 bytes.
struct FaceParts {
    int32_t nose = 0;
    int32_t unknown = 0;
    int32_t eyes = 0;
    int32_t mouth = 0;
};
static_assert(sizeof(FaceParts) == 16);

// NPC_ QNAM — skin tone, 12 bytes (RGB floats).
struct SkinTone {
    float r = 0.f, g = 0.f, b = 0.f;
};
static_assert(sizeof(SkinTone) == 12);

// NPC_ TINC — tint color, 4 bytes.
struct TintColor {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};
static_assert(sizeof(TintColor) == 4);

// NPC_ CSDT/CSDI/CSDC group and DEST/DSTD/ATKD ride through as passthrough
// this wave.

#pragma pack(pop)

// ── NPC_ — actor base record ─────────────────────────────────────────────────

struct NpcRec : RecordExtras {
    std::optional<std::string>          editorId;       // EDID
    std::optional<Vmad>                 vmad;           // VMAD
    std::optional<ObjectBounds>         bounds;         // OBND
    std::optional<AcbsData>             acbs;           // ACBS
    std::vector<FactionRank>            factions;       // SNAM
    std::optional<FormRef>              deathItem;      // INAM
    std::optional<FormRef>              voice;          // VTCK
    std::optional<FormRef>              templ;          // TPLT
    std::optional<FormRef>              race;           // RNAM
    std::optional<uint32_t>             spellCount;     // SPCT
    std::vector<FormRef>                spells;         // SPLO
    std::optional<FormRef>              wornArmor;      // WNAM
    std::optional<FormRef>              farAwayModel;   // ANAM
    std::optional<FormRef>              attackRace;     // ATKR
    std::vector<std::string>            attackEvents;   // ATKE
    std::optional<FormRef>              spectatorGroup; // SPOR
    std::optional<FormRef>              observeCorpse;  // OCOR
    std::optional<FormRef>              guardWarn;      // GWOR
    std::optional<FormRef>              combatOverride; // ECOR
    std::optional<uint16_t>             nam5;           // NAM5
    std::optional<float>                height;         // NAM6
    std::optional<float>                weight;         // NAM7
    std::optional<uint32_t>             soundLevel;     // NAM8
    std::optional<FormRef>              audioTemplate;  // CSCR
    std::optional<FormRef>              defaultOutfit;  // DOFT
    std::optional<FormRef>              sleepOutfit;    // SOFT
    std::optional<FormRef>              defaultPackList;// DPLT
    std::optional<FormRef>              crimeFaction;   // CRIF
    std::optional<FormRef>              headTexture;    // FTST
    std::vector<FormRef>                headParts;      // PNAM
    std::optional<FormRef>              hairColor;      // HCLF
    std::optional<FormRef>              combatStyle;    // ZNAM
    std::optional<FormRef>              giftFilter;     // GNAM
    std::optional<FaceMorphs>           faceMorphs;     // NAM9
    std::optional<FaceParts>            faceParts;      // NAMA
    std::vector<uint16_t>               tintIndices;    // TINI
    std::vector<TintColor>              tintColors;     // TINC
    std::vector<int32_t>                tintValues;     // TINV (via I32 codec below)
    std::vector<int16_t>                tintPresets;    // TIAS
    std::optional<uint32_t>             keywordCount;   // KSIZ
    std::optional<std::vector<FormRef>> keywords;       // KWDA
    std::optional<uint32_t>             perkCount;      // PRKZ
    std::vector<PerkEntry>              perks;          // PRKR
    std::optional<uint32_t>             itemCount;      // COCT
    std::vector<ContainerEntry>         items;          // CNTO
    std::optional<AiData>               aiData;         // AIDT
    std::vector<FormRef>                packages;       // PKID
    std::optional<FormRef>              npcClass;       // CNAM
    std::optional<Lstring>              fullName;       // FULL
    std::optional<Lstring>              shortName;      // SHRT
    std::optional<std::monostate>       dataMarker;     // DATA (zero-size marker)
    std::optional<NpcSkills>            skills;         // DNAM
    std::optional<SkinTone>             skinTone;       // QNAM

    bool IsFemale() const { return acbs && (acbs->flags & 0x1u); }
};

struct I32 {
    static bool Decode(int32_t& out, SubView s, const SchemaCtx&) {
        if (s.size != 4) return false;
        std::memcpy(&out, s.data, 4);
        return true;
    }
    static void Encode(const int32_t& v, const SchemaCtx&, ByteWriter& w) {
        w.U32(static_cast<uint32_t>(v));
    }
};

struct I16 {
    static bool Decode(int16_t& out, SubView s, const SchemaCtx&) {
        if (s.size != 2) return false;
        std::memcpy(&out, s.data, 2);
        return true;
    }
    static void Encode(const int16_t& v, const SchemaCtx&, ByteWriter& w) {
        w.U16(static_cast<uint16_t>(v));
    }
};

inline const auto kNpcSchema = MakeSchema<NpcRec>("NPC_",
    Field<&NpcRec::editorId, ZString>("EDID"),
    Field<&NpcRec::vmad, VmadCodec>("VMAD"),
    Field<&NpcRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&NpcRec::acbs, Packed<AcbsData>>("ACBS"),
    Repeated<&NpcRec::factions, PackedIds<FactionRank, kFactionRankIds>>("SNAM"),
    Field<&NpcRec::deathItem, FormRefCodec>("INAM"),
    Field<&NpcRec::voice, FormRefCodec>("VTCK"),
    Field<&NpcRec::templ, FormRefCodec>("TPLT"),
    Field<&NpcRec::race, FormRefCodec>("RNAM"),
    Field<&NpcRec::spellCount, U32>("SPCT"),
    Repeated<&NpcRec::spells, FormRefCodec>("SPLO"),
    Field<&NpcRec::wornArmor, FormRefCodec>("WNAM"),
    Field<&NpcRec::farAwayModel, FormRefCodec>("ANAM"),
    Field<&NpcRec::attackRace, FormRefCodec>("ATKR"),
    Repeated<&NpcRec::attackEvents, ZString>("ATKE"),
    Field<&NpcRec::spectatorGroup, FormRefCodec>("SPOR"),
    Field<&NpcRec::observeCorpse, FormRefCodec>("OCOR"),
    Field<&NpcRec::guardWarn, FormRefCodec>("GWOR"),
    Field<&NpcRec::combatOverride, FormRefCodec>("ECOR"),
    Field<&NpcRec::nam5, U16v>("NAM5"),
    Field<&NpcRec::height, F32>("NAM6"),
    Field<&NpcRec::weight, F32>("NAM7"),
    Field<&NpcRec::soundLevel, U32>("NAM8"),
    Field<&NpcRec::audioTemplate, FormRefCodec>("CSCR"),
    Field<&NpcRec::defaultOutfit, FormRefCodec>("DOFT"),
    Field<&NpcRec::sleepOutfit, FormRefCodec>("SOFT"),
    Field<&NpcRec::defaultPackList, FormRefCodec>("DPLT"),
    Field<&NpcRec::crimeFaction, FormRefCodec>("CRIF"),
    Field<&NpcRec::headTexture, FormRefCodec>("FTST"),
    Repeated<&NpcRec::headParts, FormRefCodec>("PNAM"),
    Field<&NpcRec::hairColor, FormRefCodec>("HCLF"),
    Field<&NpcRec::combatStyle, FormRefCodec>("ZNAM"),
    Field<&NpcRec::giftFilter, FormRefCodec>("GNAM"),
    Field<&NpcRec::faceMorphs, Packed<FaceMorphs>>("NAM9"),
    Field<&NpcRec::faceParts, Packed<FaceParts>>("NAMA"),
    Repeated<&NpcRec::tintIndices, U16v>("TINI"),
    Repeated<&NpcRec::tintColors, Packed<TintColor>>("TINC"),
    Repeated<&NpcRec::tintValues, I32>("TINV"),
    Repeated<&NpcRec::tintPresets, I16>("TIAS"),
    Field<&NpcRec::keywordCount, U32>("KSIZ"),
    Field<&NpcRec::keywords, FormRefArray>("KWDA"),
    Field<&NpcRec::perkCount, U32>("PRKZ"),
    Repeated<&NpcRec::perks, PackedIds<PerkEntry, kPerkEntryIds>>("PRKR"),
    Field<&NpcRec::itemCount, U32>("COCT"),
    Repeated<&NpcRec::items, PackedIds<ContainerEntry, kContainerEntryIds>>("CNTO"),
    Field<&NpcRec::aiData, Packed<AiData>>("AIDT"),
    Repeated<&NpcRec::packages, FormRefCodec>("PKID"),
    Field<&NpcRec::npcClass, FormRefCodec>("CNAM"),
    Field<&NpcRec::fullName, LString>("FULL"),
    Field<&NpcRec::shortName, LString>("SHRT"),
    Field<&NpcRec::dataMarker, Marker>("DATA"),
    Field<&NpcRec::skills, Packed<NpcSkills>>("DNAM"),
    Field<&NpcRec::skinTone, Packed<SkinTone>>("QNAM"));

// ── RACE ─────────────────────────────────────────────────────────────────────
// RACE reuses signatures across marker-delimited sections (gendered ANAM
// skeleton paths, MODL model paths under both the body-data and behavior-graph
// sections, HEAD/INDX runs under NAM0 blocks). Rows collect occurrences in
// encounter order; `layout` preserves the sectioning, so semantic helpers
// interpret positions while byte fidelity is structural. The tint-layer complex
// (TINI/TINT/TINP/TIND/TINC/TINV/TIRS) is now typed — see RaceTintLayers() below;
// the big DATA block and the attack complex still ride through as passthrough.

struct RaceRec : RecordExtras {
    std::optional<std::string>          editorId;       // EDID
    std::optional<Lstring>              fullName;       // FULL
    std::optional<Lstring>              description;    // DESC
    std::optional<uint32_t>             spellCount;     // SPCT
    std::vector<FormRef>                spells;         // SPLO
    std::optional<FormRef>              skin;           // WNAM (skin ARMO)
    std::optional<BipedBodyTemplate>    biped;          // BOD2
    std::optional<uint32_t>             keywordCount;   // KSIZ
    std::optional<std::vector<FormRef>> keywords;       // KWDA
    std::vector<std::string>            skeletonModels; // ANAM (male, then female)
    std::vector<std::vector<uint8_t>>   modelData;      // MODT (opaque, follows each model)
    std::vector<std::vector<uint8_t>>   movementNames;  // MTNM (4-char tags)
    std::optional<std::vector<FormRef>> voices;         // VTCK (male+female in one sub)
    std::optional<std::vector<FormRef>> decapArmors;    // DNAM (male+female)
    std::optional<std::vector<FormRef>> defaultHairColors; // HCLF (male+female)
    std::optional<uint16_t>             tintCount;      // TINL
    // RACE tint-layer array — a contiguous run of subrecords, modeled as flat
    // parallel vectors (the schema binds one subrecord per row; layer grouping is
    // reconstructed positionally by RaceTintLayers() below). Per layer: TINI opens
    // it, TINT/TINP/TIND describe it (TINP and TIND are BOTH optional per layer,
    // so the vectors are NOT index-aligned to tintLayerIndex), then a run of
    // TINC/TINV/TIRS preset triples. TINC/TIND are FormIDs (CLFM colour refs) —
    // unlike NPC_ TINC, which is a packed 4-byte RGBA.
    std::vector<uint16_t>               tintLayerIndex;    // TINI (one per layer)
    std::vector<std::string>            tintLayerTexture;  // TINT (one per layer)
    std::vector<uint16_t>               tintLayerMaskType; // TINP (optional per layer)
    std::vector<FormRef>                tintLayerDefault;  // TIND (optional per layer; CLFM)
    std::vector<FormRef>                tintPresetColor;   // TINC (CLFM, per preset)
    std::vector<float>                  tintPresetValue;   // TINV (per preset)
    std::vector<uint16_t>               tintPresetId;      // TIRS (per preset)
    std::optional<float>                facegenMainClamp; // PNAM
    std::optional<float>                facegenFaceClamp; // UNAM
    std::optional<FormRef>              attackRace;     // ATKR
    std::vector<std::string>            attackEvents;   // ATKE
    std::vector<uint32_t>               partIndices;    // INDX
    std::vector<std::string>            modelPaths;     // MODL (egt / behavior graph)
    std::optional<FormRef>              bodyPartData;   // GNAM
    std::optional<FormRef>              materialType;   // NAM4
    std::optional<FormRef>              impactDataSet;  // NAM5
    std::optional<FormRef>              decapFx;        // NAM7
    std::optional<FormRef>              openLootSound;  // ONAM
    std::optional<FormRef>              closeLootSound; // LNAM
    std::vector<std::string>            bipedObjectNames; // NAME ×32
    std::vector<FormRef>                movementTypes;  // MTYP
    std::optional<uint32_t>             equipmentFlags; // VNAM
    std::vector<FormRef>                equipSlots;     // QNAM
    std::optional<FormRef>              unarmedEquipSlot; // UNES
    std::vector<std::string>            phonemeNames;   // PHTN
    std::vector<std::vector<uint8_t>>   phonemeWeights; // PHWT
    std::optional<FormRef>              baseMoveDefault;   // WKMV
    std::optional<FormRef>              baseMoveRun;       // RNMV
    std::optional<FormRef>              baseMoveSwim;      // SNMV
    std::optional<FormRef>              baseMoveFly;       // FLMV
    std::optional<FormRef>              baseMoveSneak;     // SNKMV? (kept: SPMV sprint)
    std::vector<FormRef>                headParts;      // HEAD
    std::optional<FormRef>              morphRace;      // NAM8? — via passthrough if odd
    std::optional<FormRef>              armorRace;      // RNAM
};

struct BlobCodec { // vector<vector<uint8_t>> element helper
    static bool Decode(std::vector<uint8_t>& out, SubView s, const SchemaCtx&) {
        out.assign(s.data, s.data + s.size);
        return true;
    }
    static void Encode(const std::vector<uint8_t>& v, const SchemaCtx&, ByteWriter& w) {
        if (!v.empty()) w.Bytes(v.data(), v.size());
    }
};

inline const auto kRaceSchema = MakeSchema<RaceRec>("RACE",
    Field<&RaceRec::editorId, ZString>("EDID"),
    Field<&RaceRec::fullName, LString>("FULL"),
    Field<&RaceRec::description, LStringDL>("DESC"),
    Field<&RaceRec::spellCount, U32>("SPCT"),
    Repeated<&RaceRec::spells, FormRefCodec>("SPLO"),
    Field<&RaceRec::skin, FormRefCodec>("WNAM"),
    Field<&RaceRec::biped, Packed<BipedBodyTemplate>>("BOD2"),
    Field<&RaceRec::keywordCount, U32>("KSIZ"),
    Field<&RaceRec::keywords, FormRefArray>("KWDA"),
    Repeated<&RaceRec::skeletonModels, ZString>("ANAM"),
    Repeated<&RaceRec::modelData, BlobCodec>("MODT"),
    Repeated<&RaceRec::movementNames, BlobCodec>("MTNM"),
    Field<&RaceRec::voices, FormRefArray>("VTCK"),
    Field<&RaceRec::decapArmors, FormRefArray>("DNAM"),
    Field<&RaceRec::defaultHairColors, FormRefArray>("HCLF"),
    Field<&RaceRec::tintCount, U16v>("TINL"),
    Repeated<&RaceRec::tintLayerIndex, U16v>("TINI"),
    Repeated<&RaceRec::tintLayerTexture, ZString>("TINT"),
    Repeated<&RaceRec::tintLayerMaskType, U16v>("TINP"),
    Repeated<&RaceRec::tintLayerDefault, FormRefCodec>("TIND"),
    Repeated<&RaceRec::tintPresetColor, FormRefCodec>("TINC"),
    Repeated<&RaceRec::tintPresetValue, F32>("TINV"),
    Repeated<&RaceRec::tintPresetId, U16v>("TIRS"),
    Field<&RaceRec::facegenMainClamp, F32>("PNAM"),
    Field<&RaceRec::facegenFaceClamp, F32>("UNAM"),
    Field<&RaceRec::attackRace, FormRefCodec>("ATKR"),
    Repeated<&RaceRec::attackEvents, ZString>("ATKE"),
    Repeated<&RaceRec::partIndices, U32>("INDX"),
    Repeated<&RaceRec::modelPaths, ZString>("MODL"),
    Field<&RaceRec::bodyPartData, FormRefCodec>("GNAM"),
    Field<&RaceRec::materialType, FormRefCodec>("NAM4"),
    Field<&RaceRec::impactDataSet, FormRefCodec>("NAM5"),
    Field<&RaceRec::decapFx, FormRefCodec>("NAM7"),
    Field<&RaceRec::openLootSound, FormRefCodec>("ONAM"),
    Field<&RaceRec::closeLootSound, FormRefCodec>("LNAM"),
    Repeated<&RaceRec::bipedObjectNames, ZString>("NAME"),
    Repeated<&RaceRec::movementTypes, FormRefCodec>("MTYP"),
    Field<&RaceRec::equipmentFlags, U32>("VNAM"),
    Repeated<&RaceRec::equipSlots, FormRefCodec>("QNAM"),
    Field<&RaceRec::unarmedEquipSlot, FormRefCodec>("UNES"),
    Repeated<&RaceRec::phonemeNames, ZString>("PHTN"),
    Repeated<&RaceRec::phonemeWeights, BlobCodec>("PHWT"),
    Field<&RaceRec::baseMoveDefault, FormRefCodec>("WKMV"),
    Field<&RaceRec::baseMoveRun, FormRefCodec>("RNMV"),
    Field<&RaceRec::baseMoveSwim, FormRefCodec>("SNMV"),
    Field<&RaceRec::baseMoveFly, FormRefCodec>("FLMV"),
    Field<&RaceRec::baseMoveSneak, FormRefCodec>("SPMV"),
    Repeated<&RaceRec::headParts, FormRefCodec>("HEAD"),
    Field<&RaceRec::armorRace, FormRefCodec>("RNAM"));

// ── RACE tint-layer reconstruction ───────────────────────────────────────────
// One tint layer, regrouped from the flat parallel vectors on RaceRec. This is
// what an NPC's TINI index selects: `texture` is the mask .dds the game
// composites, `maskType` (TINP) is the blend category, and `presets` are the
// CLFM colour choices offered for it (an NPC picks one and supplies its own
// TINC/TINV). Grouping is positional — TINP and TIND are optional per layer, so
// the vectors are NOT index-aligned; we walk RaceRec::layout (decode order) and
// draw each value from its vector by a running counter.
struct RaceTintLayer {
    uint16_t                index = 0;         // TINI — the index NPC_ TINI matches
    std::string             texture;           // TINT — mask .dds (data-relative)
    std::optional<uint16_t> maskType;          // TINP — blend/mask category
    std::optional<FormRef>  defaultColor;      // TIND — default CLFM colour
    struct Preset { FormRef color; float value = 0.f; uint16_t id = 0; };
    std::vector<Preset>     presets;           // TINC + TINV + TIRS triples
};

inline std::vector<RaceTintLayer> RaceTintLayers(const RaceRec& r) {
    std::vector<RaceTintLayer> out;
    size_t ci = 0, vi = 0, ii = 0;   // preset color / value / id cursors
    size_t xi = 0, ti = 0, pi = 0, di = 0;  // index / texture / maskType / default cursors
    static const uint32_t sTINI = Sig4("TINI"), sTINT = Sig4("TINT"),
                          sTINP = Sig4("TINP"), sTIND = Sig4("TIND"),
                          sTINC = Sig4("TINC"), sTINV = Sig4("TINV"), sTIRS = Sig4("TIRS");
    for (const RecordExtras::LayoutEntry& e : r.layout) {
        if (e.binder < 0) continue;
        const uint32_t sig = kRaceSchema.rows[static_cast<size_t>(e.binder)].sig;
        if (sig == sTINI) {
            RaceTintLayer l;
            if (xi < r.tintLayerIndex.size()) l.index = r.tintLayerIndex[xi];
            ++xi;
            out.push_back(std::move(l));
        } else if (sig == sTINT) {
            if (!out.empty() && ti < r.tintLayerTexture.size()) out.back().texture = r.tintLayerTexture[ti];
            ++ti;
        } else if (sig == sTINP) {
            if (!out.empty() && pi < r.tintLayerMaskType.size()) out.back().maskType = r.tintLayerMaskType[pi];
            ++pi;
        } else if (sig == sTIND) {
            if (!out.empty() && di < r.tintLayerDefault.size()) out.back().defaultColor = r.tintLayerDefault[di];
            ++di;
        } else if (sig == sTINC) {
            if (!out.empty() && ci < r.tintPresetColor.size())
                out.back().presets.push_back(RaceTintLayer::Preset{ r.tintPresetColor[ci], 0.f, 0 });
            ++ci;
        } else if (sig == sTINV) {
            if (!out.empty() && !out.back().presets.empty() && vi < r.tintPresetValue.size())
                out.back().presets.back().value = r.tintPresetValue[vi];
            ++vi;
        } else if (sig == sTIRS) {
            if (!out.empty() && !out.back().presets.empty() && ii < r.tintPresetId.size())
                out.back().presets.back().id = r.tintPresetId[ii];
            ++ii;
        }
    }
    return out;
}

} // namespace esper
