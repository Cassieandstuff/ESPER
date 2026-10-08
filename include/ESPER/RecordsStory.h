#pragma once
#include <ESPER/Conditions.h>
#include <ESPER/RecordsCommon.h>
#include <ESPER/Vmad.h>

namespace esper {

// Wave 4 — actors/leveled/story/dialogue. Notes:
//  • QUST / SCEN / PACK / INFO VMADs carry FRAGMENT (and for QUST, alias)
//    sections after the scripts — VmadCodec refuses those payloads, so they
//    ride through as unknown-passthrough (byte-faithful, remap-refusing)
//    until the fragment model lands.
//  • Package data trees (PDTO/PTDA/…), quest alias sections (ALST…ALED) and
//    FACT's variant-sized CRVA/PLVD stay passthrough — several hold
//    conditionally-typed FormIDs.
//  • COED (owner data) stays passthrough everywhere: its second field is a
//    GLOB FormID or a rank int depending on the owner's type.

#pragma pack(push, 1)

// LVLO — leveled-list entry, 12 bytes.
struct LeveledEntry {
    int32_t  level     = 0;
    uint32_t reference = 0;          // FormID
    int32_t  count     = 0;
};
static_assert(sizeof(LeveledEntry) == 12);
inline constexpr FieldDef kLeveledEntryIds[] = { FormIdAt(4) };

// FACT XNAM — interfaction relation, 12 bytes.
struct FactionRelation {
    uint32_t faction        = 0;     // FormID
    int32_t  modifier       = 0;
    uint32_t combatReaction = 0;     // 0 neutral 1 enemy 2 ally 3 friend
};
static_assert(sizeof(FactionRelation) == 12);
inline constexpr FieldDef kFactionRelationIds[] = { FormIdAt(0) };

// FACT VENV — vendor values, 12 bytes.
struct VendorValues {
    uint16_t startHour = 0;
    uint16_t endHour = 0;
    uint32_t radius = 0;
    uint8_t  buysStolen = 0;
    uint8_t  notSellBuy = 0;
    uint16_t unused = 0;
};
static_assert(sizeof(VendorValues) == 12);

// PACK PKCU — counter/template, 12 bytes.
struct PackCounter {
    uint32_t dataInputCount  = 0;
    uint32_t packageTemplate = 0;    // FormID
    uint32_t versionCounter  = 0;
};
static_assert(sizeof(PackCounter) == 12);
inline constexpr FieldDef kPackCounterIds[] = { FormIdAt(4) };

// DIAL DATA — 4 bytes.
struct DialData {
    uint8_t  unknown = 0;
    uint8_t  category = 0;           // 0 topic 1 favor 2 scene 3 combat …
    uint16_t subtype = 0;
};
static_assert(sizeof(DialData) == 4);

// INFO ENAM — response flags, 4 bytes.
struct InfoFlags {
    uint16_t flags = 0;
    uint16_t resetHours = 0;         // encoded hours ×2730
};
static_assert(sizeof(InfoFlags) == 4);

// INFO TRDT — response data, 24 bytes.
struct ResponseData {
    uint32_t emotionType = 0;
    int32_t  emotionValue = 0;
    uint32_t unknown = 0;
    uint8_t  responseNumber = 0;
    uint8_t  pad[3] = {};
    uint32_t sound = 0;              // FormID
    uint8_t  useEmotionAnimation = 0;
    uint8_t  pad2[3] = {};
};
static_assert(sizeof(ResponseData) == 24);
inline constexpr FieldDef kResponseDataIds[] = { FormIdAt(16) };

// QUST INDX — stage index, 4 bytes.
struct QuestStageIndex {
    uint16_t index = 0;
    uint8_t  flags = 0;
    uint8_t  unknown = 0;
};
static_assert(sizeof(QuestStageIndex) == 4);

#pragma pack(pop)

// ── Leveled lists (LVLN / LVLI / LVSP) ───────────────────────────────────────

struct LeveledRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<uint8_t>              chanceNone;  // LVLD
    std::optional<uint8_t>              flags;       // LVLF
    std::optional<FormRef>              global;      // LVLG (LVLI)
    std::optional<uint8_t>              entryCount;  // LLCT
    std::vector<LeveledEntry>           entries;     // LVLO
    std::optional<std::string>          model;       // MODL (LVLN)
    std::optional<std::vector<uint8_t>> modelData;   // MODT
};

