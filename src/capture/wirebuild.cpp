#include "capture/wirebuild.h"

#include <string.h>

#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"

namespace replay::capture {

uint32_t BuildPlayerBody(uint8_t* out, size_t cap, const PlayerFrame& f, uint64_t hdr0, uint64_t stamp) {
    if (cap < kWireBodyBytes) return 0;
    memset(out, 0, kWireBodyBytes);
    memcpy(out, &hdr0, 8);
    memcpy(out + 8, &stamp, 8);
    const bitio::WireField* W = bitio::kPlayerStateWire;
    bitio::EngineBitWriter w(out + 16, kWireBodyBytes - 16);
    w.BitPackQuat(f.hmdOri, W[0].bits);
    w.BitPackQuat(f.lHandOri, W[1].bits);
    w.BitPackQuat(f.rHandOri, W[2].bits);
    w.BitPackVec(f.refVel, W[3].min, W[3].max, W[3].step);
    w.Write(f.bits28, W[4].bits);
    w.Write(f.holdLSlot, W[5].bits);
    w.Write(f.holdLGen, W[6].bits);
    w.Write(f.holdRSlot, W[7].bits);
    w.Write(f.holdRGen, W[8].bits);
    w.BitPackReal(f.bendBack, W[9].min, W[9].max, W[9].step);
    w.BitPackReal(f.bendFwd, W[10].min, W[10].max, W[10].step);
    w.BitPackReal(f.restPoseHeight, W[11].min, W[11].max, W[11].step);
    w.BitPackVec(f.lHandOff, W[12].min, W[12].max, W[12].step);
    w.BitPackVec(f.rHandOff, W[13].min, W[13].max, W[13].step);
    w.BitPackReal(f.trigL, W[14].min, W[14].max, W[14].step);
    w.BitPackReal(f.trigR, W[15].min, W[15].max, W[15].step);
    w.Write(f.anchor, W[16].bits);
    w.BitPackVec(f.refVelDup, W[17].min, W[17].max, W[17].step);
    w.Finish();
    return w.Status() == bitio::WireStatus::Ok ? kWireBodyBytes : 0u;
}

uint32_t BuildBodyBody(uint8_t* out, size_t cap, const BodyFrame& f, uint64_t hdr0, uint64_t stamp) {
    if (cap < kWireBodyBytes) return 0;
    memset(out, 0, kWireBodyBytes);
    memcpy(out, &hdr0, 8);
    memcpy(out + 8, &stamp, 8);
    const uint64_t hdr2 = 0x50;
    memcpy(out + 16, &hdr2, 8);
    const bitio::WireField* W = bitio::kSimStateWire;
    bitio::EngineBitWriter w(out + 24, kWireBodyBytes - 24);
    w.Write(f.bits11, W[0].bits);
    w.Write(f.idValue, W[1].bits);
    w.BitPackVec(f.com, W[2].min, W[2].max, W[2].step);
    w.BitPackQuat(f.ori, W[3].bits);
    w.BitPackVec(f.vel, W[4].min, W[4].max, W[4].step);
    w.BitPackVec(f.angVel, W[5].min, W[5].max, W[5].step);
    w.BitPackVec(f.lastHeldCom, W[6].min, W[6].max, W[6].step);
    w.Finish();
    return w.Status() == bitio::WireStatus::Ok ? kWireBodyBytes : 0u;
}

uint32_t BuildDuvBody(uint8_t* out, size_t cap, uint64_t owner, uint16_t slot, uint32_t seq, uint64_t name,
                      uint64_t value) {
    if (cap < kDuvBodyBytes) return 0;
    memset(out, 0, kDuvBodyBytes);
    memcpy(out, &owner, 8);
    memcpy(out + 8, &slot, 2);
    const uint32_t one = 1;
    memcpy(out + 12, &one, 4);
    memcpy(out + 16, &seq, 4);
    memcpy(out + 24, &seq, 4);
    const uint16_t rows = 1;
    memcpy(out + 28, &rows, 2);
    memcpy(out + 30, &name, 8);
    memcpy(out + 38, &value, 8);
    return kDuvBodyBytes;
}

}  // namespace replay::capture
