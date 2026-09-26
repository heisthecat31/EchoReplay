#pragma once

// At most one pose owner, enforced ACROSS IMAGES.
//
// Two objects in this plugin can drive the spectator camera's pose and they are NOT compatible:
//
//   StockFreeCam  W2-E's camera_control over the shipping rig (S8.1.2). The engine's own
//                 integrator flies it on wall time, gated by `cmp dword ptr [reg+9Ch], 0`, so the
//                 view-state gate must stay ARMED for it to respond to input at all.
//   WritePoint    this packet's camera_writepoint (S8.1.7), the fallback pose sink. It needs
//                 viewstate+0x9C CLEARED and re-clears it every frame because the engine keeps
//                 re-arming it, and its first SetPose forces ci+0x3C to 6 (api).
//
// So publishing both does not produce two cameras fighting: the fallback's first write silently
// DISABLES the primary, and the symptom is a free camera that ignores input -- a bug that reads as
// a UI fault. The exclusion is a mechanism, not a comment. A second claimant is REFUSED -- named,
// counted, and reported on the status line -- and never overrides the holder.
//
// ★ THE TOKEN IS KEYED ON (IMAGE, OWNER), NOT ON THE OWNER (AUDIT_I finding 1). Keying either half
// on the owner alone leaves two holes that both read clean: a SECOND IMAGE claiming the SAME owner
// is taken for a re-claim, so both bind with `conflicts 0`; and any image may release a token it
// never took, because "the owner matches" is true in every image at once. Both are one deployment
// change from live -- camera module's Init and Shutdown call camera_control::Release()
// unconditionally, and probe_camera.dll carries its own copy of the write point's claim site. So a
// re-claim succeeds only for the image that holds it, and a release by anyone else is a counted,
// named refusal rather than a silent no-op.
//
// ★ WHY THIS IS NOT A STATIC. The first version kept the state in
// an inline function's local static, which is exactly one instance PER LINKED IMAGE -- and the two
// claimants are not in one image. camera_control is compiled into EchoReplay.dll and
// EchoReplay.dll; camera::Create's only caller is probe_camera.dll, its own SHARED library. At L3
// and L5 each would claim a DIFFERENT arbiter, both would bind, and both would print `conflicts 0`
// while the fallback's first write disabled the primary. The state therefore lives in a pagefile-
// backed section named per PROCESS, which every image maps: one PoseOwnerState per process, one
// conflicts counter, one holder. The claim, the keyed release and the counter keep the semantics
// they already had; this header is a wrapper over the mapped view and is no longer storage.
//
//   Local\ and never Global\   -- session namespace, nothing reachable outside the session.
//   ...PoseOwner.<pid>         -- the pid is what makes it process-wide rather than session-wide:
//                                 two game processes must not arbitrate each other.
//   INVALID_HANDLE_VALUE       -- pagefile-backed, no file on disk.
//
// If the section cannot be established the claim is REFUSED and says why. It does NOT fall back to
// a per-image static: a fallback that silently reinstates the defect is worse than none, so the
// unavailable case is a named, visible refusal (PoseOwner::Unavailable) rather than a clean-reading
// zero.
//
// It lives on this path because the design grants `src/engine/camera_writepoint*` to W1-H; W2-E
// cites it rather than keeping a second copy of the rule (S9.10). Header-only still, because the
// only build file that could carry a .cpp for it is src/engine/CMakeLists.txt, which is W0-1's.

#include <stdint.h>
#include <stdio.h>
#include <windows.h>