namespace schema_detail {
template <uint32_t /*tag*/>
inline auto MakeLeveledSchema(const char sig[5]) {
    return MakeSchema<LeveledRec>(sig,
        Field<&LeveledRec::editorId, ZString>("EDID"),
        Field<&LeveledRec::bounds, Packed<ObjectBounds>>("OBND"),
        Field<&LeveledRec::chanceNone, U8v>("LVLD"),
        Field<&LeveledRec::flags, U8v>("LVLF"),
        Field<&LeveledRec::global, FormRefCodec>("LVLG"),
        Field<&LeveledRec::entryCount, U8v>("LLCT"),
        Repeated<&LeveledRec::entries,
                 PackedIds<LeveledEntry, kLeveledEntryIds>>("LVLO"),
        Field<&LeveledRec::model, ZString>("MODL"),
        Field<&LeveledRec::modelData, Blob>("MODT"));
}
} // namespace schema_detail

inline const auto kLvlnSchema = schema_detail::MakeLeveledSchema<0>("LVLN");
inline const auto kLvliSchema = schema_detail::MakeLeveledSchema<1>("LVLI");
inline const auto kLvspSchema = schema_detail::MakeLeveledSchema<2>("LVSP");

// ── FACT ─────────────────────────────────────────────────────────────────────

struct FactRec : RecordExtras {
    std::optional<std::string>   editorId;       // EDID
    std::optional<Lstring>       fullName;       // FULL
    std::vector<FactionRelation> relations;      // XNAM
    std::optional<uint32_t>      flags;          // DATA
    std::optional<FormRef>       exteriorJail;   // JAIL
    std::optional<FormRef>       followerWait;   // WAIT
    std::optional<FormRef>       stolenContainer;// STOL
    std::optional<FormRef>       playerInventory;// PLCN
    std::optional<FormRef>       crimeGroup;     // CRGR
    std::optional<FormRef>       jailOutfit;     // JOUT
    std::vector<uint32_t>        rankIds;        // RNAM
    std::vector<Lstring>         rankMaleTitles; // MNAM
    std::vector<Lstring>         rankFemaleTitles; // FNAM
    std::optional<FormRef>       vendorList;     // VEND
    std::optional<FormRef>       vendorContainer;// VENC
    std::optional<VendorValues>  vendorValues;   // VENV
    ConditionList                conds;          // CTDA/CIS1/CIS2 (vendor conds)
};

inline const auto kFactSchema = MakeSchema<FactRec>("FACT",
    Field<&FactRec::editorId, ZString>("EDID"),
    Field<&FactRec::fullName, LString>("FULL"),
    Repeated<&FactRec::relations,
             PackedIds<FactionRelation, kFactionRelationIds>>("XNAM"),
    Field<&FactRec::flags, U32>("DATA"),
    Field<&FactRec::exteriorJail, FormRefCodec>("JAIL"),
    Field<&FactRec::followerWait, FormRefCodec>("WAIT"),
    Field<&FactRec::stolenContainer, FormRefCodec>("STOL"),
    Field<&FactRec::playerInventory, FormRefCodec>("PLCN"),
    Field<&FactRec::crimeGroup, FormRefCodec>("CRGR"),
    Field<&FactRec::jailOutfit, FormRefCodec>("JOUT"),
    Repeated<&FactRec::rankIds, U32>("RNAM"),
    Repeated<&FactRec::rankMaleTitles, LString>("MNAM"),
    Repeated<&FactRec::rankFemaleTitles, LString>("FNAM"),
    Field<&FactRec::vendorList, FormRefCodec>("VEND"),
    Field<&FactRec::vendorContainer, FormRefCodec>("VENC"),
    Field<&FactRec::vendorValues, Packed<VendorValues>>("VENV"),
    CtdaRow<&FactRec::conds>(),
    Cis1Row<&FactRec::conds>(),
    Cis2Row<&FactRec::conds>());

