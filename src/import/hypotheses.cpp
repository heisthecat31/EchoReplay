#include "import/hypotheses.h"

namespace replay::import {

Result CheckPoseFacts(const EmitFacts& facts) {
    if (!facts.worldAxesMatchEngine.value) {
        return Refuse(ResultCode::BadArguments, "%s says API and engine world axes differ; no transform is defined",
                      facts.worldAxesMatchEngine.id);
    }
    if (!facts.handAxesDirect.value) {
        return Refuse(ResultCode::BadArguments, "%s says hand axes are not direct; no hand offset rotation is defined",
                      facts.handAxesDirect.id);
    }
    if (facts.hmdOri.value == OriSource::SameAsHmd) {
        return Refuse(ResultCode::BadArguments, "%s cannot be SameAsHmd", facts.hmdOri.id);
    }
    return Success();
}

void ComFromHead(const float headPos[3], const EmitFacts& facts, float com[3]) {
    const Vec3f& d = facts.comFromHead.value;
    com[0] = headPos[0] + d.x;
    com[1] = headPos[1] + d.y;
    com[2] = headPos[2] + d.z;
}

const float* HmdQuat(const SrcPlayer& player, const EmitFacts& facts) {
    return facts.hmdOri.value == OriSource::BodyBasis ? player.body.q : player.head.q;
}

const float* BodyQuat(const SrcPlayer& player, const EmitFacts& facts) {
    switch (facts.bodyOri.value) {
        case OriSource::HeadBasis: return player.head.q;
        case OriSource::BodyBasis: return player.body.q;
        case OriSource::SameAsHmd: break;
    }
    return HmdQuat(player, facts);
}

const float* HandQuat(const Pose& hand, const EmitFacts&) { return hand.q; }

uint16_t DiscOwnerSlot(const SourceMatch& match, const SrcFrame& frame, DiscOwnerMeaning meaning,
                       uint16_t previousOwner) {
    uint16_t lowest = kNoSlot;
    for (uint32_t i = frame.firstPlayer; i < frame.firstPlayer + frame.playerCount; ++i) {
        const SrcPlayer& p = match.players[i];
        const bool holding = p.holdL == Hold::Disc || p.holdR == Hold::Disc;
        const bool claims = meaning == DiscOwnerMeaning::InHand ? holding : holding || p.possession;
        if (!claims) continue;
        if (p.rosterIndex == previousOwner) return previousOwner;
        if (p.rosterIndex < lowest) lowest = p.rosterIndex;
    }
    return lowest;
}

void QuatAxis(const float q[4], int axis, float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    switch (axis) {
        case 0:
            out[0] = 1 - 2 * (y * y + z * z);
            out[1] = 2 * (x * y + w * z);
            out[2] = 2 * (x * z - w * y);
            return;
        case 1:
            out[0] = 2 * (x * y - w * z);
            out[1] = 1 - 2 * (x * x + z * z);
            out[2] = 2 * (y * z + w * x);
            return;
        default:
            out[0] = 2 * (x * z + w * y);
            out[1] = 2 * (y * z - w * x);
            out[2] = 1 - 2 * (x * x + y * y);
            return;
    }
}

}  // namespace replay::import