namespace replay {
namespace camera {

enum class PoseOwner : uint8_t {
    None = 0,
    StockFreeCam,  // W2-E camera_control: the engine's integrator, gate ARMED
    WritePoint,    // W1-H camera_writepoint: our pose sink, gate CLEARED
    Unavailable,   // the arbiter itself could not be established -- nobody may claim
};

inline const char* PoseOwnerName(PoseOwner owner) {
    switch (owner) {
        case PoseOwner::None:         return "none";
        case PoseOwner::StockFreeCam: return "StockFreeCam";
        case PoseOwner::WritePoint:   return "WritePoint";
        case PoseOwner::Unavailable:  return "unavailable";
    }
    return "unknown";
}

namespace detail {

constexpr LONG kPoseOwnerInitialising = 1;
constexpr LONG kPoseOwnerReady        = 0x504F5732;  // 'POW2'

// A loser of the creation race waits this long for the winner to publish kReady, then refuses.
// Unbounded is not an option: an image that hangs here hangs the load it was called from.
constexpr ULONGLONG kPoseOwnerReadyTimeoutMs = 2000;

// holder packs the claimant key and the owner into ONE word so a single CAS settles both. A live
// claim always carries a key >= 1, so holder == 0 is exactly "unheld".
struct PoseOwnerState {
    volatile LONG   magic;
    volatile LONG64 holder;
    volatile LONG64 conflicts;
    volatile LONG64 relRefused; // releases refused because the caller was not the holding image
    volatile LONG64 transfers;  // completed TransferPoseOwner calls, process-wide
    volatile LONG64 transferRefused;  // transfers refused because `from` was not the holder
    volatile LONG   epoch;      // successful None->owner transitions; the same count in every image
    volatile LONG   nextKey;    // hands each mapping image a key unique by construction
    volatile LONG   diagLatch;
    uint32_t        pid;        // published under magic; a section from another pid is refused
    char            lastConflict[96];
};

inline LONG64 PackHolder(uint32_t key, PoseOwner owner) {
    return static_cast<LONG64>((static_cast<uint64_t>(key) << 32) | static_cast<uint32_t>(owner));
}

inline PoseOwner HolderOwner(LONG64 holder) {
    return static_cast<PoseOwner>(static_cast<uint8_t>(static_cast<uint32_t>(holder)));
}

inline uint32_t HolderKey(LONG64 holder) {
    return static_cast<uint32_t>(static_cast<uint64_t>(holder) >> 32);
}

typedef HANDLE(WINAPI* CreateSectionFn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);

inline HANDLE WINAPI RefuseSectionForTest(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD,
                                          LPCWSTR) {
    SetLastError(ERROR_ACCESS_DENIED);
    return nullptr;
}

// The test seam for the unavailable arm (AUDIT_I finding 2). "There is NO fallback to a per-image
// static" is the load-bearing half of this header, and until a test could reach the arm, a mutant
// that reinstated the static lived in a branch nothing entered. Defaults to the real API and is
// reassigned only by ForcePoseOwnerUnavailableForTest (grep: nothing outside tests/ calls it).
inline CreateSectionFn& PoseOwnerCreateSection() {
    static CreateSectionFn create = &CreateFileMappingW;
    return create;
}

// One per IMAGE: the section is shared, the handle, the view pointer and the key are not. Its
// destructor runs on DLL detach, which is what stops an unloaded or crashed probe from wedging the
// token it was holding.
struct PoseOwnerMapping {
    PoseOwnerState* view    = nullptr;
    HANDLE          section = nullptr;
    uint32_t        key     = 0;
    bool            created = false;
    char            fault[128] = {};
    char            arbiter[32] = {};

    PoseOwnerMapping() { Establish(); }

    ~PoseOwnerMapping() { Teardown(); }

    // Drops this image's mapping, handing back a token it still holds first. An image that goes
    // away must not wedge the pose for the rest of the process.
    void Teardown() {
        if (view) {
            const LONG64 held = view->holder;
            if (held != 0 && HolderKey(held) == key) {
                InterlockedCompareExchange64(&view->holder, 0, held);
            }
            UnmapViewOfFile(view);
            view = nullptr;
        }
        if (section) { CloseHandle(section); section = nullptr; }
    }

    // Test-only (ForcePoseOwnerUnavailableForTest): drops the mapping and takes the section call
    // again, so a test can move this image between established and unavailable.
    void Reestablish() {
        Teardown();
        key = 0;
        created = false;
        fault[0] = '\0';
        arbiter[0] = '\0';
        Establish();
    }

