#pragma once
#include <ESPER/Conditions.h>
#include <ESPER/RecordsCommon.h>
#include <ESPER/Vmad.h>

namespace esper {

// Wave 3 — items and magic. Conditions (CTDA/CIS1/CIS2) are fully typed via
// Conditions.h but bound remap-opaque: a record carrying them refuses FormID
// remapping until the per-function parameter-kind table lands (see the
// rationale in Conditions.h). MGEF's 152-byte DATA and PERK's section
// machinery stay passthrough for the same conditional-FormID reason.

#pragma pack(push, 1)

// EFIT — magic effect item, 12 bytes.
struct EffectItem {
    float    magnitude = 0.f;
    uint32_t area      = 0;
    uint32_t duration  = 0;
};
static_assert(sizeof(EffectItem) == 12);

// SPEL/SCRL SPIT — spell data, 36 bytes.
struct SpellData {
    uint32_t cost = 0;
    uint32_t flags = 0;
    uint32_t type = 0;
    float    chargeTime = 0.f;
    uint32_t castType = 0;
    uint32_t delivery = 0;
    float    castDuration = 0.f;
    float    range = 0.f;
    uint32_t halfCostPerk = 0;       // FormID
};
static_assert(sizeof(SpellData) == 36);
inline constexpr FieldDef kSpellDataIds[] = { FormIdAt(32) };

// ENCH ENIT — enchantment data, 36 bytes (SSE form).
struct EnchantData {
    int32_t  cost = 0;
    uint32_t flags = 0;
    uint32_t castType = 0;
    int32_t  amount = 0;
    uint32_t delivery = 0;
    uint32_t type = 0;
    float    chargeTime = 0.f;
    uint32_t baseEnchantment = 0;    // FormID
    uint32_t wornRestrictions = 0;   // FormID
};
static_assert(sizeof(EnchantData) == 36);
inline constexpr FieldDef kEnchantDataIds[] = { FormIdAt(28), FormIdAt(32) };

// ALCH ENIT — potion data, 20 bytes.
struct PotionData {
    int32_t  value = 0;
    uint32_t flags = 0;
    uint32_t addiction = 0;          // FormID
    uint32_t addictionChance = 0;
    uint32_t useSound = 0;           // FormID
};
static_assert(sizeof(PotionData) == 20);
inline constexpr FieldDef kPotionDataIds[] = { FormIdAt(8), FormIdAt(16) };

// INGR ENIT — ingredient data, 8 bytes.
struct IngredientData {
    int32_t  value = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(IngredientData) == 8);

// AMMO DATA — 16 bytes.
struct AmmoData {
    uint32_t projectile = 0;         // FormID
    uint32_t flags = 0;
    float    damage = 0.f;
    uint32_t value = 0;
};
static_assert(sizeof(AmmoData) == 16);
inline constexpr FieldDef kAmmoDataIds[] = { FormIdAt(0) };

// WEAP DATA — 10 bytes.
struct WeaponBasic {
    int32_t value = 0;
    float   weight = 0.f;
    int16_t damage = 0;
};
static_assert(sizeof(WeaponBasic) == 10);

// WEAP CRDT — critical data, 24 bytes (SSE form).
struct WeaponCrit {
    uint16_t critDamage = 0;
    uint16_t pad = 0;
    float    critMult = 0.f;
    uint32_t flags = 0;
    uint32_t pad2 = 0;
    uint32_t critEffect = 0;         // SPEL FormID
    uint32_t pad3 = 0;
};
static_assert(sizeof(WeaponCrit) == 24);
inline constexpr FieldDef kWeaponCritIds[] = { FormIdAt(16) };

// SHOU SNAM — word-of-power entry, 12 bytes.
struct ShoutWord {
    uint32_t word = 0;               // WOOP FormID
    uint32_t spell = 0;              // SPEL FormID
    float    recoveryTime = 0.f;
};
static_assert(sizeof(ShoutWord) == 12);
inline constexpr FieldDef kShoutWordIds[] = { FormIdAt(0), FormIdAt(4) };

// SLGM soul levels ride as single bytes (SOUL/SLCP).

#pragma pack(pop)

// ── Simple carryable items (MISC / KEYM / SLGM share the shape) ──────────────

struct ItemRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Vmad>                 vmad;        // VMAD
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::vector<uint8_t>> modelSwap;   // MODS
    std::optional<std::string>          icon;        // ICON
    std::optional<std::string>          msgIcon;     // MICO
    std::optional<FormRef>              pickupSound; // YNAM
    std::optional<FormRef>              dropSound;   // ZNAM
    std::optional<uint32_t>             keywordCount;// KSIZ
    std::optional<std::vector<FormRef>> keywords;    // KWDA
    std::optional<ItemValueWeight>      valueWeight; // DATA
    // SLGM extras
    std::optional<uint8_t>              soulLevel;   // SOUL
    std::optional<uint8_t>              soulCapacity;// SLCP
    std::optional<FormRef>              linkedTo;    // NAM0
};

