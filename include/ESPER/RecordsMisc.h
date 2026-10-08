#pragma once
#include <ESPER/RecordsCommon.h>

namespace esper {

// ── TXST — texture set ────────────────────────────────────────────────────────

struct TxstRec : RecordExtras {
    std::optional<std::string>  editorId;      // EDID
    std::optional<ObjectBounds> bounds;        // OBND
    std::optional<std::string>  tx00;          // diffuse
    std::optional<std::string>  tx01;          // normal/gloss
    std::optional<std::string>  tx02;          // environment mask / subsurface
    std::optional<std::string>  tx03;          // glow / detail
    std::optional<std::string>  tx04;          // height
    std::optional<std::string>  tx05;          // environment
    std::optional<std::string>  tx06;          // multilayer
    std::optional<std::string>  tx07;          // backlight mask / specular
    std::optional<DecalData>    decal;         // DODT
    std::optional<uint16_t>     flags;         // DNAM
};

inline const auto kTxstSchema = MakeSchema<TxstRec>("TXST",
    Field<&TxstRec::editorId, ZString>("EDID"),
    Field<&TxstRec::bounds, Packed<ObjectBounds>>("OBND"),
    Field<&TxstRec::tx00, ZString>("TX00"),
    Field<&TxstRec::tx01, ZString>("TX01"),
    Field<&TxstRec::tx02, ZString>("TX02"),
    Field<&TxstRec::tx03, ZString>("TX03"),
    Field<&TxstRec::tx04, ZString>("TX04"),
    Field<&TxstRec::tx05, ZString>("TX05"),
    Field<&TxstRec::tx06, ZString>("TX06"),
    Field<&TxstRec::tx07, ZString>("TX07"),
    Field<&TxstRec::decal, Packed<DecalData>>("DODT"),
    Field<&TxstRec::flags, U16v>("DNAM"));

// ── HDPT — head part ──────────────────────────────────────────────────────────
// NAM0 (part type) / NAM1 (part file path) form repeating PAIRS; each is
// collected in encounter order and `layout` preserves the interleaving, so
// pairing is positional (NAM1[i] belongs to NAM0[i]).

struct HdptRec : RecordExtras {
    std::optional<std::string>          editorId;    // EDID
    std::optional<Lstring>              fullName;    // FULL
    std::optional<std::string>          model;       // MODL
    std::optional<std::vector<uint8_t>> modelData;   // MODT (texture hashes, opaque)
    std::optional<uint8_t>              flags;       // DATA
    std::optional<uint32_t>             type;        // PNAM (0=misc 1=face 2=eyes 3=hair …)
    std::vector<FormRef>                extraParts;  // HNAM (repeated)
    std::vector<uint32_t>               partTypes;   // NAM0 (0=race morph 1=tri 2=chargen)
    std::vector<std::string>            partFiles;   // NAM1
    std::optional<FormRef>              textureSet;  // TNAM
    std::optional<FormRef>              color;       // CNAM (CLFM)
    std::optional<FormRef>              validRaces;  // RNAM (FLST)
};

inline const auto kHdptSchema = MakeSchema<HdptRec>("HDPT",
    Field<&HdptRec::editorId, ZString>("EDID"),
    Field<&HdptRec::fullName, LString>("FULL"),
    Field<&HdptRec::model, ZString>("MODL"),
    Field<&HdptRec::modelData, Blob>("MODT"),
    Field<&HdptRec::flags, U8v>("DATA"),
    Field<&HdptRec::type, U32>("PNAM"),
    Repeated<&HdptRec::extraParts, FormRefCodec>("HNAM"),
    Repeated<&HdptRec::partTypes, U32>("NAM0"),
    Repeated<&HdptRec::partFiles, ZString>("NAM1"),
    Field<&HdptRec::textureSet, FormRefCodec>("TNAM"),
    Field<&HdptRec::color, FormRefCodec>("CNAM"),
    Field<&HdptRec::validRaces, FormRefCodec>("RNAM"));

} // namespace esper
