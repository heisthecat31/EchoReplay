#pragma once

// Engine-shaped wire message bodies, packed through the engine mirror (foundation/bitio). Shared by
// every producer that has no live engine to tap: the mkdemo fixture and the legacy-format converter
// (src/import). One encoder of engine bytes, so the two cannot drift apart.
//
// Every value must already sit on its field's quantisation grid: the bit writer refuses off-grid
// values, and a refusal returns 0 here.

#include <stddef.h>
#include <stdint.h>

#include "capture/streamcodec.h"

namespace replay::capture {

constexpr uint32_t kWireBodyBytes = 64;  // S_PLAYER_WIRE / S_BODY_WIRE message body
constexpr uint32_t kDuvBodyBytes = 46;   // one-row S_NETVAR DUV body

// hdr0 (8) + stamp (8) + the bit-packed SR15NetPlayerState. Returns kWireBodyBytes, or 0.
uint32_t BuildPlayerBody(uint8_t* out, size_t cap, const PlayerFrame& f, uint64_t hdr0, uint64_t stamp);

// hdr0 (8) + stamp (8) + hdr2 = 0x50 (8) + the bit-packed SR15NetPhysicsSimState. Returns
// kWireBodyBytes, or 0.
uint32_t BuildBodyBody(uint8_t* out, size_t cap, const BodyFrame& f, uint64_t hdr0, uint64_t stamp);

// A one-row DUV (owner, slot, pkt_seq, name, raw SValue). Returns kDuvBodyBytes, or 0 when cap is short.
uint32_t BuildDuvBody(uint8_t* out, size_t cap, uint64_t owner, uint16_t slot, uint32_t seq, uint64_t name,
                      uint64_t value);

}  // namespace replay::capture
