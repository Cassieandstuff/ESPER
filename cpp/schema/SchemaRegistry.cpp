#include <ESPER/SchemaRegistry.h>

#include <ESPER/RecordsActor.h>
#include <ESPER/RecordsItem.h>
#include <ESPER/RecordsFx.h>
#include <ESPER/RecordsMagic.h>
#include <ESPER/RecordsMore.h>
#include <ESPER/RecordsMisc.h>
#include <ESPER/RecordsStory.h>
#include <ESPER/RecordsSystem.h>
#include <ESPER/RecordsWorld.h>

#include <cstdio>
#include <cstring>

namespace esper {

namespace {

template <const auto& Schema>
bool RoundtripImpl(std::span<const uint8_t> payload, const SchemaCtx& ctx,
                   size_t& totalSubs, size_t& unknownSubs, std::string& why) {
    using Rec = typename std::remove_reference_t<decltype(Schema)>::RecType;
    Rec rec;
    if (!DecodeRecord(Schema, payload, ctx, rec)) {
        why = "truncated/malformed subrecord stream";
        return false;
    }
    totalSubs   = rec.layout.size();
    unknownSubs = rec.unknown.size();

    ByteWriter w;
    EncodeRecord(Schema, rec, ctx, w);

    if (w.buf.size() != payload.size()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "size mismatch: original %zu, emitted %zu",
                      payload.size(), w.buf.size());
        why = buf;
        return false;
    }
    if (!payload.empty() &&
        std::memcmp(w.buf.data(), payload.data(), payload.size()) != 0) {
        size_t at = 0;
        while (at < payload.size() && w.buf[at] == payload[at]) ++at;
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "byte mismatch at payload offset %zu (orig 0x%02X, emitted 0x%02X)",
                      at, payload[at], w.buf[at]);
        why = buf;
        return false;
    }
    return true;
}

template <const auto& Schema>
bool RemapIdsImpl(std::span<const uint8_t> payload, const SchemaCtx& ctx,
                  void (*fn)(uint32_t&, void*), void* userCtx,
                  std::vector<uint8_t>& outBytes, std::string& why) {
    using Rec = typename std::remove_reference_t<decltype(Schema)>::RecType;
    Rec rec;
    if (!DecodeRecord(Schema, payload, ctx, rec)) {
        why = "truncated/malformed subrecord stream";
        return false;
    }
    if (!rec.unknown.empty()) {
        char tag[5] = {};
        std::memcpy(tag, &rec.unknown.front().sig, 4);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "%zu unknown-passthrough subrecord(s) (first: %s) — refusing "
                      "FormID remap over unseen bytes",
                      rec.unknown.size(), tag);
        why = buf;
        return false;
    }
    for (const auto& row : Schema.rows) {
        if (row.remapSafe && !row.remapSafe(rec)) {
            char tag[5] = {};
            std::memcpy(tag, &row.sig, 4);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "record carries %s data that cannot be safely FormID-remapped "
                          "(e.g. an unknown condition function) — refusing FormID remap",
                          tag);
            why = buf;
            return false;
        }
    }
    VisitFormIds(Schema, rec, fn, userCtx);
    ByteWriter w;
    EncodeRecord(Schema, rec, ctx, w);
    outBytes = std::move(w.buf);
    return true;
}

