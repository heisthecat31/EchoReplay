#pragma once
// DESIGN-HOST-SYNC P0 seam, faked at the IEntity surface: one script graph with a real-typed
// variable, a data-word block and an event log. The engine-memory half of the seam (the walk, the
// module+240/248/512 bindings) is tested against a byte-level rig in tests/entity.

#include <stdint.h>

#include <vector>

#include "fake_disc_entity.h"

struct FakeScriptGraphEntity : disctest::DiscEntity {
    bool present = true;            // FindScriptGraph finds the graph
    void* presentIn = nullptr;      // ...only when walking this space (null: in any space)
    std::vector<void*> chain;       // what DiscCandidateSpaces hands out
    std::vector<void*> walked;      // every space FindScriptGraph was asked about
    bool valid = true;              // ScriptGraphValid answers
    bool setAnswers = true;         // the setter returns 1
    bool setStores = true;          // ...and actually stores (false models a wrong binding)
    float variable = 0.0f;
    uint64_t lastSetName = 0, lastSetId = 0;
    uint32_t findCalls = 0, setCalls = 0, getCalls = 0;
    int32_t words[32] = {};
    struct Sent { uint64_t name; uint64_t component; };
    std::vector<Sent> sent;
    uint8_t instToken = 0;
    // A rebuilt level: the next walk finds the graph at another address, and the old instance still
    // answers ScriptGraphValid, as a freed one whose bytes were not scribbled can.
    bool rebuilt = false;
    uint8_t rebuiltToken = 0;

    uint32_t DiscCandidateSpaces(void*, void** out, uint32_t cap) const override {
        uint32_t n = 0;
        for (void* s : chain) {
            if (n < cap) out[n++] = s;
        }
        return n;
    }
    bool FindScriptGraph(void* space, uint64_t hash, uint64_t, uint64_t, ScriptGraphRef& out,
                         const char** why) override {
        ++findCalls;
        walked.push_back(space);
        if (!present || (presentIn != nullptr && space != presentIn)) {
            if (why) *why = "no ncaScript graph with this module hash on the walked space";
            return false;
        }
        out = ScriptGraphRef();
        uint8_t* const at = rebuilt ? &rebuiltToken : &instToken;
        out.inst = at;
        out.graph = at;
        out.module = at;
        out.hash = hash;
        out.selfValid = true;
        valid = true;
        return true;
    }
    bool ScriptGraphValid(const ScriptGraphRef& r) override {
        return valid && (r.inst == &instToken || r.inst == &rebuiltToken);
    }
    bool ScriptGetReal(const ScriptGraphRef& r, uint64_t, float& out) override {
        ++getCalls;
        if (!ScriptGraphValid(r)) return false;
        out = variable;
        return true;
    }
    bool ScriptSetReal(const ScriptGraphRef& r, uint64_t name, uint64_t id, float v) override {
        ++setCalls;
        if (!ScriptGraphValid(r)) return false;
        lastSetName = name;
        lastSetId = id;
        if (setStores) variable = v;
        return setAnswers;
    }
    bool ScriptDataWord(const ScriptGraphRef& r, uint32_t word, int32_t& out) override {
        if (!ScriptGraphValid(r) || word >= 32) return false;
        out = words[word];
        return true;
    }
    bool ScriptSendEvent(const ScriptGraphRef& r, uint64_t name, uint64_t component) override {
        if (!ScriptGraphValid(r) || !r.selfValid) return false;
        sent.push_back({name, component});
        return true;
    }
};