namespace schema_detail {
template <uint32_t /*tag*/>
inline auto MakeItemSchema(const char sig[5]) {
    return MakeSchema<ItemRec>(sig,
        Field<&ItemRec::editorId, ZString>("EDID"),
        Field<&ItemRec::vmad, VmadCodec>("VMAD"),
        Field<&ItemRec::bounds, Packed<ObjectBounds>>("OBND"),
        Field<&ItemRec::fullName, LString>("FULL"),
        Field<&ItemRec::model, ZString>("MODL"),
        Field<&ItemRec::modelData, Blob>("MODT"),
        Field<&ItemRec::modelSwap, Blob>("MODS"),
        Field<&ItemRec::icon, ZString>("ICON"),
        Field<&ItemRec::msgIcon, ZString>("MICO"),
        Field<&ItemRec::pickupSound, FormRefCodec>("YNAM"),
        Field<&ItemRec::dropSound, FormRefCodec>("ZNAM"),
        Field<&ItemRec::keywordCount, U32>("KSIZ"),
        Field<&ItemRec::keywords, FormRefArray>("KWDA"),
        Field<&ItemRec::valueWeight, Packed<ItemValueWeight>>("DATA"),
        Field<&ItemRec::soulLevel, U8v>("SOUL"),
        Field<&ItemRec::soulCapacity, U8v>("SLCP"),
        Field<&ItemRec::linkedTo, FormRefCodec>("NAM0"));
}
} // namespace schema_detail

inline const auto kMiscSchema = schema_detail::MakeItemSchema<0>("MISC");
inline const auto kKeymSchema = schema_detail::MakeItemSchema<1>("KEYM");
inline const auto kSlgmSchema = schema_detail::MakeItemSchema<2>("SLGM");

// ── WEAP ─────────────────────────────────────────────────────────────────────