// ── CLAS / OTFT / EYES / ASPC ────────────────────────────────────────────────

struct ClasRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Lstring>              fullName;    // FULL
    std::optional<Lstring>              description; // DESC
    std::optional<std::string>          icon;        // ICON
    std::optional<std::vector<uint8_t>> data;        // DATA (skill weights; no FormIDs)
};

inline const auto kClasSchema = MakeSchema<ClasRec>("CLAS",
    Field<&ClasRec::editorId, ZString>("EDID"),
    Field<&ClasRec::fullName, LString>("FULL"),
    Field<&ClasRec::description, LStringDL>("DESC"),
    Field<&ClasRec::icon, ZString>("ICON"),
    Field<&ClasRec::data, Blob>("DATA"));

struct OtftRec : RecordExtras {
    std::optional<std::string>          editorId;  // EDID
    std::optional<std::vector<FormRef>> items;     // INAM (array in one sub)
};

inline const auto kOtftSchema = MakeSchema<OtftRec>("OTFT",
    Field<&OtftRec::editorId, ZString>("EDID"),
    Field<&OtftRec::items, FormRefArray>("INAM"));

struct EyesRec : RecordExtras {
    std::optional<std::string> editorId;  // EDID
    std::optional<Lstring>     fullName;  // FULL
    std::optional<std::string> icon;      // ICON
    std::optional<uint8_t>     flags;     // DATA
};

inline const auto kEyesSchema = MakeSchema<EyesRec>("EYES",
    Field<&EyesRec::editorId, ZString>("EDID"),
    Field<&EyesRec::fullName, LString>("FULL"),
    Field<&EyesRec::icon, ZString>("ICON"),
    Field<&EyesRec::flags, U8v>("DATA"));

struct AspcRec : RecordExtras {
    std::optional<std::string>  editorId;      // EDID
    std::optional<ObjectBounds> bounds;        // OBND
    std::optional<FormRef>      ambientSound;  // SNAM
    std::optional<FormRef>      regionData;    // RDAT
    std::optional<FormRef>      reverb;        // BNAM
};

inline const auto kAspcSchema = MakeSchema<AspcRec>("ASPC",
    Field<&AspcRec::editorId, ZString>("EDID"),
    Field<&AspcRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&AspcRec::ambientSound, FormRefCodec>("SNAM"),
    Field<&AspcRec::regionData, FormRefCodec>("RDAT"),
    Field<&AspcRec::reverb, FormRefCodec>("BNAM"));

// ── PACK ─────────────────────────────────────────────────────────────────────

struct PackRec : RecordExtras {
    std::optional<std::string>          editorId;   // EDID
    std::optional<Vmad>                 vmad;       // VMAD (fragments → passthrough)
    std::optional<std::vector<uint8_t>> data;       // PKDT (flags/type; no FormIDs)
    std::optional<std::vector<uint8_t>> schedule;   // PSDT (no FormIDs)
    std::optional<PackCounter>          counter;    // PKCU
    ConditionList                       conds;      // CTDA/CIS1/CIS2
};

inline const auto kPackSchema = MakeSchema<PackRec>("PACK",
    Field<&PackRec::editorId, ZString>("EDID"),
    Field<&PackRec::vmad, VmadCodec>("VMAD"),
    Field<&PackRec::data, Blob>("PKDT"),
    Field<&PackRec::schedule, Blob>("PSDT"),
    Field<&PackRec::counter, PackedIds<PackCounter, kPackCounterIds>>("PKCU"),
    CtdaRow<&PackRec::conds>(),
    Cis1Row<&PackRec::conds>(),
    Cis2Row<&PackRec::conds>());

// ── QUST ─────────────────────────────────────────────────────────────────────
// Stage/objective/alias sections stay passthrough (alias fill types carry
// conditionally-typed FormIDs); the bound rows cover identity + text.

