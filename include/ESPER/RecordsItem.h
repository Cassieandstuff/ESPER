#pragma once
#include <ESPER/RecordsCommon.h>
#include <ESPER/Vmad.h>

namespace esper {

#pragma pack(push, 1)
// ARMA DNAM — addon data, 12 bytes.
struct ArmaDnam {
    uint8_t  malePriority   = 0;
    uint8_t  femalePriority = 0;
    uint8_t  weightSliderMale   = 0;   // bit1 = enabled
    uint8_t  weightSliderFemale = 0;
    uint16_t unknown        = 0;
    uint8_t  detectionSound = 0;
    uint8_t  pad            = 0;
    float    weaponAdjust   = 0.f;
};
static_assert(sizeof(ArmaDnam) == 12);
#pragma pack(pop)

// ── ARMA — armor addon ────────────────────────────────────────────────────────
// NOTE the Skyrim sig quirk: ARMA's MODL subrecords are 4-byte FormIDs
// (additional races), while the world-model PATHS live in MOD2..MOD5.

struct ArmaRec : RecordExtras {
    std::optional<std::string>          editorId;        // EDID
    std::optional<BipedBodyTemplate>    biped;           // BOD2
    std::optional<FormRef>              race;            // RNAM
    std::optional<ArmaDnam>             data;            // DNAM
    std::optional<std::string>          maleModel;       // MOD2
    std::optional<std::vector<uint8_t>> maleModelData;   // MO2T
    std::optional<std::vector<uint8_t>> maleModelSwap;   // MO2S
    std::optional<std::string>          femaleModel;     // MOD3
    std::optional<std::vector<uint8_t>> femaleModelData; // MO3T
    std::optional<std::vector<uint8_t>> femaleModelSwap; // MO3S
    std::optional<std::string>          male1stModel;    // MOD4
    std::optional<std::vector<uint8_t>> male1stData;     // MO4T
    std::optional<std::vector<uint8_t>> male1stSwap;     // MO4S
    std::optional<std::string>          female1stModel;  // MOD5
    std::optional<std::vector<uint8_t>> female1stData;   // MO5T
    std::optional<std::vector<uint8_t>> female1stSwap;   // MO5S
    std::optional<FormRef>              maleSkinTexture;   // NAM0
    std::optional<FormRef>              femaleSkinTexture; // NAM1
    std::optional<FormRef>              maleSkinSwapList;  // NAM2
    std::optional<FormRef>              femaleSkinSwapList;// NAM3
    std::vector<FormRef>                additionalRaces; // MODL (repeated FormRef!)
    std::optional<FormRef>              footstepSound;   // SNDD
    std::optional<FormRef>              artObject;       // ONAM
};

inline const auto kArmaSchema = MakeSchema<ArmaRec>("ARMA",
    Field<&ArmaRec::editorId, ZString>("EDID"),
    Field<&ArmaRec::biped, Packed<BipedBodyTemplate>>("BOD2"),
    Field<&ArmaRec::race, FormRefCodec>("RNAM"),
    Field<&ArmaRec::data, Packed<ArmaDnam>>("DNAM"),
    Field<&ArmaRec::maleModel, ZString>("MOD2"),
    Field<&ArmaRec::maleModelData, Blob>("MO2T"),
    Field<&ArmaRec::maleModelSwap, Blob>("MO2S"),
    Field<&ArmaRec::femaleModel, ZString>("MOD3"),
    Field<&ArmaRec::femaleModelData, Blob>("MO3T"),
    Field<&ArmaRec::femaleModelSwap, Blob>("MO3S"),
    Field<&ArmaRec::male1stModel, ZString>("MOD4"),
    Field<&ArmaRec::male1stData, Blob>("MO4T"),
    Field<&ArmaRec::male1stSwap, Blob>("MO4S"),
    Field<&ArmaRec::female1stModel, ZString>("MOD5"),
    Field<&ArmaRec::female1stData, Blob>("MO5T"),
    Field<&ArmaRec::female1stSwap, Blob>("MO5S"),
    Field<&ArmaRec::maleSkinTexture, FormRefCodec>("NAM0"),
    Field<&ArmaRec::femaleSkinTexture, FormRefCodec>("NAM1"),
    Field<&ArmaRec::maleSkinSwapList, FormRefCodec>("NAM2"),
    Field<&ArmaRec::femaleSkinSwapList, FormRefCodec>("NAM3"),
    Repeated<&ArmaRec::additionalRaces, FormRefCodec>("MODL"),
    Field<&ArmaRec::footstepSound, FormRefCodec>("SNDD"),
    Field<&ArmaRec::artObject, FormRefCodec>("ONAM"));

// ── ARMO — armor ──────────────────────────────────────────────────────────────
// Skyrim sig quirk again: ARMO's MODL subrecords are 4-byte FormIDs — the
// armature (ARMA) list. World-model paths are MOD2 (male) / MOD4 (female).
// VMAD and legacy BODT ride through as unknown-passthrough this wave.

struct ArmoRec : RecordExtras {
    std::optional<std::string>          editorId;        // EDID
    std::optional<Vmad>                 vmad;            // VMAD
    std::optional<ObjectBounds>         bounds;          // OBND
    std::optional<Lstring>              fullName;        // FULL
    std::optional<FormRef>              enchantment;     // EITM
    std::optional<uint16_t>             enchantAmount;   // EAMT
    std::optional<std::string>          maleModel;       // MOD2
    std::optional<std::vector<uint8_t>> maleModelData;   // MO2T
    std::optional<std::vector<uint8_t>> maleModelSwap;   // MO2S
    std::optional<std::string>          maleIcon;        // ICON
    std::optional<std::string>          maleMsgIcon;     // MICO
    std::optional<std::string>          femaleModel;     // MOD4
    std::optional<std::vector<uint8_t>> femaleModelData; // MO4T
    std::optional<std::vector<uint8_t>> femaleModelSwap; // MO4S
    std::optional<std::string>          femaleIcon;      // ICO2
    std::optional<std::string>          femaleMsgIcon;   // MIC2
    std::optional<BipedBodyTemplate>    biped;           // BOD2
    std::optional<FormRef>              pickupSound;     // YNAM
    std::optional<FormRef>              dropSound;       // ZNAM
    std::optional<std::string>          ragdollTemplate; // BMCT
    std::optional<FormRef>              equipType;       // ETYP
    std::optional<FormRef>              bashImpact;      // BIDS
    std::optional<FormRef>              bashMaterial;    // BAMT
    std::optional<FormRef>              race;            // RNAM
    std::optional<uint32_t>             keywordCount;    // KSIZ
    std::optional<std::vector<FormRef>> keywords;        // KWDA
    std::optional<Lstring>              description;     // DESC
    std::vector<FormRef>                armatures;       // MODL (repeated FormRef!)
    std::optional<ItemValueWeight>      valueWeight;     // DATA
    std::optional<uint32_t>             armorRating;     // DNAM (rating ×100)
    std::optional<FormRef>              templateArmor;   // TNAM
};

inline const auto kArmoSchema = MakeSchema<ArmoRec>("ARMO",
    Field<&ArmoRec::editorId, ZString>("EDID"),
    Field<&ArmoRec::vmad, VmadCodec>("VMAD"),
    Field<&ArmoRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&ArmoRec::fullName, LString>("FULL"),
    Field<&ArmoRec::enchantment, FormRefCodec>("EITM"),
    Field<&ArmoRec::enchantAmount, U16v>("EAMT"),
    Field<&ArmoRec::maleModel, ZString>("MOD2"),
    Field<&ArmoRec::maleModelData, Blob>("MO2T"),
    Field<&ArmoRec::maleModelSwap, Blob>("MO2S"),
    Field<&ArmoRec::maleIcon, ZString>("ICON"),
    Field<&ArmoRec::maleMsgIcon, ZString>("MICO"),
    Field<&ArmoRec::femaleModel, ZString>("MOD4"),
    Field<&ArmoRec::femaleModelData, Blob>("MO4T"),
    Field<&ArmoRec::femaleModelSwap, Blob>("MO4S"),
    Field<&ArmoRec::femaleIcon, ZString>("ICO2"),
    Field<&ArmoRec::femaleMsgIcon, ZString>("MIC2"),
    Field<&ArmoRec::biped, Packed<BipedBodyTemplate>>("BOD2"),
    Field<&ArmoRec::pickupSound, FormRefCodec>("YNAM"),
    Field<&ArmoRec::dropSound, FormRefCodec>("ZNAM"),
    Field<&ArmoRec::ragdollTemplate, ZString>("BMCT"),
    Field<&ArmoRec::equipType, FormRefCodec>("ETYP"),
    Field<&ArmoRec::bashImpact, FormRefCodec>("BIDS"),
    Field<&ArmoRec::bashMaterial, FormRefCodec>("BAMT"),
    Field<&ArmoRec::race, FormRefCodec>("RNAM"),
    Field<&ArmoRec::keywordCount, U32>("KSIZ"),
    Field<&ArmoRec::keywords, FormRefArray>("KWDA"),
    Field<&ArmoRec::description, LStringDL>("DESC"),
    Repeated<&ArmoRec::armatures, FormRefCodec>("MODL"),
    Field<&ArmoRec::valueWeight, Packed<ItemValueWeight>>("DATA"),
    Field<&ArmoRec::armorRating, U32>("DNAM"),
    Field<&ArmoRec::templateArmor, FormRefCodec>("TNAM"));

} // namespace esper