const SchemaOps kOps[] = {
    // Wave 1 — actor/appearance.
    { kNpcSchema.sig,  &RoundtripImpl<kNpcSchema>,  &RemapIdsImpl<kNpcSchema>  },
    { kRaceSchema.sig, &RoundtripImpl<kRaceSchema>, &RemapIdsImpl<kRaceSchema> },
    { kArmoSchema.sig, &RoundtripImpl<kArmoSchema>, &RemapIdsImpl<kArmoSchema> },
    { kArmaSchema.sig, &RoundtripImpl<kArmaSchema>, &RemapIdsImpl<kArmaSchema> },
    { kHdptSchema.sig, &RoundtripImpl<kHdptSchema>, &RemapIdsImpl<kHdptSchema> },
    { kTxstSchema.sig, &RoundtripImpl<kTxstSchema>, &RemapIdsImpl<kTxstSchema> },
    // Wave 2 — cell/world/terrain.
    { kWrldSchema.sig, &RoundtripImpl<kWrldSchema>, &RemapIdsImpl<kWrldSchema> },
    { kCellSchema.sig, &RoundtripImpl<kCellSchema>, &RemapIdsImpl<kCellSchema> },
    { kRefrSchema.sig, &RoundtripImpl<kRefrSchema>, &RemapIdsImpl<kRefrSchema> },
    { kAchrSchema.sig, &RoundtripImpl<kAchrSchema>, &RemapIdsImpl<kAchrSchema> },
    { kLandSchema.sig, &RoundtripImpl<kLandSchema>, &RemapIdsImpl<kLandSchema> },
    { kLtexSchema.sig, &RoundtripImpl<kLtexSchema>, &RemapIdsImpl<kLtexSchema> },
    { kStatSchema.sig, &RoundtripImpl<kStatSchema>, &RemapIdsImpl<kStatSchema> },
    { kMsttSchema.sig, &RoundtripImpl<kMsttSchema>, &RemapIdsImpl<kMsttSchema> },
    { kTreeSchema.sig, &RoundtripImpl<kTreeSchema>, &RemapIdsImpl<kTreeSchema> },
    { kActiSchema.sig, &RoundtripImpl<kActiSchema>, &RemapIdsImpl<kActiSchema> },
    { kContSchema.sig, &RoundtripImpl<kContSchema>, &RemapIdsImpl<kContSchema> },
    { kDoorSchema.sig, &RoundtripImpl<kDoorSchema>, &RemapIdsImpl<kDoorSchema> },
    { kLighSchema.sig, &RoundtripImpl<kLighSchema>, &RemapIdsImpl<kLighSchema> },
    { kFurnSchema.sig, &RoundtripImpl<kFurnSchema>, &RemapIdsImpl<kFurnSchema> },
    // Wave 3 — items/magic (CTDA stays passthrough until the function table).
    { kMiscSchema.sig, &RoundtripImpl<kMiscSchema>, &RemapIdsImpl<kMiscSchema> },
    { kKeymSchema.sig, &RoundtripImpl<kKeymSchema>, &RemapIdsImpl<kKeymSchema> },
    { kSlgmSchema.sig, &RoundtripImpl<kSlgmSchema>, &RemapIdsImpl<kSlgmSchema> },
    { kWeapSchema.sig, &RoundtripImpl<kWeapSchema>, &RemapIdsImpl<kWeapSchema> },
    { kAmmoSchema.sig, &RoundtripImpl<kAmmoSchema>, &RemapIdsImpl<kAmmoSchema> },
    { kAlchSchema.sig, &RoundtripImpl<kAlchSchema>, &RemapIdsImpl<kAlchSchema> },
    { kIngrSchema.sig, &RoundtripImpl<kIngrSchema>, &RemapIdsImpl<kIngrSchema> },
    { kBookSchema.sig, &RoundtripImpl<kBookSchema>, &RemapIdsImpl<kBookSchema> },
    { kSpelSchema.sig, &RoundtripImpl<kSpelSchema>, &RemapIdsImpl<kSpelSchema> },
    { kScrlSchema.sig, &RoundtripImpl<kScrlSchema>, &RemapIdsImpl<kScrlSchema> },
    { kEnchSchema.sig, &RoundtripImpl<kEnchSchema>, &RemapIdsImpl<kEnchSchema> },
    { kShouSchema.sig, &RoundtripImpl<kShouSchema>, &RemapIdsImpl<kShouSchema> },
    { kWoopSchema.sig, &RoundtripImpl<kWoopSchema>, &RemapIdsImpl<kWoopSchema> },
    { kMgefSchema.sig, &RoundtripImpl<kMgefSchema>, &RemapIdsImpl<kMgefSchema> },
    { kPerkSchema.sig, &RoundtripImpl<kPerkSchema>, &RemapIdsImpl<kPerkSchema> },
    { kAvifSchema.sig, &RoundtripImpl<kAvifSchema>, &RemapIdsImpl<kAvifSchema> },
    // Wave 4 — actors/leveled/story/dialogue.
    { kLvlnSchema.sig, &RoundtripImpl<kLvlnSchema>, &RemapIdsImpl<kLvlnSchema> },
    { kLvliSchema.sig, &RoundtripImpl<kLvliSchema>, &RemapIdsImpl<kLvliSchema> },
    { kLvspSchema.sig, &RoundtripImpl<kLvspSchema>, &RemapIdsImpl<kLvspSchema> },
    { kFactSchema.sig, &RoundtripImpl<kFactSchema>, &RemapIdsImpl<kFactSchema> },
    { kClasSchema.sig, &RoundtripImpl<kClasSchema>, &RemapIdsImpl<kClasSchema> },
    { kOtftSchema.sig, &RoundtripImpl<kOtftSchema>, &RemapIdsImpl<kOtftSchema> },
    { kEyesSchema.sig, &RoundtripImpl<kEyesSchema>, &RemapIdsImpl<kEyesSchema> },
    { kAspcSchema.sig, &RoundtripImpl<kAspcSchema>, &RemapIdsImpl<kAspcSchema> },
    { kPackSchema.sig, &RoundtripImpl<kPackSchema>, &RemapIdsImpl<kPackSchema> },
    { kQustSchema.sig, &RoundtripImpl<kQustSchema>, &RemapIdsImpl<kQustSchema> },
    { kScenSchema.sig, &RoundtripImpl<kScenSchema>, &RemapIdsImpl<kScenSchema> },
    { kDlbrSchema.sig, &RoundtripImpl<kDlbrSchema>, &RemapIdsImpl<kDlbrSchema> },
    { kDlvwSchema.sig, &RoundtripImpl<kDlvwSchema>, &RemapIdsImpl<kDlvwSchema> },
    { kDialSchema.sig, &RoundtripImpl<kDialSchema>, &RemapIdsImpl<kDialSchema> },
    { kInfoSchema.sig, &RoundtripImpl<kInfoSchema>, &RemapIdsImpl<kInfoSchema> },
    // Wave 5 — audio/visual/fx.
    { kSounSchema.sig, &RoundtripImpl<kSounSchema>, &RemapIdsImpl<kSounSchema> },
    { kSndrSchema.sig, &RoundtripImpl<kSndrSchema>, &RemapIdsImpl<kSndrSchema> },
    { kSopmSchema.sig, &RoundtripImpl<kSopmSchema>, &RemapIdsImpl<kSopmSchema> },
    { kMuscSchema.sig, &RoundtripImpl<kMuscSchema>, &RemapIdsImpl<kMuscSchema> },
    { kMustSchema.sig, &RoundtripImpl<kMustSchema>, &RemapIdsImpl<kMustSchema> },
    { kRevbSchema.sig, &RoundtripImpl<kRevbSchema>, &RemapIdsImpl<kRevbSchema> },
    { kImgsSchema.sig, &RoundtripImpl<kImgsSchema>, &RemapIdsImpl<kImgsSchema> },
    { kImadSchema.sig, &RoundtripImpl<kImadSchema>, &RemapIdsImpl<kImadSchema> },
    { kEfshSchema.sig, &RoundtripImpl<kEfshSchema>, &RemapIdsImpl<kEfshSchema> },
    { kExplSchema.sig, &RoundtripImpl<kExplSchema>, &RemapIdsImpl<kExplSchema> },
    { kProjSchema.sig, &RoundtripImpl<kProjSchema>, &RemapIdsImpl<kProjSchema> },
    { kIpctSchema.sig, &RoundtripImpl<kIpctSchema>, &RemapIdsImpl<kIpctSchema> },
    { kIpdsSchema.sig, &RoundtripImpl<kIpdsSchema>, &RemapIdsImpl<kIpdsSchema> },
    { kCamsSchema.sig, &RoundtripImpl<kCamsSchema>, &RemapIdsImpl<kCamsSchema> },
    { kCpthSchema.sig, &RoundtripImpl<kCpthSchema>, &RemapIdsImpl<kCpthSchema> },
    { kDebrSchema.sig, &RoundtripImpl<kDebrSchema>, &RemapIdsImpl<kDebrSchema> },
    { kRfctSchema.sig, &RoundtripImpl<kRfctSchema>, &RemapIdsImpl<kRfctSchema> },
    { kAddnSchema.sig, &RoundtripImpl<kAddnSchema>, &RemapIdsImpl<kAddnSchema> },
    { kArtoSchema.sig, &RoundtripImpl<kArtoSchema>, &RemapIdsImpl<kArtoSchema> },
    { kMatoSchema.sig, &RoundtripImpl<kMatoSchema>, &RemapIdsImpl<kMatoSchema> },
    // Wave 6 — world/system remainder.
    { kGmstSchema.sig, &RoundtripImpl<kGmstSchema>, &RemapIdsImpl<kGmstSchema> },
    { kGlobSchema.sig, &RoundtripImpl<kGlobSchema>, &RemapIdsImpl<kGlobSchema> },
    { kKywdSchema.sig, &RoundtripImpl<kKywdSchema>, &RemapIdsImpl<kKywdSchema> },
    { kLcrtSchema.sig, &RoundtripImpl<kLcrtSchema>, &RemapIdsImpl<kLcrtSchema> },
    { kAactSchema.sig, &RoundtripImpl<kAactSchema>, &RemapIdsImpl<kAactSchema> },
    { kClfmSchema.sig, &RoundtripImpl<kClfmSchema>, &RemapIdsImpl<kClfmSchema> },
    { kVtypSchema.sig, &RoundtripImpl<kVtypSchema>, &RemapIdsImpl<kVtypSchema> },
    { kEqupSchema.sig, &RoundtripImpl<kEqupSchema>, &RemapIdsImpl<kEqupSchema> },
    { kCollSchema.sig, &RoundtripImpl<kCollSchema>, &RemapIdsImpl<kCollSchema> },
    { kFlstSchema.sig, &RoundtripImpl<kFlstSchema>, &RemapIdsImpl<kFlstSchema> },
    { kDobjSchema.sig, &RoundtripImpl<kDobjSchema>, &RemapIdsImpl<kDobjSchema> },
    { kIdleSchema.sig, &RoundtripImpl<kIdleSchema>, &RemapIdsImpl<kIdleSchema> },
    { kIdlmSchema.sig, &RoundtripImpl<kIdlmSchema>, &RemapIdsImpl<kIdlmSchema> },
    { kMovtSchema.sig, &RoundtripImpl<kMovtSchema>, &RemapIdsImpl<kMovtSchema> },
    { kGrasSchema.sig, &RoundtripImpl<kGrasSchema>, &RemapIdsImpl<kGrasSchema> },
    { kEcznSchema.sig, &RoundtripImpl<kEcznSchema>, &RemapIdsImpl<kEcznSchema> },
    { kLctnSchema.sig, &RoundtripImpl<kLctnSchema>, &RemapIdsImpl<kLctnSchema> },
    { kClmtSchema.sig, &RoundtripImpl<kClmtSchema>, &RemapIdsImpl<kClmtSchema> },
    { kWatrSchema.sig, &RoundtripImpl<kWatrSchema>, &RemapIdsImpl<kWatrSchema> },
    // Wave 7 — remaining roster (closes the vanilla + AE CC record types).
    { kAnioSchema.sig, &RoundtripImpl<kAnioSchema>, &RemapIdsImpl<kAnioSchema> },
    { kAppaSchema.sig, &RoundtripImpl<kAppaSchema>, &RemapIdsImpl<kAppaSchema> },
    { kTactSchema.sig, &RoundtripImpl<kTactSchema>, &RemapIdsImpl<kTactSchema> },
    { kFlorSchema.sig, &RoundtripImpl<kFlorSchema>, &RemapIdsImpl<kFlorSchema> },
    { kCobjSchema.sig, &RoundtripImpl<kCobjSchema>, &RemapIdsImpl<kCobjSchema> },
    { kAstpSchema.sig, &RoundtripImpl<kAstpSchema>, &RemapIdsImpl<kAstpSchema> },
    { kMattSchema.sig, &RoundtripImpl<kMattSchema>, &RemapIdsImpl<kMattSchema> },
    { kSnctSchema.sig, &RoundtripImpl<kSnctSchema>, &RemapIdsImpl<kSnctSchema> },
    { kDualSchema.sig, &RoundtripImpl<kDualSchema>, &RemapIdsImpl<kDualSchema> },
    { kHazdSchema.sig, &RoundtripImpl<kHazdSchema>, &RemapIdsImpl<kHazdSchema> },
    { kCstySchema.sig, &RoundtripImpl<kCstySchema>, &RemapIdsImpl<kCstySchema> },
    { kLgtmSchema.sig, &RoundtripImpl<kLgtmSchema>, &RemapIdsImpl<kLgtmSchema> },
    { kSpgdSchema.sig, &RoundtripImpl<kSpgdSchema>, &RemapIdsImpl<kSpgdSchema> },
    { kFstpSchema.sig, &RoundtripImpl<kFstpSchema>, &RemapIdsImpl<kFstpSchema> },
    { kFstsSchema.sig, &RoundtripImpl<kFstsSchema>, &RemapIdsImpl<kFstsSchema> },
    { kLscrSchema.sig, &RoundtripImpl<kLscrSchema>, &RemapIdsImpl<kLscrSchema> },
    { kMesgSchema.sig, &RoundtripImpl<kMesgSchema>, &RemapIdsImpl<kMesgSchema> },
    { kSmbnSchema.sig, &RoundtripImpl<kSmbnSchema>, &RemapIdsImpl<kSmbnSchema> },
    { kSmqnSchema.sig, &RoundtripImpl<kSmqnSchema>, &RemapIdsImpl<kSmqnSchema> },
    { kSmenSchema.sig, &RoundtripImpl<kSmenSchema>, &RemapIdsImpl<kSmenSchema> },
    { kRelaSchema.sig, &RoundtripImpl<kRelaSchema>, &RemapIdsImpl<kRelaSchema> },
    { kRegnSchema.sig, &RoundtripImpl<kRegnSchema>, &RemapIdsImpl<kRegnSchema> },
    { kPwatSchema.sig, &RoundtripImpl<kPwatSchema>, &RemapIdsImpl<kPwatSchema> },
    { kWthrSchema.sig, &RoundtripImpl<kWthrSchema>, &RemapIdsImpl<kWthrSchema> },
    { kNavmSchema.sig, &RoundtripImpl<kNavmSchema>, &RemapIdsImpl<kNavmSchema> },
    { kNaviSchema.sig, &RoundtripImpl<kNaviSchema>, &RemapIdsImpl<kNaviSchema> },
    { kPgreSchema.sig, &RoundtripImpl<kPgreSchema>, &RemapIdsImpl<kPgreSchema> },
    { kPhzdSchema.sig, &RoundtripImpl<kPhzdSchema>, &RemapIdsImpl<kPhzdSchema> },
    { kBptdSchema.sig, &RoundtripImpl<kBptdSchema>, &RemapIdsImpl<kBptdSchema> },
    { kVoliSchema.sig, &RoundtripImpl<kVoliSchema>, &RemapIdsImpl<kVoliSchema> },
};

} // namespace

std::span<const SchemaOps> AllSchemas() { return kOps; }

const SchemaOps* FindSchema(uint32_t sig) {
    for (const SchemaOps& o : kOps)
        if (o.sig == sig) return &o;
    return nullptr;
}

} // namespace esper
