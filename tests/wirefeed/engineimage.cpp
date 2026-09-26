#include "engineimage.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace w1l {
namespace {

// Everything CBroadcaster::Listen and CMatSym::Hash reach. Listen's prologue and epilogue swap
// the engine's per-thread allocator context through TLS; the rest of the path is arithmetic on
// the containers plus the allocator vtable the caller supplies, so nothing else is needed. A
// path that wants more names the import it wanted rather than silently taking another route.
struct Allowed {
    const char* dll;
    const char* name;
};
constexpr Allowed kAllowlist[] = {
    {"KERNEL32.dll", "TlsGetValue"},
    {"KERNEL32.dll", "TlsSetValue"},
};

bool IsAllowed(const char* dll, const char* name) {
    for (const Allowed& a : kAllowlist) {
        if (_stricmp(a.dll, dll) == 0 && strcmp(a.name, name) == 0) {
            return true;
        }
    }
    return false;
}

// Reached only if mapped engine code calls an import W1-L did not expect it to need. Killing the
// process is the point: a silent fallback would let the offline host report a verdict about a
// code path that took a route nobody has looked at.
extern "C" void PoisonedImport(const char* what) {
    std::fprintf(stderr, "ENGINE-IMAGE-POISON %s\n", what);
    std::fflush(stderr);
    ExitProcess(90);
}

constexpr size_t kThunkSize = 22;

void EmitThunk(uint8_t* at, const char* name) {
    const void* handler = reinterpret_cast<const void*>(&PoisonedImport);
    at[0] = 0x48;  // mov rcx, <name>
    at[1] = 0xB9;
    std::memcpy(at + 2, &name, 8);
    at[10] = 0x48;  // mov rax, <PoisonedImport>
    at[11] = 0xB8;
    std::memcpy(at + 12, &handler, 8);
    at[20] = 0xFF;  // jmp rax
    at[21] = 0xE0;
}

}  // namespace

EngineImage::~EngineImage() {
    if (thunks_ != nullptr) {
        VirtualFree(thunks_, 0, MEM_RELEASE);
    }
    if (base_ != 0) {
        VirtualFree(reinterpret_cast<void*>(base_), 0, MEM_RELEASE);
    }
}

