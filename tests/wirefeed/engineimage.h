#pragma once

// Maps echovr.exe into this process as an image, without starting it.
//
// Sections are copied, base relocations applied, and imports bound only from an explicit
// allowlist -- every other IAT slot is pointed at a thunk that names the import and kills the
// process. The entry point is never called and the TLS callbacks are never run, so no engine
// initialiser executes; the only engine code that runs is what the caller calls by address.
//
// W1-L uses it to exercise NRadEngine::CBroadcaster::Listen and CMatSym::Hash offline. The
// allowlist is what makes that safe to say out loud: WS2_32, WININET and every other network
// import in the file are poisoned, so no code path reached through this image can open a socket.

#include <cstdint>
#include <string>
#include <vector>

namespace w1l {

class EngineImage {
public:
    ~EngineImage();

    EngineImage() = default;
    EngineImage(const EngineImage&) = delete;
    EngineImage& operator=(const EngineImage&) = delete;

    // Returns false and fills `error` if anything about the file or the mapping is not what
    // W1-L derived against. Nothing is left mapped on failure.
    bool Map(const wchar_t* exePath, std::string& error);

    uintptr_t Base() const { return base_; }
    uint64_t  PreferredBase() const { return preferredBase_; }
    intptr_t  Slide() const { return static_cast<intptr_t>(base_ - preferredBase_); }

    // A preferred-base VA out of the packet's facts, translated to where the image actually is.
    template <typename T>
    T At(uint64_t preferredVa) const {
        return reinterpret_cast<T>(base_ + static_cast<uintptr_t>(preferredVa - preferredBase_));
    }

    // Imports bound from the allowlist, and slots pointed at the poison thunk instead.
    uint32_t BoundImports() const { return bound_; }
    uint32_t PoisonedImports() const { return poisoned_; }

private:
    uintptr_t base_ = 0;
    uint64_t  preferredBase_ = 0;
    size_t    size_ = 0;
    void*     thunks_ = nullptr;
    uint32_t  bound_ = 0;
    uint32_t  poisoned_ = 0;
    std::vector<std::string> thunkNames_;
};

}  // namespace w1l
