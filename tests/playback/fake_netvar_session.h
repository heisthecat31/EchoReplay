#pragma once

// Synthetic engine memory laid out the way the netvar route reads it (netvarsite.h): a CR15NetGame
// carrying the embedded CComponentHandle<CR15NetVarCS> and its level space, the CncaGame whose
// topology counter the handle snapshotted, the component with the CCompactPool the engine embeds at
// cs+0xC8, and one CReplicatedVariables store per record -- sorted 288-byte rows, key at +0, raw
// SValue at +0x118.
//
// The pool is laid out as sub_140CFD730 builds it and sub_140C80660 fills it
// (DIAG-NETVAR-TABLEGUARD S1.3): cs+0x100 is the BARE record-array pointer, the counts are u16s at
// +0xF8/+0xFA/+0xFC, and the members past the pool at +0x130/+0x138 hold what a live CR15NetVarCS
// holds there -- a pointer and a set bool, which a CTable-header reading of +0x100 takes for
// `count` and `flags` and refuses.
//
// The two generations start DIFFERENT, as they are on every tick of a real level after the first
// spawn: a walk that gates on them binds nothing here.

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "generated/facts_generated.h"
#include "playback/core/netvarsite.h"

namespace fake_netvar {

constexpr uint64_t kCaptureLevelId = 0x4152454E41303031ull;  // "ARENA001"
constexpr uint64_t kCachedGeneration = 0x77;
constexpr uint64_t kLiveGeneration = 0x4A31;

struct FakeStore {
    uint8_t header[0x48] = {};
    std::vector<uint8_t> rows;
    std::vector<uint64_t> keys;

    void Set(uint64_t key, uint64_t raw) {
        if (uint8_t* row = Row(key)) {
            memcpy(row + replay::playback::kNetVarRowValueOff, &raw, sizeof raw);
            return;
        }
        keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        std::vector<uint8_t> next(keys.size() * replay::playback::kNetVarRowStride, 0);
        for (size_t i = 0; i < keys.size(); ++i) {
            uint8_t* dst = next.data() + i * replay::playback::kNetVarRowStride;
            if (keys[i] == key) {
                memcpy(dst, &key, sizeof key);
                memcpy(dst + replay::playback::kNetVarRowValueOff, &raw, sizeof raw);
            } else {
                memcpy(dst, Row(keys[i]), replay::playback::kNetVarRowStride);
            }
        }
        rows.swap(next);
        Publish();
    }
    bool Get(uint64_t key, uint64_t& raw) {
        uint8_t* row = Row(key);
        if (!row) return false;
        memcpy(&raw, row + replay::playback::kNetVarRowValueOff, sizeof raw);
        return true;
    }

private:
    uint8_t* Row(uint64_t key) {
        const size_t n = rows.size() / replay::playback::kNetVarRowStride;
        for (size_t i = 0; i < n; ++i) {
            uint8_t* row = rows.data() + i * replay::playback::kNetVarRowStride;
            uint64_t k = 0;
            memcpy(&k, row, sizeof k);
            if (k == key) return row;
        }
        return nullptr;
    }
    void Publish() {
        const void* base = rows.data();
        const uint64_t count = keys.size();
        memcpy(header + replay::playback::kNetVarStoreRowsOff, &base, sizeof base);
        memcpy(header + replay::playback::kNetVarStoreCountOff, &count, sizeof count);
    }
};

struct FakeSession {
    static constexpr size_t kCapacity = 8;
    static constexpr size_t kRecords = 3;           // appended so far
    static constexpr uint16_t kGlobalSlot = 1;      // the netGame's global handle
    static constexpr uint16_t kGlobalRecord = 2;    // ...whose slot maps to this record
    static constexpr uint16_t kGlobalGeneration = 5;
    static constexpr uint16_t kOtherSlot = 0;
    static constexpr uint16_t kOtherRecord = 0;

    uint8_t  netGame[0x2E60] = {};
    uint8_t  game[0x2140] = {};
    uint8_t  rules[0x100] = {};
    uint8_t  cs[0x200] = {};
    uint64_t levelSpace[16] = {};
    uint64_t nextLevelSpace[16] = {};
    uint16_t slotMap[kCapacity][2] = {};            // {index, generation}
    uint8_t  records[kCapacity * 0x30] = {};
    uint8_t  identities[kCapacity * 0x10] = {};
    FakeStore globalStore;
    FakeStore otherStore;