struct WeapRec : RecordExtras {
    std::optional<std::string>          editorId;      // EDID
    std::optional<Vmad>                 vmad;          // VMAD
    std::optional<ObjectBounds>         bounds;        // OBND
    std::optional<Lstring>              fullName;      // FULL
    std::optional<std::string>          model;         // MODL
    std::optional<std::vector<uint8_t>> modelData;     // MODT
    std::optional<std::vector<uint8_t>> modelSwap;     // MODS
    std::optional<FormRef>              enchantment;   // EITM
    std::optional<uint16_t>             enchantAmount; // EAMT
    std::optional<FormRef>              equipType;     // ETYP
    std::optional<FormRef>              bashImpact;    // BIDS
    std::optional<FormRef>              bashMaterial;  // BAMT
    std::optional<FormRef>              pickupSound;   // YNAM
    std::optional<FormRef>              dropSound;     // ZNAM
    std::optional<uint32_t>             keywordCount;  // KSIZ
    std::optional<std::vector<FormRef>> keywords;      // KWDA
    std::optional<Lstring>              description;   // DESC
    std::optional<FormRef>              impactDataSet; // INAM
    std::optional<FormRef>              firstPersonModel; // WNAM
    std::optional<FormRef>              attackSound;   // SNAM
    std::optional<FormRef>              attackSound2d; // XNAM
    std::optional<FormRef>              attackLoop;    // NAM7
    std::optional<FormRef>              attackFail;    // TNAM
    std::optional<FormRef>              idleSound;     // UNAM
    std::optional<FormRef>              equipSound;    // NAM9
    std::optional<FormRef>              unequipSound;  // NAM8
    std::optional<WeaponBasic>          basic;         // DATA
    std::optional<std::vector<uint8_t>> dnam;          // DNAM (100B block, opaque)
    std::optional<WeaponCrit>           crit;          // CRDT
    std::optional<uint32_t>             detectionLevel;// VNAM
    std::optional<FormRef>              templateWeapon;// CNAM
};

inline const auto kWeapSchema = MakeSchema<WeapRec>("WEAP",
    Field<&WeapRec::editorId, ZString>("EDID"),
    Field<&WeapRec::vmad, VmadCodec>("VMAD"),
    Field<&WeapRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&WeapRec::fullName, LString>("FULL"),
    Field<&WeapRec::model, ZString>("MODL"),
    Field<&WeapRec::modelData, Blob>("MODT"),
    Field<&WeapRec::modelSwap, Blob>("MODS"),
    Field<&WeapRec::enchantment, FormRefCodec>("EITM"),
    Field<&WeapRec::enchantAmount, U16v>("EAMT"),
    Field<&WeapRec::equipType, FormRefCodec>("ETYP"),
    Field<&WeapRec::bashImpact, FormRefCodec>("BIDS"),
    Field<&WeapRec::bashMaterial, FormRefCodec>("BAMT"),
    Field<&WeapRec::pickupSound, FormRefCodec>("YNAM"),
    Field<&WeapRec::dropSound, FormRefCodec>("ZNAM"),
    Field<&WeapRec::keywordCount, U32>("KSIZ"),
    Field<&WeapRec::keywords, FormRefArray>("KWDA"),
    Field<&WeapRec::description, LStringDL>("DESC"),
    Field<&WeapRec::impactDataSet, FormRefCodec>("INAM"),
    Field<&WeapRec::firstPersonModel, FormRefCodec>("WNAM"),
    Field<&WeapRec::attackSound, FormRefCodec>("SNAM"),
    Field<&WeapRec::attackSound2d, FormRefCodec>("XNAM"),
    Field<&WeapRec::attackLoop, FormRefCodec>("NAM7"),
    Field<&WeapRec::attackFail, FormRefCodec>("TNAM"),
    Field<&WeapRec::idleSound, FormRefCodec>("UNAM"),
    Field<&WeapRec::equipSound, FormRefCodec>("NAM9"),
    Field<&WeapRec::unequipSound, FormRefCodec>("NAM8"),
    Field<&WeapRec::basic, Packed<WeaponBasic>>("DATA"),
    Field<&WeapRec::dnam, Blob>("DNAM"),
    Field<&WeapRec::crit, PackedIds<WeaponCrit, kWeaponCritIds>>("CRDT"),
    Field<&WeapRec::detectionLevel, U32>("VNAM"),
    Field<&WeapRec::templateWeapon, FormRefCodec>("CNAM"));

// ── AMMO ─────────────────────────────────────────────────────────────────────

struct AmmoRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::string>          icon;        // ICON
    std::optional<std::string>          msgIcon;     // MICO
    std::optional<FormRef>              pickupSound; // YNAM
    std::optional<FormRef>              dropSound;   // ZNAM
    std::optional<Lstring>              description; // DESC
    std::optional<uint32_t>             keywordCount;// KSIZ
    std::optional<std::vector<FormRef>> keywords;    // KWDA
    std::optional<AmmoData>             data;        // DATA
    std::optional<std::string>          shortName;   // ONAM
};