    void Fail(const char* what, DWORD gle) {
        _snprintf_s(fault, sizeof fault, _TRUNCATE, "pose-owner-unavailable: %s (gle %lu)", what,
                    static_cast<unsigned long>(gle));
        _snprintf_s(arbiter, sizeof arbiter, _TRUNCATE, "unavailable");
        if (view) { UnmapViewOfFile(view); view = nullptr; }
        if (section) { CloseHandle(section); section = nullptr; }
    }

    void Establish() {
        const DWORD pid = GetCurrentProcessId();
        wchar_t name[64];
        _snwprintf_s(name, _TRUNCATE, L"Local\\EchoReplay.PoseOwner.%lu",
                     static_cast<unsigned long>(pid));

        section = PoseOwnerCreateSection()(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                           sizeof(PoseOwnerState), name);
        if (!section) { Fail("CreateFileMappingW", GetLastError()); return; }
        const bool won = GetLastError() != ERROR_ALREADY_EXISTS;

        view = static_cast<PoseOwnerState*>(
            MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PoseOwnerState)));
        if (!view) { Fail("MapViewOfFile", GetLastError()); return; }

        // CreateFileMappingW is atomic and a new section is zero-filled by the OS, so exactly one
        // caller CASes magic away from 0 and initialises. Everyone else waits for kReady rather
        // than reading a view whose fields are not published yet.
        if (InterlockedCompareExchange(&view->magic, kPoseOwnerInitialising, 0) == 0) {
            view->pid = pid;
            InterlockedExchange(&view->magic, kPoseOwnerReady);
        } else {
            const ULONGLONG deadline = GetTickCount64() + kPoseOwnerReadyTimeoutMs;
            while (view->magic != kPoseOwnerReady) {
                if (GetTickCount64() > deadline) { Fail("section init timed out", 0); return; }
                Sleep(1);
            }
            MemoryBarrier();
        }

        // The name carries the pid, so this only fires on a section that outlived a recycled pid --
        // and arbitrating for a dead process is worse than refusing.
        if (view->pid != pid) { Fail("section pid mismatch", view->pid); return; }

        created = won;
        key = static_cast<uint32_t>(InterlockedIncrement(&view->nextKey));
        _snprintf_s(arbiter, sizeof arbiter, _TRUNCATE, "%s#%lu", won ? "created" : "opened",
                    static_cast<unsigned long>(key));
    }
};

inline PoseOwnerMapping& PoseOwnerImage() {
    static PoseOwnerMapping mapping;
    return mapping;
}

// Diagnostics only. A writer that finds the latch taken skips rather than blocks: a refusal line
// is worth having but never worth a lock on a claim path.
//
// A conflict naming the SAME owner on both sides is a two-image conflict by construction: a
// re-claim from the image that already holds the token never reaches here.
inline void RecordConflict(PoseOwnerState& state, PoseOwner holder, PoseOwner claimant) {
    if (InterlockedCompareExchange(&state.diagLatch, 1, 0) != 0) return;
    _snprintf_s(state.lastConflict, sizeof state.lastConflict, _TRUNCATE,
                "pose-owner-conflict: %s vs %s", PoseOwnerName(holder), PoseOwnerName(claimant));
    InterlockedExchange(&state.diagLatch, 0);
}

inline void RecordTransferRefused(PoseOwnerState& state, LONG64 held, PoseOwner from,
                                  PoseOwner to) {
    if (InterlockedCompareExchange(&state.diagLatch, 1, 0) != 0) return;
    _snprintf_s(state.lastConflict, sizeof state.lastConflict, _TRUNCATE,
                "pose-owner-transfer-refused: %s->%s but %s holds it (image %lu)",
                PoseOwnerName(from), PoseOwnerName(to), PoseOwnerName(HolderOwner(held)),
                static_cast<unsigned long>(HolderKey(held)));
    InterlockedExchange(&state.diagLatch, 0);
}

inline void RecordReleaseRefused(PoseOwnerState& state, LONG64 held, PoseOwner claimant) {
    if (InterlockedCompareExchange(&state.diagLatch, 1, 0) != 0) return;
    _snprintf_s(state.lastConflict, sizeof state.lastConflict, _TRUNCATE,
                "pose-owner-release-refused: %s is not the holder (%s, image %lu)",
                PoseOwnerName(claimant), PoseOwnerName(HolderOwner(held)),
                static_cast<unsigned long>(HolderKey(held)));
    InterlockedExchange(&state.diagLatch, 0);
}

}  // namespace detail

