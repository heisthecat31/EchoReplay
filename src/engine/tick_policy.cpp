#include "engine/tick_policy.h"

#include <string.h>

#include "bootstrap/config.h"
#include "generated/facts_generated.h"

namespace replay {
namespace tick {

SlotChoice ChooseFreeSlot(bool slotAOccupied, bool slotCOccupied) {
    if (!slotAOccupied && slotCOccupied) {
        return SlotChoice::ClaimA;
    }
    if (slotAOccupied && !slotCOccupied) {
        return SlotChoice::ClaimC;
    }
    return slotAOccupied ? SlotChoice::AbortBothTaken : SlotChoice::AbortBothFree;
}

const char* SlotChoiceName(SlotChoice choice) {
    switch (choice) {
        case SlotChoice::ClaimA:         return "claim-A";
        case SlotChoice::ClaimC:         return "claim-C";
        case SlotChoice::AbortBothFree:  return "abort:both-slots-free";
        case SlotChoice::AbortBothTaken: return "abort:both-slots-taken";
    }
    return "?";
}

uint32_t SlotChoiceOffset(SlotChoice choice) {
    switch (choice) {
        case SlotChoice::ClaimA: return facts::GAME_PROVIDER_SLOT_A;
        case SlotChoice::ClaimC: return facts::GAME_PROVIDER_SLOT_C;
        default:                 return 0;
    }
}

bool CommandLineHasFlag(const char* commandLine, const char* flag) {
    if (commandLine == nullptr || flag == nullptr || *flag == '\0') {
        return false;
    }
    const size_t flagLen = strlen(flag);
    for (const char* p = commandLine; *p != '\0'; ++p) {
        if (_strnicmp(p, flag, flagLen) != 0) {
            continue;
        }
        const char before = p == commandLine ? ' ' : p[-1];
        const char after = p[flagLen];
        const bool startOk = before == ' ' || before == '\t' || before == '"';
        const bool endOk = after == '\0' || after == ' ' || after == '\t' || after == '"';
        if (startOk && endOk) {
            return true;
        }
    }
    return false;
}

void AuditFrame(FrameAudit* audit, uint64_t frameCounter, uint64_t simTimeUs) {
    if (audit == nullptr) {
        return;
    }
    if (audit->started) {
        // Signed-safe: sim time only ever grows, so a wrap here would mean the engine reset the clock
        // and a frame delta computed across that point would be meaningless. Report it, do not hide it.
        if (simTimeUs >= audit->lastSimTimeUs) {
            audit->frameDeltaUs = simTimeUs - audit->lastSimTimeUs;
        } else {
            audit->frameDeltaUs = 0;
            ++audit->simRegressions;
        }
    } else {
        audit->frameDeltaUs = 0;
    }
    audit->lastSimTimeUs = simTimeUs;
    if (audit->started) {
        // Unsigned wrap is the right arithmetic here and not an accident: the engine's counter is a
        // u64 that only ever increments, so the difference is the number of frames since our last tick
        // however the value is represented.
        const uint64_t advance = frameCounter - audit->last;
        if (advance == 0) {
            ++audit->duplicates;
        } else if (advance > 1) {
            audit->skips += advance - 1;
        }
    } else {
        audit->first = frameCounter;
        audit->started = true;
    }
    audit->last = frameCounter;
    ++audit->ticks;
}

const char* EffectiveModeName(EffectiveMode mode) {
    switch (mode) {
        case EffectiveMode::Slot: return "slot";
        case EffectiveMode::Hook: return "hook";
        case EffectiveMode::None: return "none";
    }
    return "none";
}

ModePlan PlanMode() {
    ModePlan plan{};
    plan.requested = config::RequestedString("tick.mode");
    if (plan.requested == nullptr) {
        plan.requested = "auto";
    }
    const bool rowEnabled = config::Interface()->GetBool("tick.slot_claim", true);
    plan.slotAllowed = rowEnabled && strcmp(plan.requested, "hook") != 0;
    plan.hookAllowed = strcmp(plan.requested, "slot") != 0;
    return plan;
}

namespace {

EffectiveMode DecideMode(const ModePlan& plan, const ClaimOutcome& outcome) {
    if (outcome.slotClaimed) {
        return EffectiveMode::Slot;
    }
    if (outcome.stopping || !plan.hookAllowed || !outcome.gameResolved) {
        return EffectiveMode::None;
    }
    return EffectiveMode::Hook;
}

}  // namespace

EffectiveMode ResolveAndPublishMode(const ModePlan& plan, const ClaimOutcome& outcome) {
    const EffectiveMode mode = DecideMode(plan, outcome);
    if (mode != EffectiveMode::None) {
        config::SetEffectiveString("tick.mode", EffectiveModeName(mode));
    }
    return mode;
}


const void* ImageCellCache::Get(Resolver resolve, void* ctx) {
    const void* cell = cell_.load(std::memory_order_acquire);
    if (cell != nullptr) {
        return cell;
    }
    if (resolve == nullptr) {
        return nullptr;
    }
    cell = resolve(ctx);
    if (cell != nullptr) {
        // Two threads racing here resolve the same fixed address, so the loser stores the value the
        // winner already stored.
        cell_.store(cell, std::memory_order_release);
    }
    return cell;
}

bool ShouldReaskClock(uint64_t nullDispatches, uint64_t elapsedMs) {
    if (elapsedMs < kClockReaskWarmMs) {
        return true;
    }
    return (nullDispatches % kClockReaskEveryNth) == 0;
}

bool ClockBindHorizonPassed(uint64_t elapsedMs) { return elapsedMs >= kClockBindHorizonMs; }

}  // namespace tick
}  // namespace replay