inline const auto kAmmoSchema = MakeSchema<AmmoRec>("AMMO",
    Field<&AmmoRec::editorId, ZString>("EDID"),
    Field<&AmmoRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&AmmoRec::fullName, LString>("FULL"),
    Field<&AmmoRec::model, ZString>("MODL"),
    Field<&AmmoRec::modelData, Blob>("MODT"),
    Field<&AmmoRec::icon, ZString>("ICON"),
    Field<&AmmoRec::msgIcon, ZString>("MICO"),
    Field<&AmmoRec::pickupSound, FormRefCodec>("YNAM"),
    Field<&AmmoRec::dropSound, FormRefCodec>("ZNAM"),
    Field<&AmmoRec::description, LStringDL>("DESC"),
    Field<&AmmoRec::keywordCount, U32>("KSIZ"),
    Field<&AmmoRec::keywords, FormRefArray>("KWDA"),
    Field<&AmmoRec::data, PackedIds<AmmoData, kAmmoDataIds>>("DATA"),
    Field<&AmmoRec::shortName, ZString>("ONAM"));

// ── ALCH / INGR — effect carriers ────────────────────────────────────────────

struct AlchRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<uint32_t>             keywordCount;// KSIZ
    std::optional<std::vector<FormRef>> keywords;    // KWDA
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::vector<uint8_t>> modelSwap;   // MODS
    std::optional<std::string>          icon;        // ICON
    std::optional<std::string>          msgIcon;     // MICO
    std::optional<FormRef>              pickupSound; // YNAM
    std::optional<FormRef>              dropSound;   // ZNAM
    std::optional<float>                weight;      // DATA
    std::optional<PotionData>           potion;      // ENIT
    std::vector<FormRef>                effects;     // EFID
    std::vector<EffectItem>             effectItems; // EFIT
    ConditionList                       conds;       // CTDA/CIS1/CIS2 (per effect)
};

inline const auto kAlchSchema = MakeSchema<AlchRec>("ALCH",
    Field<&AlchRec::editorId, ZString>("EDID"),
    Field<&AlchRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&AlchRec::fullName, LString>("FULL"),
    Field<&AlchRec::keywordCount, U32>("KSIZ"),
    Field<&AlchRec::keywords, FormRefArray>("KWDA"),
    Field<&AlchRec::model, ZString>("MODL"),
    Field<&AlchRec::modelData, Blob>("MODT"),
    Field<&AlchRec::modelSwap, Blob>("MODS"),
    Field<&AlchRec::icon, ZString>("ICON"),
    Field<&AlchRec::msgIcon, ZString>("MICO"),
    Field<&AlchRec::pickupSound, FormRefCodec>("YNAM"),
    Field<&AlchRec::dropSound, FormRefCodec>("ZNAM"),
    Field<&AlchRec::weight, F32>("DATA"),
    Field<&AlchRec::potion, PackedIds<PotionData, kPotionDataIds>>("ENIT"),
    Repeated<&AlchRec::effects, FormRefCodec>("EFID"),
    Repeated<&AlchRec::effectItems, Packed<EffectItem>>("EFIT"),
    CtdaRow<&AlchRec::conds>(),
    Cis1Row<&AlchRec::conds>(),
    Cis2Row<&AlchRec::conds>());

struct IngrRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Vmad>                 vmad;        // VMAD
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<uint32_t>             keywordCount;// KSIZ
    std::optional<std::vector<FormRef>> keywords;    // KWDA
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::string>          icon;        // ICON
    std::optional<FormRef>              pickupSound; // YNAM
    std::optional<FormRef>              dropSound;   // ZNAM
    std::optional<FormRef>              equipType;   // ETYP
    std::optional<ItemValueWeight>      valueWeight; // DATA
    std::optional<IngredientData>       data;        // ENIT
    std::vector<FormRef>                effects;     // EFID
    std::vector<EffectItem>             effectItems; // EFIT
    ConditionList                       conds;       // CTDA/CIS1/CIS2
};