// True when the claimant now owns the pose. Re-claiming succeeds only for the image that already
// holds it -- `prior == want` compares the packed key AND owner in one read, so a second image
// claiming the same owner is a conflict rather than a re-claim, and the image that actually took
// the pose stays the one whose unload frees it. Both modules re-bind on every identity change, so
// the same-image re-claim is ordinary traffic and is not counted.
inline bool ClaimPoseOwner(PoseOwner claimant) {
    if (claimant != PoseOwner::StockFreeCam && claimant != PoseOwner::WritePoint) return false;
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    if (!image.view) return false;

    const LONG64 want = detail::PackHolder(image.key, claimant);
    const LONG64 prior = InterlockedCompareExchange64(&image.view->holder, want, 0);
    if (prior == 0) {
        InterlockedIncrement(&image.view->epoch);
        return true;
    }
    if (prior == want) return true;

    InterlockedIncrement64(&image.view->conflicts);
    detail::RecordConflict(*image.view, detail::HolderOwner(prior), claimant);
    return false;
}

// Keyed on the holding IMAGE as well as the owner. Releasing an unheld token is a no-op -- camera
// module's Init calls Release() before anything has bound -- but releasing a token another image
// holds is a REFUSAL: named, counted, and never a silent success.
inline void ReleasePoseOwner(PoseOwner claimant) {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    if (!image.view) return;
    const LONG64 held = image.view->holder;
    if (held == 0) return;
    if (held != detail::PackHolder(image.key, claimant)) {
        InterlockedIncrement64(&image.view->relRefused);
        detail::RecordReleaseRefused(*image.view, held, claimant);
        return;
    }
    InterlockedCompareExchange64(&image.view->holder, 0, held);
}

// Move the token from `from` to `to` atomically. Neither a release-then-claim (which opens a
// window in which a third claimant wins and the caller silently loses the camera) nor a second
// arbiter. Returns false and leaves the holder UNCHANGED if `from` is not the current holder --
// which is the honest answer when the engine's own state machine or another image took it.
//
// ARCH-17 S3.3(b): camera_control::CommandPose transfers StockFreeCam->WritePoint on entry to
// regime COMMANDED and WritePoint->StockFreeCam on ReleasePose(). The rig bind is NOT dropped --
// Release() + Create() would re-run the rig probe and the engine's table-sorting lookup on the
// tick thread (ARCH-10 S7) for what is a camera nudge.
//
// ONE interlocked compare-exchange settles it, because the holder word packs the image key and
// the owner together: the holder is never observably unheld part-way through, which is the whole
// difference from release-then-claim.
//
// Keyed on the holding IMAGE for the same reason ReleasePoseOwner is: `from` must be held by the
// image asking, or any image could take a token it never claimed. In the shipping arrangement both
// claimants are compiled into EchoReplay.dll, so the key matches by construction; a transfer
// against a token probe_camera.dll holds is refused and names the holder.
//
// A transfer is not a None->owner transition, so it does NOT move `epoch`: two images comparing
// epochs are asking whether they share one arbiter, and a rename of the holder is not a new bind.
// `from == to` while this image holds it is a success that changed nothing.
inline bool TransferPoseOwner(PoseOwner from, PoseOwner to) {
    if (from != PoseOwner::StockFreeCam && from != PoseOwner::WritePoint) return false;
    if (to != PoseOwner::StockFreeCam && to != PoseOwner::WritePoint) return false;
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    if (!image.view) return false;

    const LONG64 expect = detail::PackHolder(image.key, from);
    const LONG64 want   = detail::PackHolder(image.key, to);
    const LONG64 prior  = InterlockedCompareExchange64(&image.view->holder, want, expect);
    if (prior == expect) {
        InterlockedIncrement64(&image.view->transfers);
        return true;
    }

    InterlockedIncrement64(&image.view->transferRefused);
    detail::RecordTransferRefused(*image.view, prior, from, to);
    return false;
}