struct QustRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Vmad>                 vmad;        // VMAD (fragments+aliases → passthrough)
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::vector<uint8_t>> data;        // DNAM (flags/priority/delay)
    std::optional<std::vector<uint8_t>> eventName;   // ENAM (4-char story event)
    std::optional<FormRef>              displayGlobals; // QTGL
    std::optional<std::string>          objectWindowFilter; // FLTR
    std::vector<QuestStageIndex>        stageIndices;// INDX
    std::vector<uint8_t>                stageFlags;  // QSDT
    std::vector<Lstring>                logEntries;  // CNAM (per stage, DL)
    std::vector<Lstring>                objectiveTexts; // NNAM
    ConditionList                       conds;       // CTDA/CIS1/CIS2
};

inline const auto kQustSchema = MakeSchema<QustRec>("QUST",
    Field<&QustRec::editorId, ZString>("EDID"),
    Field<&QustRec::vmad, VmadCodec>("VMAD"),
    Field<&QustRec::fullName, LString>("FULL"),
    Field<&QustRec::data, Blob>("DNAM"),
    Field<&QustRec::eventName, Blob>("ENAM"),
    Field<&QustRec::displayGlobals, FormRefCodec>("QTGL"),
    Field<&QustRec::objectWindowFilter, ZString>("FLTR"),
    Repeated<&QustRec::stageIndices, Packed<QuestStageIndex>>("INDX"),
    Repeated<&QustRec::stageFlags, U8v>("QSDT"),
    Repeated<&QustRec::logEntries, LStringDL>("CNAM"),
    Repeated<&QustRec::objectiveTexts, LString>("NNAM"),
    CtdaRow<&QustRec::conds>(),
    Cis1Row<&QustRec::conds>(),
    Cis2Row<&QustRec::conds>());

// ── SCEN ─────────────────────────────────────────────────────────────────────

struct ScenRec : RecordExtras {
    std::optional<std::string> editorId;  // EDID
    std::optional<Vmad>        vmad;      // VMAD (fragments → passthrough)
    std::optional<uint32_t>    flags;     // FNAM
    std::optional<FormRef>     quest;     // PNAM
    std::optional<uint32_t>    lastActionIndex; // INAM
    ConditionList              conds;     // CTDA/CIS1/CIS2
};

inline const auto kScenSchema = MakeSchema<ScenRec>("SCEN",
    Field<&ScenRec::editorId, ZString>("EDID"),
    Field<&ScenRec::vmad, VmadCodec>("VMAD"),
    Field<&ScenRec::flags, U32>("FNAM"),
    Field<&ScenRec::quest, FormRefCodec>("PNAM"),
    Field<&ScenRec::lastActionIndex, U32>("INAM"),
    CtdaRow<&ScenRec::conds>(),
    Cis1Row<&ScenRec::conds>(),
    Cis2Row<&ScenRec::conds>());

// ── DLBR / DLVW / DIAL / INFO ────────────────────────────────────────────────

struct DlbrRec : RecordExtras {
    std::optional<std::string> editorId;      // EDID
    std::optional<FormRef>     quest;         // QNAM
    std::optional<uint32_t>    category;      // TNAM
    std::optional<uint32_t>    flags;         // DNAM
    std::optional<FormRef>     startingTopic; // SNAM
};

inline const auto kDlbrSchema = MakeSchema<DlbrRec>("DLBR",
    Field<&DlbrRec::editorId, ZString>("EDID"),
    Field<&DlbrRec::quest, FormRefCodec>("QNAM"),
    Field<&DlbrRec::category, U32>("TNAM"),
    Field<&DlbrRec::flags, U32>("DNAM"),
    Field<&DlbrRec::startingTopic, FormRefCodec>("SNAM"));

struct DlvwRec : RecordExtras {
    std::optional<std::string> editorId;  // EDID
    std::optional<FormRef>     quest;     // QNAM
    std::vector<FormRef>       branches;  // BNAM
    std::vector<FormRef>       topics;    // TNAM
};

inline const auto kDlvwSchema = MakeSchema<DlvwRec>("DLVW",
    Field<&DlvwRec::editorId, ZString>("EDID"),
    Field<&DlvwRec::quest, FormRefCodec>("QNAM"),
    Repeated<&DlvwRec::branches, FormRefCodec>("BNAM"),
    Repeated<&DlvwRec::topics, FormRefCodec>("TNAM"));