inline const auto kIngrSchema = MakeSchema<IngrRec>("INGR",
    Field<&IngrRec::editorId, ZString>("EDID"),
    Field<&IngrRec::vmad, VmadCodec>("VMAD"),
    Field<&IngrRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&IngrRec::fullName, LString>("FULL"),
    Field<&IngrRec::keywordCount, U32>("KSIZ"),
    Field<&IngrRec::keywords, FormRefArray>("KWDA"),
    Field<&IngrRec::model, ZString>("MODL"),
    Field<&IngrRec::modelData, Blob>("MODT"),
    Field<&IngrRec::icon, ZString>("ICON"),
    Field<&IngrRec::pickupSound, FormRefCodec>("YNAM"),
    Field<&IngrRec::dropSound, FormRefCodec>("ZNAM"),
    Field<&IngrRec::equipType, FormRefCodec>("ETYP"),
    Field<&IngrRec::valueWeight, Packed<ItemValueWeight>>("DATA"),
    Field<&IngrRec::data, Packed<IngredientData>>("ENIT"),
    Repeated<&IngrRec::effects, FormRefCodec>("EFID"),
    Repeated<&IngrRec::effectItems, Packed<EffectItem>>("EFIT"),
    CtdaRow<&IngrRec::conds>(),
    Cis1Row<&IngrRec::conds>(),
    Cis2Row<&IngrRec::conds>());

// ── BOOK ─────────────────────────────────────────────────────────────────────
// DATA stays passthrough: its "teaches" field is conditionally a FormID
// (spell flag), which the visitor can't express until modeled precisely.

struct BookRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Vmad>                 vmad;        // VMAD
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<std::vector<uint8_t>> modelSwap;   // MODS
    std::optional<Lstring>              bookText;    // DESC (the book body)
    std::optional<std::string>          icon;        // ICON
    std::optional<std::string>          msgIcon;     // MICO
    std::optional<FormRef>              pickupSound; // YNAM
    std::optional<FormRef>              dropSound;   // ZNAM
    std::optional<uint32_t>             keywordCount;// KSIZ
    std::optional<std::vector<FormRef>> keywords;    // KWDA
    std::optional<FormRef>              inventoryArt;// INAM
    std::optional<Lstring>              description; // CNAM (inventory description)
};

inline const auto kBookSchema = MakeSchema<BookRec>("BOOK",
    Field<&BookRec::editorId, ZString>("EDID"),
    Field<&BookRec::vmad, VmadCodec>("VMAD"),
    Field<&BookRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&BookRec::fullName, LString>("FULL"),
    Field<&BookRec::model, ZString>("MODL"),
    Field<&BookRec::modelData, Blob>("MODT"),
    Field<&BookRec::modelSwap, Blob>("MODS"),
    Field<&BookRec::bookText, LStringDL>("DESC"),
    Field<&BookRec::icon, ZString>("ICON"),
    Field<&BookRec::msgIcon, ZString>("MICO"),
    Field<&BookRec::pickupSound, FormRefCodec>("YNAM"),
    Field<&BookRec::dropSound, FormRefCodec>("ZNAM"),
    Field<&BookRec::keywordCount, U32>("KSIZ"),
    Field<&BookRec::keywords, FormRefArray>("KWDA"),
    Field<&BookRec::inventoryArt, FormRefCodec>("INAM"),
    Field<&BookRec::description, LStringDL>("CNAM"));

// ── SPEL / SCRL ──────────────────────────────────────────────────────────────