bool EngineImage::Map(const wchar_t* exePath, std::string& error) {
    const HANDLE file = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open the engine image";
        return false;
    }
    LARGE_INTEGER fileSize{};
    GetFileSizeEx(file, &fileSize);
    std::vector<uint8_t> raw(static_cast<size_t>(fileSize.QuadPart));
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr) != 0 &&
        read == raw.size();
    CloseHandle(file);
    if (!readOk) {
        error = "short read on the engine image";
        return false;
    }
    if (raw.size() < sizeof(IMAGE_DOS_HEADER)) {
        error = "engine image is too small to be a PE";
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw.size()) {
        error = "engine image has no PE header";
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        error = "engine image is not PE32+/x64";
        return false;
    }
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size == 0) {
        error = "engine image carries no base relocations, so it cannot be mapped elsewhere";
        return false;
    }

    preferredBase_ = nt->OptionalHeader.ImageBase;
    size_ = nt->OptionalHeader.SizeOfImage;

    // Never at the preferred base: this host is itself linked at 0x140000000, so the relocated
    // path is the only one available, and it is the same rebasing arithmetic the plugin does.
    auto* at = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (at == nullptr) {
        error = "cannot reserve " + std::to_string(size_) + " bytes for the engine image";
        return false;
    }
    base_ = reinterpret_cast<uintptr_t>(at);

    std::memcpy(at, raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].SizeOfRawData != 0) {
            std::memcpy(at + sec[i].VirtualAddress, raw.data() + sec[i].PointerToRawData,
                        sec[i].SizeOfRawData);
        }
    }

    const auto& relocDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const uint64_t delta = base_ - preferredBase_;
    for (size_t walked = 0; walked < relocDir.Size;) {
        const auto* block =
            reinterpret_cast<const IMAGE_BASE_RELOCATION*>(at + relocDir.VirtualAddress + walked);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
            break;
        }
        const auto* entry = reinterpret_cast<const uint16_t*>(block + 1);
        const size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
        for (size_t i = 0; i < count; ++i) {
            const uint16_t type = static_cast<uint16_t>(entry[i] >> 12);
            if (type == IMAGE_REL_BASED_DIR64) {
                auto* slot =
                    reinterpret_cast<uint64_t*>(at + block->VirtualAddress + (entry[i] & 0x0FFF));
                *slot += delta;
            } else if (type != IMAGE_REL_BASED_ABSOLUTE) {
                error = "unsupported relocation type in the engine image";
                return false;
            }
        }
        walked += block->SizeOfBlock;
    }

    const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.Size != 0) {
        // Sized before anything is written, because the thunks carry pointers into thunkNames_
        // and a reallocation would leave the poison message pointing at freed storage.
        size_t imports = 0;
        for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                 at + impDir.VirtualAddress);
             d->Name != 0; ++d) {
            const auto* t = reinterpret_cast<const uint64_t*>(
                at + (d->OriginalFirstThunk != 0 ? d->OriginalFirstThunk : d->FirstThunk));
            while (*t++ != 0) {
                ++imports;
            }
        }
        thunkNames_.reserve(imports);
        thunks_ = VirtualAlloc(nullptr, (imports + 1) * kThunkSize, MEM_RESERVE | MEM_COMMIT,
                               PAGE_READWRITE);
        if (thunks_ == nullptr) {
            error = "cannot reserve the import poison thunks";
            return false;
        }

        auto* thunk = static_cast<uint8_t*>(thunks_);
        for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                 at + impDir.VirtualAddress);
             d->Name != 0; ++d) {
            const char* dll = reinterpret_cast<const char*>(at + d->Name);
            const auto* names = reinterpret_cast<const uint64_t*>(
                at + (d->OriginalFirstThunk != 0 ? d->OriginalFirstThunk : d->FirstThunk));
            auto* iat = reinterpret_cast<uint64_t*>(at + d->FirstThunk);
            for (size_t i = 0; names[i] != 0; ++i) {
                const char* fn = "(by ordinal)";
                if ((names[i] & IMAGE_ORDINAL_FLAG64) == 0) {
                    fn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(at + names[i])->Name;
                }
                if (IsAllowed(dll, fn)) {
                    const HMODULE mod = GetModuleHandleA(dll);
                    const FARPROC proc = mod != nullptr ? GetProcAddress(mod, fn) : nullptr;
                    if (proc == nullptr) {
                        error = std::string("allowlisted import ") + dll + "!" + fn +
                                " is not resolvable";
                        return false;
                    }
                    iat[i] = reinterpret_cast<uint64_t>(proc);
                    ++bound_;
                    continue;
                }
                thunkNames_.emplace_back(std::string(dll) + "!" + fn);
                EmitThunk(thunk, thunkNames_.back().c_str());
                iat[i] = reinterpret_cast<uint64_t>(thunk);
                thunk += kThunkSize;
                ++poisoned_;
            }
        }
        DWORD old = 0;
        VirtualProtect(thunks_, (imports + 1) * kThunkSize, PAGE_EXECUTE_READ, &old);
    }

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD ch = sec[i].Characteristics;
        DWORD protect = PAGE_READONLY;
        if ((ch & IMAGE_SCN_MEM_EXECUTE) != 0) {
            protect = (ch & IMAGE_SCN_MEM_WRITE) != 0 ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        } else if ((ch & IMAGE_SCN_MEM_WRITE) != 0) {
            protect = PAGE_READWRITE;
        }
        DWORD old = 0;
        const SIZE_T bytes =
            sec[i].Misc.VirtualSize != 0 ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        VirtualProtect(at + sec[i].VirtualAddress, bytes, protect, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), at, size_);

    // AddressOfEntryPoint is never called and IMAGE_DIRECTORY_ENTRY_TLS's callback list is never
    // walked. That is what makes the mapping safe to run offline: no engine initialiser, no
    // logging, no crash handler and no network stack starts. Only code the caller calls runs.
    return true;
}

}  // namespace w1l