// Unavailable is a real answer, not a missing one: it must never read as "none" on a status line
// that a launch is scored from.
inline PoseOwner PoseOwnerHeld() {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    if (!image.view) return PoseOwner::Unavailable;
    return detail::HolderOwner(image.view->holder);
}

inline uint64_t PoseOwnerConflicts() {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    return image.view ? static_cast<uint64_t>(image.view->conflicts) : 0;
}

// Releases refused because the caller was not the holding image. Its own counter, not conflicts':
// a refused CLAIM and a refused RELEASE are different events and a status line that merged them
// could not say which one a launch saw.
inline uint64_t PoseOwnerReleaseRefusals() {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    return image.view ? static_cast<uint64_t>(image.view->relRefused) : 0;
}

// Completed transfers, and transfers refused because `from` was not the holder. Their own
// counters, not conflicts': a refused CLAIM, a refused RELEASE and a refused TRANSFER are three
// events, and an operator does something different with each.
inline uint64_t PoseOwnerTransfers() {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    return image.view ? static_cast<uint64_t>(image.view->transfers) : 0;
}

inline uint64_t PoseOwnerTransferRefusals() {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    return image.view ? static_cast<uint64_t>(image.view->transferRefused) : 0;
}

// Successful None->owner transitions, shared by every image. Two images reading the same non-zero
// epoch is the cheapest positive proof that they are looking at one arbiter and not at two.
inline uint32_t PoseOwnerEpoch() {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    return image.view ? static_cast<uint32_t>(image.view->epoch) : 0;
}

// "created#1" / "opened#2" / "unavailable". Exactly one image in a process creates the section and
// every other opens it, so this is the field that separates one shared arbiter from several
// private ones on a launch artifact -- which `conflicts 0` on its own cannot do.
inline const char* PoseOwnerArbiter() { return detail::PoseOwnerImage().arbiter; }

inline uint32_t PoseOwnerImageKey() { return detail::PoseOwnerImage().key; }

// "pose-owner-conflict: <holder> vs <claimant>", "pose-owner-unavailable: <why>" when the arbiter
// could not be established, or "none" if no claim has ever been refused.
inline const char* LastPoseOwnerConflict() {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    if (!image.view) return image.fault;
    return image.view->lastConflict[0] ? image.view->lastConflict : "none";
}

// Clears the arbiter's own state so one process can run the ordering cases. It bypasses no claim
// path -- every claim in a test still goes through ClaimPoseOwner -- and nothing outside tests/
// calls it (grep ResetPoseOwnerForTest). The section's identity survives: magic, pid and the keys
// already handed out stay valid, so an image that has claimed can claim again after a reset.
inline void ResetPoseOwnerForTest() {
    detail::PoseOwnerMapping& image = detail::PoseOwnerImage();
    if (!image.view) return;
    InterlockedExchange64(&image.view->holder, 0);
    InterlockedExchange64(&image.view->conflicts, 0);
    InterlockedExchange64(&image.view->relRefused, 0);
    InterlockedExchange64(&image.view->transfers, 0);
    InterlockedExchange64(&image.view->transferRefused, 0);
    InterlockedExchange(&image.view->epoch, 0);
    image.view->lastConflict[0] = '\0';
}

// Test-only. Makes THIS image's section call fail (and, off again, succeed), so the unavailable arm
// -- claims refused, no per-image fallback, the fault text on the status line -- is reachable by a
// test instead of being a branch nothing enters (AUDIT_I finding 2). Re-establishing takes a fresh
// image key; if this image was the last one mapped, the section itself goes with it and comes back
// zeroed, so a caller must reset rather than carry counters across.
inline void ForcePoseOwnerUnavailableForTest(bool forced) {
    detail::PoseOwnerCreateSection() = forced ? &detail::RefuseSectionForTest : &CreateFileMappingW;
    detail::PoseOwnerImage().Reestablish();
}

}  // namespace camera
}  // namespace replay