struct SpellRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<uint32_t>             keywordCount;// KSIZ
    std::optional<std::vector<FormRef>> keywords;    // KWDA
    std::optional<FormRef>              menuDisplayObject; // MDOB
    std::optional<FormRef>              equipType;   // ETYP
    std::optional<Lstring>              description; // DESC
    std::optional<std::string>          model;       // MODL (scrolls)
    std::optional<std::vector<uint8_t>> modelData;   // MODT
    std::optional<ItemValueWeight>      valueWeight; // DATA (scrolls)
    std::optional<SpellData>            spit;        // SPIT
    std::vector<FormRef>                effects;     // EFID
    std::vector<EffectItem>             effectItems; // EFIT
    ConditionList                       conds;       // CTDA/CIS1/CIS2
};

namespace schema_detail {
template <uint32_t /*tag*/>
inline auto MakeSpellSchema(const char sig[5]) {
    return MakeSchema<SpellRec>(sig,
        Field<&SpellRec::editorId, ZString>("EDID"),
        Field<&SpellRec::bounds, Packed<ObjectBounds>>("OBND"),
        Field<&SpellRec::fullName, LString>("FULL"),
        Field<&SpellRec::keywordCount, U32>("KSIZ"),
        Field<&SpellRec::keywords, FormRefArray>("KWDA"),
        Field<&SpellRec::menuDisplayObject, FormRefCodec>("MDOB"),
        Field<&SpellRec::equipType, FormRefCodec>("ETYP"),
        Field<&SpellRec::description, LStringDL>("DESC"),
        Field<&SpellRec::model, ZString>("MODL"),
        Field<&SpellRec::modelData, Blob>("MODT"),
        Field<&SpellRec::valueWeight, Packed<ItemValueWeight>>("DATA"),
        Field<&SpellRec::spit, PackedIds<SpellData, kSpellDataIds>>("SPIT"),
        Repeated<&SpellRec::effects, FormRefCodec>("EFID"),
        Repeated<&SpellRec::effectItems, Packed<EffectItem>>("EFIT"),
        CtdaRow<&SpellRec::conds>(),
        Cis1Row<&SpellRec::conds>(),
        Cis2Row<&SpellRec::conds>());
}
} // namespace schema_detail

inline const auto kSpelSchema = schema_detail::MakeSpellSchema<0>("SPEL");
inline const auto kScrlSchema = schema_detail::MakeSpellSchema<1>("SCRL");

// ── ENCH ─────────────────────────────────────────────────────────────────────

struct EnchRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<ObjectBounds>         bounds;      // OBND
    std::optional<Lstring>              fullName;    // FULL
    std::optional<EnchantData>          data;        // ENIT
    std::vector<FormRef>                effects;     // EFID
    std::vector<EffectItem>             effectItems; // EFIT
    ConditionList                       conds;       // CTDA/CIS1/CIS2
};

inline const auto kEnchSchema = MakeSchema<EnchRec>("ENCH",
    Field<&EnchRec::editorId, ZString>("EDID"),
    Field<&EnchRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&EnchRec::fullName, LString>("FULL"),
    Field<&EnchRec::data, PackedIds<EnchantData, kEnchantDataIds>>("ENIT"),
    Repeated<&EnchRec::effects, FormRefCodec>("EFID"),
    Repeated<&EnchRec::effectItems, Packed<EffectItem>>("EFIT"),
    CtdaRow<&EnchRec::conds>(),
    Cis1Row<&EnchRec::conds>(),
    Cis2Row<&EnchRec::conds>());

// ── SHOU ─────────────────────────────────────────────────────────────────────

struct ShouRec : RecordExtras {
    std::optional<std::string> editorId;             // EDID
    std::optional<Lstring>     fullName;             // FULL
    std::optional<FormRef>     menuDisplayObject;    // MDOB
    std::optional<Lstring>     description;          // DESC
    std::vector<ShoutWord>     words;                // SNAM ×3
};

