#pragma once

// The pose hypotheses H0-H5 (DESIGN-FORMATS §5.3), each as one function of its fact in EmitFacts.
// Emit and Infer read poses only through these, so P1's verdicts change table values, never call sites.

#include <stdint.h>

#include "import/import_facts.h"
#include "import/result.h"
#include "import/sourcemodel.h"

namespace replay::import {

// Refuses fact values this converter has no defined transform for: H0 false (a world mirror would
// also swap the hands' handedness) and H4 false (P1 would have to supply the offset rotation), and
// an hmdOri of SameAsHmd.
Result CheckPoseFacts(const EmitFacts& facts);

// H1: body com = API head position + d. Takes a position so Emit can pass a resampled head.
void ComFromHead(const float headPos[3], const EmitFacts& facts, float com[3]);

// H2: the basis behind hmdOri (full head basis, or the yaw-only body basis).
const float* HmdQuat(const SrcPlayer& player, const EmitFacts& facts);

// H3: the body record's orientation.
const float* BodyQuat(const SrcPlayer& player, const EmitFacts& facts);

// H4: hand orientation; direct is the only supported mapping (CheckPoseFacts).
const float* HandQuat(const Pose& hand, const EmitFacts& facts);

// H5: the file slot the disc body names as owner in `frame`, or kNoSlot. InHand = a player holding
// it. Possession (measured) = holding it or carrying the API possession flag: the API's own
// holding_*=="disc" is already possession, not a hand distance, and the flag keeps the owner through
// the throw as the native owner field does. Two claimants keep `previousOwner` if it is one of them,
// else the lowest slot.
uint16_t DiscOwnerSlot(const SourceMatch& match, const SrcFrame& frame, DiscOwnerMeaning meaning,
                       uint16_t previousOwner);

// Column `axis` (0 x, 1 y, 2 z) of the rotation matrix of the unit xyzw quaternion q.
void QuatAxis(const float q[4], int axis, float out[3]);

}  // namespace replay::import