    FakeSession() {
        using namespace replay::playback;
        const int32_t inMatch = 9;
        memcpy(netGame, &inMatch, sizeof inMatch);
        Put(netGame, facts::GS_RULES_OFF, static_cast<void*>(rules));
        SetCachedGeneration(kCachedGeneration);
        SetLiveGeneration(kLiveGeneration);
        levelSpace[0x58 / 8] = kCaptureLevelId;
        nextLevelSpace[0x58 / 8] = kCaptureLevelId;
        SetLevelSpace(levelSpace);
        FillHandle(levelSpace);

        Put(cs, facts::NETVAR_LEVEL_NETGAME_OFF, static_cast<void*>(netGame));
        for (auto& entry : slotMap) entry[0] = 0xFFFF;   // sub_140CFD730: every slot -> "no record"  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        slotMap[kGlobalSlot][0] = kGlobalRecord;
        slotMap[kGlobalSlot][1] = kGlobalGeneration;
        slotMap[kOtherSlot][0] = kOtherRecord;

        Put(cs, 0xD0, static_cast<void*>(slotMap));
        Put(cs, 0xF8, static_cast<uint16_t>(kCapacity));
        SetRecordCount(kRecords);
        Put(cs, 0xFC, uint16_t{1});   // the active prefix: NOT the record count
        Put(cs, 0x100, static_cast<void*>(records));
        Put(cs, 0x108, static_cast<void*>(identities));  // facts-ok: LOCAL.BROADCASTER_CS_KEYS_OFF a test fixture value this case compares against, not an engine position
        Put(cs, 0x110, static_cast<void*>(this));  // facts-ok: LOCAL.ATTACHED_CONSTRAINT_OFF a test fixture value this case compares against, not an engine position
        Put(cs, 0x120, uint64_t{kRecords});
        Put(cs, 0x128, uint64_t{kCapacity});
        Put(cs, 0x130, reinterpret_cast<void*>(0x000001C08F7A1230ull));  // facts-ok: PHYS.CI_TARGET_TIME_OFF a test fixture value this case compares against, not an engine position
        Put(cs, 0x138, uint32_t{1});  // facts-ok: ENTITY.REMOTEPLAYER_CS_NCAPHYSICS_OFF a test fixture value this case compares against, not an engine position

        Put(records, kGlobalRecord * 0x30 + 0x10, static_cast<void*>(globalStore.header));
        Put(records, kOtherRecord * 0x30 + 0x10, static_cast<void*>(otherStore.header));
    }

    void SetCachedGeneration(uint64_t g) {
        memcpy(netGame + replay::playback::kNetGameNetVarGenerationOff, &g, sizeof g);
    }
    void SetLiveGeneration(uint64_t g) {
        memcpy(game + replay::playback::kGameGenerationOff, &g, sizeof g);
    }
    void SetLevelSpace(void* space) { Put(netGame, replay::playback::kNetGameLevelSpaceOff, space); }
    void SetComponentSpace(void* space) { Put(cs, 0x80, space); }
    void SetHandle(uint32_t handle) {
        memcpy(netGame + replay::playback::kNetGameNetVarHandleOff, &handle, sizeof handle);
    }
    // What CR15NetVarCS::InitNetVarCI leaves on the netGame, for a component living in `space`.
    void FillHandle(void* space) {
        Put(netGame, replay::playback::kNetGameNetVarGameOff, static_cast<void*>(game));
        Put(netGame, replay::playback::kNetGameNetVarCsOff, static_cast<void*>(cs));
        SetHandle(kGlobalSlot | (uint32_t{kGlobalGeneration} << 16));   // as sub_140C80660 packs it
        SetComponentSpace(space);
    }
    // What CR15NetVarCS::RemoveReferences leaves: {null game, generation -1, slot 0xFFFF}.
    void ClearHandle() {
        Put(netGame, replay::playback::kNetGameNetVarGameOff, static_cast<void*>(nullptr));
        Put(netGame, replay::playback::kNetGameNetVarCsOff, static_cast<void*>(nullptr));
        SetCachedGeneration(~0ull);
        SetHandle(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    void SetState(int32_t s) { memcpy(netGame, &s, sizeof s); }
    void SetRecordCount(uint16_t n) { Put(cs, 0xFA, n); }
    void SetStoreOf(uint16_t record, void* store) { Put(records, record * 0x30 + 0x10, store); }
    float RulesClock() const {
        float v = 0;
        memcpy(&v, rules + 0xBC, sizeof v);
        return v;
    }
    void SetRulesClock(float v) { memcpy(rules + 0xBC, &v, sizeof v); }
    void* NetGame() { return netGame; }

    template <class T>
    static void Put(uint8_t* base, size_t off, T value) {
        memcpy(base + off, &value, sizeof value);
    }
};

}  // namespace fake_netvar