inline const auto kShouSchema = MakeSchema<ShouRec>("SHOU",
    Field<&ShouRec::editorId, ZString>("EDID"),
    Field<&ShouRec::fullName, LString>("FULL"),
    Field<&ShouRec::menuDisplayObject, FormRefCodec>("MDOB"),
    Field<&ShouRec::description, LStringDL>("DESC"),
    Repeated<&ShouRec::words, PackedIds<ShoutWord, kShoutWordIds>>("SNAM"));

// ── WOOP ─────────────────────────────────────────────────────────────────────

struct WoopRec : RecordExtras {
    std::optional<std::string> editorId;    // EDID
    std::optional<Lstring>     fullName;    // FULL
    std::optional<Lstring>     translation; // TNAM
};

inline const auto kWoopSchema = MakeSchema<WoopRec>("WOOP",
    Field<&WoopRec::editorId, ZString>("EDID"),
    Field<&WoopRec::fullName, LString>("FULL"),
    Field<&WoopRec::translation, LString>("TNAM"));

// ── MGEF / PERK / AVIF — thin shells (heavy sections stay passthrough) ───────

struct MgefRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Vmad>                 vmad;        // VMAD
    std::optional<Lstring>              fullName;    // FULL
    std::optional<FormRef>              menuDisplayObject; // MDOB
    std::optional<uint32_t>             keywordCount;// KSIZ
    std::optional<std::vector<FormRef>> keywords;    // KWDA
    std::optional<Lstring>              description; // DNAM
    ConditionList                       conds;       // CTDA/CIS1/CIS2
};

inline const auto kMgefSchema = MakeSchema<MgefRec>("MGEF",
    Field<&MgefRec::editorId, ZString>("EDID"),
    Field<&MgefRec::vmad, VmadCodec>("VMAD"),
    Field<&MgefRec::fullName, LString>("FULL"),
    Field<&MgefRec::menuDisplayObject, FormRefCodec>("MDOB"),
    Field<&MgefRec::keywordCount, U32>("KSIZ"),
    Field<&MgefRec::keywords, FormRefArray>("KWDA"),
    Field<&MgefRec::description, LStringDL>("DNAM"),
    CtdaRow<&MgefRec::conds>(),
    Cis1Row<&MgefRec::conds>(),
    Cis2Row<&MgefRec::conds>());

struct PerkRec : RecordExtras {
    std::optional<std::string> editorId;    // EDID
    std::optional<Vmad>        vmad;        // VMAD
    std::optional<Lstring>     fullName;    // FULL
    std::optional<Lstring>     description; // DESC
    std::optional<std::string> icon;        // ICON
    std::optional<FormRef>     nextPerk;    // NNAM
    ConditionList              conds;       // CTDA/CIS1/CIS2
};

inline const auto kPerkSchema = MakeSchema<PerkRec>("PERK",
    Field<&PerkRec::editorId, ZString>("EDID"),
    Field<&PerkRec::vmad, VmadCodec>("VMAD"),
    Field<&PerkRec::fullName, LString>("FULL"),
    Field<&PerkRec::description, LStringDL>("DESC"),
    Field<&PerkRec::icon, ZString>("ICON"),
    Field<&PerkRec::nextPerk, FormRefCodec>("NNAM"),
    CtdaRow<&PerkRec::conds>(),
    Cis1Row<&PerkRec::conds>(),
    Cis2Row<&PerkRec::conds>());

struct AvifRec : RecordExtras {
    std::optional<std::string> editorId;     // EDID
    std::optional<Lstring>     fullName;     // FULL
    std::optional<Lstring>     description;  // DESC
    std::optional<std::string> abbreviation; // ANAM
};

inline const auto kAvifSchema = MakeSchema<AvifRec>("AVIF",
    Field<&AvifRec::editorId, ZString>("EDID"),
    Field<&AvifRec::fullName, LString>("FULL"),
    Field<&AvifRec::description, LStringDL>("DESC"),
    Field<&AvifRec::abbreviation, ZString>("ANAM"));

} // namespace esper
