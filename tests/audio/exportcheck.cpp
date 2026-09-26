// Does echovr.exe actually export every decorated AK name akbind.cpp asks for?
//
// The design's claim is that the whole audio control surface is offset-free because each entry
// point is a PE export resolved by name. That claim is only as good as the names, and a name is the
// one kind of fact this project cannot anchor with a displacement -- so it gets its own check.
//
//   passes on   the shipped echovr.exe with the list akbind.cpp carries today
//   fails on    a misspelled name, a renamed export, or a different binary. --mutate proves it by
//               asking for a name that cannot exist and requiring the check to fail.
//   cannot see  whether the FUNCTION behind a correct name does what we think. That is O4's job and
//               it needs a launch; this arm is about identity, not behaviour.
//
// The export directory is parsed here rather than via GetProcAddress on a LoadLibrary of the game:
// loading echovr.exe into this process would run its TLS callbacks and its static initialisers,
// which is a far larger thing to do than read a table.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "audio/akbind.h"

namespace {

struct Image {
    std::vector<uint8_t> bytes;
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint32_t sectionCount = 0;
    const uint8_t* sections = nullptr;
    uint32_t exportRva = 0, exportSize = 0;

    bool Load(const char* path) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
            return false;
        }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            fclose(f);
            return false;
        }
        bytes.resize(static_cast<size_t>(size));
        const size_t got = fread(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        if (got != bytes.size()) {
            return false;
        }
        p = bytes.data();
        n = bytes.size();
        return Parse();
    }

    bool Parse() {
        if (n < 0x40 || p[0] != 'M' || p[1] != 'Z') {
            return false;
        }
        uint32_t lfanew = 0;
        memcpy(&lfanew, p + 0x3C, 4);
        if (lfanew + 0x18 >= n) {
            return false;
        }
        if (memcmp(p + lfanew, "PE\0\0", 4) != 0) {
            return false;
        }
        uint16_t nsec = 0, optSize = 0;
        memcpy(&nsec, p + lfanew + 6, 2);
        memcpy(&optSize, p + lfanew + 20, 2);
        const size_t opt = lfanew + 24;
        uint16_t magic = 0;
        memcpy(&magic, p + opt, 2);
        if (magic != 0x20B) {
            return false;   // PE32+ only; this project has exactly one binary
        }
        // Data directory 0 is the export table; it sits at +112 in the PE32+ optional header.
        memcpy(&exportRva, p + opt + 112, 4);
        memcpy(&exportSize, p + opt + 116, 4);
        sectionCount = nsec;
        sections = p + opt + optSize;
        return exportRva != 0;
    }

    const uint8_t* At(uint32_t rva, size_t need) const {
        for (uint32_t i = 0; i < sectionCount; ++i) {
            const uint8_t* s = sections + 40 * i;
            uint32_t vsize = 0, vaddr = 0, rsize = 0, raddr = 0;
            memcpy(&vsize, s + 8, 4);
            memcpy(&vaddr, s + 12, 4);
            memcpy(&rsize, s + 16, 4);
            memcpy(&raddr, s + 20, 4);
            const uint32_t span = vsize > rsize ? vsize : rsize;
            if (rva < vaddr || rva >= vaddr + span) {
                continue;
            }
            const uint32_t off = rva - vaddr;
            if (off + need > rsize || raddr + off + need > n) {
                return nullptr;
            }
            return p + raddr + off;
        }
        return nullptr;
    }
};

bool CollectExportNames(const Image& img, std::vector<std::string>* out) {
    const uint8_t* dir = img.At(img.exportRva, 40);
    if (dir == nullptr) {
        return false;
    }
    uint32_t nameCount = 0, nameTableRva = 0;
    memcpy(&nameCount, dir + 24, 4);
    memcpy(&nameTableRva, dir + 32, 4);
    const uint8_t* nameTable = img.At(nameTableRva, nameCount * 4u);
    if (nameTable == nullptr) {
        return false;
    }
    out->reserve(nameCount);
    for (uint32_t i = 0; i < nameCount; ++i) {
        uint32_t rva = 0;
        memcpy(&rva, nameTable + 4 * i, 4);
        const char* s = reinterpret_cast<const char*>(img.At(rva, 1));
        if (s == nullptr) {
            return false;
        }
        out->push_back(s);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    bool mutate = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mutate") == 0) {
            mutate = true;
        } else {
            path = argv[i];
        }
    }
    if (path == nullptr) {
        printf("NOT RUN: no echovr.exe path given\n");
        return 2;
    }

    Image img;
    if (!img.Load(path)) {
        printf("NOT RUN: could not read a PE32+ export table from %s\n", path);
        return 2;
    }

    std::vector<std::string> exported;
    if (!CollectExportNames(img, &exported)) {
        printf("NOT RUN: the export directory in %s did not parse\n", path);
        return 2;
    }
    printf("export directory: %zu names in %s\n", exported.size(), path);

    size_t count = 0;
    const replay::audio::AkSymbolName* table = replay::audio::SymbolTable(&count);

    size_t missing = 0;
    for (size_t i = 0; i < count; ++i) {
        std::string want = table[i].decorated;
        if (mutate && i == 0) {
            // The planted failure. Without it, a check that found every name would be
            // indistinguishable from a check that looked for none.
            want += "_NOT_AN_EXPORT";
        }
        bool found = false;
        for (const std::string& have : exported) {
            if (have == want) {
                found = true;
                break;
            }
        }
        if (!found) {
            printf("MISSING  %-34s %s\n", table[i].member, want.c_str());
            ++missing;
        }
    }

    printf("%zu of %zu AK names requested by akbind.cpp are exported by this binary\n",
           count - missing, count);
    if (mutate) {
        if (missing == 1) {
            printf("MUTATION CAUGHT: the planted name was reported missing\n");
            return 0;
        }
        printf("MUTATION SURVIVED: the check did not notice a name that cannot exist\n");
        return 1;
    }
    if (missing != 0) {
        printf("FAIL: %zu name(s) this build calls are not in the export table\n", missing);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