struct DialRec : RecordExtras {
    std::optional<std::string> editorId;   // EDID
    std::optional<Lstring>     fullName;   // FULL
    std::optional<float>       priority;   // PNAM
    std::optional<FormRef>     branch;     // BNAM
    std::optional<FormRef>     quest;      // QNAM
    std::optional<DialData>    data;       // DATA
    std::optional<uint32_t>    subtypeName;// SNAM (FourCC)
    std::optional<uint32_t>    infoCount;  // TIFC
};

inline const auto kDialSchema = MakeSchema<DialRec>("DIAL",
    Field<&DialRec::editorId, ZString>("EDID"),
    Field<&DialRec::fullName, LString>("FULL"),
    Field<&DialRec::priority, F32>("PNAM"),
    Field<&DialRec::branch, FormRefCodec>("BNAM"),
    Field<&DialRec::quest, FormRefCodec>("QNAM"),
    Field<&DialRec::data, Packed<DialData>>("DATA"),
    Field<&DialRec::subtypeName, U32>("SNAM"),
    Field<&DialRec::infoCount, U32>("TIFC"));

struct InfoRec : RecordExtras {
    std::optional<std::string>  editorId;      // EDID
    std::optional<Vmad>         vmad;          // VMAD (fragments → passthrough)
    std::optional<InfoFlags>    flags;         // ENAM
    std::optional<FormRef>      previousInfo;  // PNAM
    std::optional<uint8_t>      favorLevel;    // CNAM
    std::vector<FormRef>        linkTo;        // TCLT
    std::optional<FormRef>      responseData;  // DNAM (shared info)
    std::vector<ResponseData>   responses;     // TRDT
    std::vector<Lstring>        responseTexts; // NAM1 (IL — dialogue lines)
    std::vector<std::string>    scriptNotes;   // NAM2
    std::vector<std::string>    edits;         // NAM3
    std::vector<FormRef>        speakerIdles;  // SNAM
    std::optional<FormRef>      listenIdle;    // LNAM
    std::optional<FormRef>      speaker;       // ANAM
    std::optional<FormRef>      walkAwayTopic; // TWAT
    std::optional<FormRef>      audioOverride; // ONAM
    std::optional<Lstring>      prompt;        // RNAM
    ConditionList               conds;         // CTDA/CIS1/CIS2
};

inline const auto kInfoSchema = MakeSchema<InfoRec>("INFO",
    Field<&InfoRec::editorId, ZString>("EDID"),
    Field<&InfoRec::vmad, VmadCodec>("VMAD"),
    Field<&InfoRec::flags, Packed<InfoFlags>>("ENAM"),
    Field<&InfoRec::previousInfo, FormRefCodec>("PNAM"),
    Field<&InfoRec::favorLevel, U8v>("CNAM"),
    Repeated<&InfoRec::linkTo, FormRefCodec>("TCLT"),
    Field<&InfoRec::responseData, FormRefCodec>("DNAM"),
    Repeated<&InfoRec::responses,
             PackedIds<ResponseData, kResponseDataIds>>("TRDT"),
    Repeated<&InfoRec::responseTexts, LStringIL>("NAM1"),
    Repeated<&InfoRec::scriptNotes, ZString>("NAM2"),
    Repeated<&InfoRec::edits, ZString>("NAM3"),
    Repeated<&InfoRec::speakerIdles, FormRefCodec>("SNAM"),
    Field<&InfoRec::listenIdle, FormRefCodec>("LNAM"),
    Field<&InfoRec::speaker, FormRefCodec>("ANAM"),
    Field<&InfoRec::walkAwayTopic, FormRefCodec>("TWAT"),
    Field<&InfoRec::audioOverride, FormRefCodec>("ONAM"),
    Field<&InfoRec::prompt, LString>("RNAM"),
    CtdaRow<&InfoRec::conds>(),
    Cis1Row<&InfoRec::conds>(),
    Cis2Row<&InfoRec::conds>());

} // namespace esper
