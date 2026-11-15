#include "gamefiles.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <MinHook.h>
#include <zstd.h>

#include <echo/distribution/engine_mount.hpp>
#include <echo/distribution/install_detect.hpp>

namespace fs = std::filesystem;

namespace erv {

// ---- finding installs --------------------------------------------------------------------------------------------
namespace {
fs::path DataDir(const fs::path& install) {
    std::error_code ec;
    for (const auto& id : fs::directory_iterator(install / "_data", ec)) {
        const fs::path c = id.path() / "rad15" / "win10";
        if (fs::is_directory(c / "manifests", ec) && fs::is_directory(c / "packages", ec)) return c;
    }
    return {};
}
}  // namespace

bool IsEchoInstall(const fs::path& p) { return !p.empty() && !DataDir(p).empty(); }

fs::path EchoInstallFor(const fs::path& p) {
    for (fs::path q = p; !q.empty(); q = q.parent_path()) {
        if (IsEchoInstall(q)) return q;
        if (q == q.parent_path()) break;
    }
    return {};
}

std::vector<fs::path> FindEchoInstalls() {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& p) {
        if (!IsEchoInstall(p)) return;
        for (auto& o : out)
            if (fs::equivalent(o, p)) return;
        out.push_back(p);
    };
    const DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        const std::wstring root = std::wstring(1, wchar_t(L'A' + d)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        for (const wchar_t* sub : {L"Program Files\\Oculus\\Software\\Software\\ready-at-dawn-echo-arena",
                                   L"Program Files (x86)\\Oculus\\Software\\Software\\ready-at-dawn-echo-arena",
                                   L"Oculus\\Software\\Software\\ready-at-dawn-echo-arena", L"Oculus Apps\\Software\\ready-at-dawn-echo-arena",
                                   L"Games\\ready-at-dawn-echo-arena", L"Games\\EchoVR", L"Games\\Echo VR"})
            add(fs::path(root) / sub);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code ec2;
            if (!e.is_directory(ec2)) continue;
            const std::wstring n = e.path().filename().wstring();
            if (n == L"Windows" || n == L"$Recycle.Bin" || n == L"System Volume Information" || n == L"ProgramData") continue;
            add(e.path());
        }
    }
    return out;
}

// ---- the mounted data ----------------------------------------------------------------------------------------------
namespace gamefiles {
namespace {

using u8 = uint8_t;
constexpr uint32_t kMaxFrame = 64u << 20;
constexpr size_t kCacheFrames = 384;   // decompressed frames kept (frames are up to 512 KB each in stock data)

struct Source {
    std::string base, variant;   // "48037dc70b0ecab2", ".stock"
    fs::path manifest_file, packages;
    echo::dist::Manifest m;
    std::vector<HANDLE> parts;
    std::mutex part_mu;
    ~Source() {
        for (HANDLE h : parts)
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
struct Entry {
    uint16_t src;
    uint32_t row;
};
struct FileNode {
    std::wstring name;
    std::vector<Entry> entries;
};
struct Bucket {
    std::wstring name;
    std::vector<uint32_t> files;
    std::unordered_map<std::wstring, uint32_t> by_name;
};
struct Base {
    std::wstring name;
    std::vector<FileNode> files;
    std::vector<Bucket> buckets;
    std::unordered_map<std::wstring, uint32_t> by_bucket;
};
struct State {
    std::wstring root, root_lower;
    std::vector<std::unique_ptr<Source>> sources;
    std::vector<Base> bases;
    std::unordered_map<std::wstring, uint32_t> by_base;
    FILETIME time{};
    std::string why, preferred;
    std::vector<Report> reports;
    std::vector<std::string> variants;
    std::vector<fs::path> installs;
    fs::path corpus;
    uint32_t generation = 0;
};
std::shared_mutex g_mu;
State g_st;
std::atomic<uint64_t> g_served{0}, g_failures{0};

// decompressed frame cache
struct FrameKey {
    uint32_t gen;
    uint16_t src;
    uint32_t frame;
    bool operator<(const FrameKey& o) const { return std::tie(gen, src, frame) < std::tie(o.gen, o.src, o.frame); }
};
std::mutex g_cache_mu;
std::list<std::pair<FrameKey, std::shared_ptr<const std::vector<u8>>>> g_lru;
std::map<FrameKey, decltype(g_lru)::iterator> g_cache;

// originals (set when hooked; the plain API until then)
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFile2_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using SetFilePointerEx_t = BOOL(WINAPI*)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
using SetFilePointer_t = DWORD(WINAPI*)(HANDLE, LONG, PLONG, DWORD);
using GetFileSizeEx_t = BOOL(WINAPI*)(HANDLE, PLARGE_INTEGER);
using GetFileSize_t = DWORD(WINAPI*)(HANDLE, LPDWORD);
using GetFileType_t = DWORD(WINAPI*)(HANDLE);
using GetFileTime_t = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);
using GetFileInformationByHandle_t = BOOL(WINAPI*)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
using GetFileInformationByHandleEx_t = BOOL(WINAPI*)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
using GetFinalPathNameByHandleW_t = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);
using GetFileAttributesW_t = DWORD(WINAPI*)(LPCWSTR);
using GetFileAttributesExW_t = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using FindFirstFileExW_t = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindFirstFileW_t = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindNextFileW_t = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAW);
using FindClose_t = BOOL(WINAPI*)(HANDLE);
using CreateDirectoryW_t = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);

CreateFileW_t o_CreateFileW = ::CreateFileW;
CreateFile2_t o_CreateFile2 = ::CreateFile2;
ReadFile_t o_ReadFile = ::ReadFile;
SetFilePointerEx_t o_SetFilePointerEx = ::SetFilePointerEx;
SetFilePointer_t o_SetFilePointer = ::SetFilePointer;
GetFileSizeEx_t o_GetFileSizeEx = ::GetFileSizeEx;
GetFileSize_t o_GetFileSize = ::GetFileSize;
GetFileType_t o_GetFileType = ::GetFileType;
GetFileTime_t o_GetFileTime = ::GetFileTime;
GetFileInformationByHandle_t o_GetFileInformationByHandle = ::GetFileInformationByHandle;
GetFileInformationByHandleEx_t o_GetFileInformationByHandleEx = ::GetFileInformationByHandleEx;
GetFinalPathNameByHandleW_t o_GetFinalPathNameByHandleW = ::GetFinalPathNameByHandleW;
CloseHandle_t o_CloseHandle = ::CloseHandle;
GetFileAttributesW_t o_GetFileAttributesW = ::GetFileAttributesW;
GetFileAttributesExW_t o_GetFileAttributesExW = ::GetFileAttributesExW;
FindFirstFileExW_t o_FindFirstFileExW = ::FindFirstFileExW;
FindFirstFileW_t o_FindFirstFileW = ::FindFirstFileW;
FindNextFileW_t o_FindNextFileW = ::FindNextFileW;
FindClose_t o_FindClose = ::FindClose;
CreateDirectoryW_t o_CreateDirectoryW = ::CreateDirectoryW;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ---- reading resources ----------------------------------------------------------------------------------------------
bool ReadAt(HANDLE h, uint64_t off, void* dst, uint32_t n) {
    OVERLAPPED ov{};
    ov.Offset = DWORD(off), ov.OffsetHigh = DWORD(off >> 32);
    DWORD got = 0;
    return o_ReadFile(h, dst, n, &got, &ov) && got == n;
}

HANDLE Part(Source& s, uint32_t i) {
    std::lock_guard<std::mutex> l(s.part_mu);
    if (i >= s.parts.size()) return INVALID_HANDLE_VALUE;
    if (!s.parts[i]) {
        const fs::path p = s.packages / (s.base + "_" + std::to_string(i));
        s.parts[i] = o_CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    return s.parts[i];
}

std::shared_ptr<const std::vector<u8>> DecodeFrame(Source& s, uint32_t fi) {
    if (fi >= s.m.frames.size()) return nullptr;
    const echo::dist::FrameEntry& f = s.m.frames[fi];
    if (f.decompressed_size > kMaxFrame || f.compressed_size > kMaxFrame) return nullptr;
    const HANDLE h = Part(s, f.package_path_index);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    std::vector<u8> packed(f.compressed_size);
    if (!ReadAt(h, f.cumulative_offset, packed.data(), f.compressed_size)) return nullptr;
    auto out = std::make_shared<std::vector<u8>>(f.decompressed_size);
    const size_t got = ZSTD_decompress(out->data(), out->size(), packed.data(), packed.size());
    if (ZSTD_isError(got) || got != out->size()) {
        // a frame stored raw (same size both ways) is taken as it is
        if (f.compressed_size == f.decompressed_size) {
            *out = std::move(packed);
            return out;
        }
        return nullptr;
    }
    return out;
}

std::shared_ptr<const std::vector<u8>> Frame(uint32_t gen, uint16_t src, Source& s, uint32_t fi) {
    const FrameKey k{gen, src, fi};
    {
        std::lock_guard<std::mutex> l(g_cache_mu);
        auto it = g_cache.find(k);
        if (it != g_cache.end()) {
            g_lru.splice(g_lru.begin(), g_lru, it->second);
            return it->second->second;
        }
    }
    auto f = DecodeFrame(s, fi);
    if (!f) return nullptr;
    std::lock_guard<std::mutex> l(g_cache_mu);
    if (g_cache.count(k)) return f;
    g_lru.emplace_front(k, f);
    g_cache[k] = g_lru.begin();
    while (g_lru.size() > kCacheFrames) {
        g_cache.erase(g_lru.back().first);
        g_lru.pop_back();
    }
    return f;
}

// Reads a file of the virtual corpus: from its first source that decodes, else the next.
std::shared_ptr<const std::vector<u8>> ReadFileNode(uint32_t base, uint32_t file) {
    std::shared_lock<std::shared_mutex> l(g_mu);
    if (base >= g_st.bases.size() || file >= g_st.bases[base].files.size()) return nullptr;
    for (const Entry& e : g_st.bases[base].files[file].entries) {
        Source& s = *g_st.sources[e.src];
        const auto& r = s.m.resources[e.row];
        auto f = Frame(g_st.generation, e.src, s, r.frame_index);
        if (!f || uint64_t(r.offset) + r.size > f->size()) continue;
        ++g_served;
        return std::make_shared<const std::vector<u8>>(f->begin() + r.offset, f->begin() + r.offset + r.size);
    }
    ++g_failures;
    return nullptr;
}

// ---- the virtual tree ------------------------------------------------------------------------------------------------
enum Kind { kMissingParent, kMissing, kDir, kFile };
struct Node {
    Kind kind = kMissingParent;
    int base = -1, bucket = -1, file = -1;
};

// `full` (absolute, backslashes) under the root? Then `rel` is the lower-case remainder ("" for the root).
bool UnderRoot(const std::wstring& full, std::wstring& rel) {
    const std::wstring& r = g_st.root_lower;
    if (r.empty() || full.size() < r.size()) return false;
    for (size_t i = 0; i < r.size(); ++i)
        if (wchar_t(towlower(full[i])) != r[i]) return false;
    if (full.size() == r.size()) return rel.clear(), true;
    if (full[r.size()] != L'\\') return false;
    rel = Lower(full.substr(r.size() + 1));
    while (!rel.empty() && rel.back() == L'\\') rel.pop_back();
    return true;
}

// Absolute form of a path the program gave, or false when it is not one of ours.
bool Resolve(LPCWSTR in, std::wstring& full, std::wstring& rel) {
    if (!in || !*in || g_st.root_lower.empty()) return false;
    std::wstring p = in;
    if (p.rfind(L"\\\\?\\", 0) == 0 || p.rfind(L"\\??\\", 0) == 0) {
        if (p.size() > 6 && p[5] == L':') p = p.substr(4);
        else return false;
    }
    wchar_t buf[2048];
    const DWORD n = GetFullPathNameW(p.c_str(), 2048, buf, nullptr);
    if (n == 0 || n >= 2048) return false;
    full.assign(buf, n);
    for (auto& c : full)
        if (c == L'/') c = L'\\';
    while (full.size() > 3 && full.back() == L'\\') full.pop_back();
    return UnderRoot(full, rel);
}

Node Lookup(const std::wstring& rel) {
    Node n;
    if (rel.empty()) return n.kind = kDir, n;
    std::vector<std::wstring> parts;
    for (size_t a = 0;;) {
        const size_t b = rel.find(L'\\', a);
        parts.push_back(rel.substr(a, b == std::wstring::npos ? b : b - a));
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    auto bi = g_st.by_base.find(parts[0]);
    if (bi == g_st.by_base.end()) return n.kind = parts.size() == 1 ? kMissing : kMissingParent, n;
    n.base = int(bi->second);
    if (parts.size() == 1) return n.kind = kDir, n;
    const Base& b = g_st.bases[n.base];
    auto ki = b.by_bucket.find(parts[1]);
    if (ki == b.by_bucket.end()) return n.kind = parts.size() == 2 ? kMissing : kMissingParent, n;
    n.bucket = int(ki->second);
    if (parts.size() == 2) return n.kind = kDir, n;
    if (parts.size() > 3) return n.kind = kMissingParent, n;
    const Bucket& k = b.buckets[n.bucket];
    auto fi = k.by_name.find(parts[2]);
    if (fi == k.by_name.end()) return n.kind = kMissing, n;
    n.file = int(fi->second);
    return n.kind = kFile, n;
}

uint64_t SizeOf(const Node& n) {
    if (n.kind != kFile) return 0;
    const FileNode& f = g_st.bases[n.base].files[n.file];
    if (f.entries.empty()) return 0;
    return g_st.sources[f.entries[0].src]->m.resources[f.entries[0].row].size;
}

uint64_t IdOf(const Node& n) { return (uint64_t(n.base + 1) << 48) ^ (uint64_t(n.bucket + 1) << 32) ^ uint64_t(n.file + 1); }

void FillFind(WIN32_FIND_DATAW& d, const std::wstring& name, bool dir, uint64_t size) {
    d = {};
    d.dwFileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    d.ftCreationTime = d.ftLastAccessTime = d.ftLastWriteTime = g_st.time;
    d.nFileSizeHigh = DWORD(size >> 32), d.nFileSizeLow = DWORD(size);
    wcsncpy_s(d.cFileName, name.c_str(), _TRUNCATE);
}

bool Wild(const wchar_t* p, const wchar_t* s) {   // case-insensitive * and ?
    for (; *p; ++p, ++s) {
        if (*p == L'*') {
            while (p[1] == L'*') ++p;
            if (!p[1]) return true;
            for (; *s; ++s)
                if (Wild(p + 1, s)) return true;
            return false;
        }
        if (!*s || (*p != L'?' && towlower(*p) != towlower(*s))) return false;
    }
    return !*s;
}

// ---- handles -----------------------------------------------------------------------------------------------------------
struct VHandle {
    Node node;
    std::wstring full;
    uint64_t size = 0, pos = 0;
    std::shared_ptr<const std::vector<u8>> data;
    bool tried = false;
};
struct VFind {
    std::vector<WIN32_FIND_DATAW> items;
    size_t next = 0;
};
std::mutex g_hmu;
std::unordered_map<HANDLE, std::unique_ptr<VHandle>> g_handles;
std::unordered_map<HANDLE, std::unique_ptr<VFind>> g_finds;
std::atomic<int> g_live{0};

HANDLE NewToken() { return CreateEventW(nullptr, TRUE, FALSE, nullptr); }

VHandle* Find(HANDLE h) {
    if (g_live.load(std::memory_order_relaxed) == 0) return nullptr;
    std::lock_guard<std::mutex> l(g_hmu);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second.get();
}
VFind* FindSearch(HANDLE h) {
    if (g_live.load(std::memory_order_relaxed) == 0) return nullptr;
    std::lock_guard<std::mutex> l(g_hmu);
    auto it = g_finds.find(h);
    return it == g_finds.end() ? nullptr : it->second.get();
}

bool Load(VHandle& v) {
    if (!v.tried) {
        v.tried = true;
        v.data = ReadFileNode(uint32_t(v.node.base), uint32_t(v.node.file));
        if (v.data) v.size = v.data->size();
    }
    return v.data != nullptr;
}

HANDLE OpenVirtual(LPCWSTR name, DWORD access, DWORD disposition, DWORD flags, bool& handled) {
    handled = false;
    std::wstring full, rel;
    std::shared_lock<std::shared_mutex> l(g_mu);
    if (!Resolve(name, full, rel)) return INVALID_HANDLE_VALUE;
    handled = true;
    const Node n = Lookup(rel);
    const DWORD writes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER;
    if (n.kind == kMissingParent || n.kind == kMissing) {
        SetLastError(disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS ? ERROR_ACCESS_DENIED
                     : n.kind == kMissing                                                                    ? ERROR_FILE_NOT_FOUND
                                                                                                             : ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if ((access & writes) || disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (disposition == CREATE_NEW) {
        SetLastError(ERROR_FILE_EXISTS);
        return INVALID_HANDLE_VALUE;
    }
    if (n.kind == kDir && !(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    auto v = std::make_unique<VHandle>();
    v->node = n;
    v->full = full;
    v->size = SizeOf(n);
    const HANDLE h = NewToken();
    if (!h) return INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> hl(g_hmu);
        g_handles[h] = std::move(v);
        ++g_live;
    }
    SetLastError(disposition == OPEN_ALWAYS ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return h;
}

// ---- hooks ---------------------------------------------------------------------------------------------------------------
HANDLE WINAPI H_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    bool handled;
    const HANDLE h = OpenVirtual(name, access, disp, flags, handled);
    return handled ? h : o_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}
HANDLE WINAPI H_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disp, LPCREATEFILE2_EXTENDED_PARAMETERS p) {
    bool handled;
    const HANDLE h = OpenVirtual(name, access, disp, p ? p->dwFileFlags : 0, handled);
    return handled ? h : o_CreateFile2(name, access, share, disp, p);
}
BOOL WINAPI H_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    VHandle* v = Find(h);
    if (!v) return o_ReadFile(h, buf, n, got, ov);
    if (got) *got = 0;
    if (v->node.kind != kFile) return SetLastError(ERROR_INVALID_FUNCTION), FALSE;
    if (!Load(*v)) return SetLastError(ERROR_READ_FAULT), FALSE;
    uint64_t at = ov ? (uint64_t(ov->OffsetHigh) << 32 | ov->Offset) : v->pos;
    const uint64_t take = at >= v->size ? 0 : std::min<uint64_t>(n, v->size - at);
    if (take) memcpy(buf, v->data->data() + at, size_t(take));
    if (got) *got = DWORD(take);
    if (ov) {
        ov->Internal = 0, ov->InternalHigh = ULONG_PTR(take);
        if (ov->hEvent) SetEvent(ov->hEvent);
    } else {
        v->pos = at + take;
    }
    if (ov && take == 0 && n) return SetLastError(ERROR_HANDLE_EOF), FALSE;
    return TRUE;
}
BOOL WINAPI H_SetFilePointerEx(HANDLE h, LARGE_INTEGER d, PLARGE_INTEGER out, DWORD how) {
    VHandle* v = Find(h);
    if (!v) return o_SetFilePointerEx(h, d, out, how);
    if (v->node.kind == kFile) Load(*v);
    const int64_t base = how == FILE_BEGIN ? 0 : how == FILE_CURRENT ? int64_t(v->pos) : int64_t(v->size);
    const int64_t np = base + d.QuadPart;
    if (np < 0) return SetLastError(ERROR_NEGATIVE_SEEK), FALSE;
    v->pos = uint64_t(np);
    if (out) out->QuadPart = np;
    return TRUE;
}
DWORD WINAPI H_SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD how) {
    VHandle* v = Find(h);
    if (!v) return o_SetFilePointer(h, lo, hi, how);
    LARGE_INTEGER d, out{};
    d.QuadPart = hi ? (int64_t(*hi) << 32 | uint32_t(lo)) : int64_t(lo);
    if (!H_SetFilePointerEx(h, d, &out, how)) return INVALID_SET_FILE_POINTER;
    if (hi) *hi = LONG(out.HighPart);
    SetLastError(ERROR_SUCCESS);
    return out.LowPart;
}
BOOL WINAPI H_GetFileSizeEx(HANDLE h, PLARGE_INTEGER out) {
    VHandle* v = Find(h);
    if (!v) return o_GetFileSizeEx(h, out);
    out->QuadPart = int64_t(v->size);
    return TRUE;
}
DWORD WINAPI H_GetFileSize(HANDLE h, LPDWORD hi) {
    VHandle* v = Find(h);
    if (!v) return o_GetFileSize(h, hi);
    if (hi) *hi = DWORD(v->size >> 32);
    SetLastError(ERROR_SUCCESS);
    return DWORD(v->size);
}
DWORD WINAPI H_GetFileType(HANDLE h) { return Find(h) ? FILE_TYPE_DISK : o_GetFileType(h); }
BOOL WINAPI H_GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w) {
    if (!Find(h)) return o_GetFileTime(h, c, a, w);
    if (c) *c = g_st.time;
    if (a) *a = g_st.time;
    if (w) *w = g_st.time;
    return TRUE;
}
BOOL WINAPI H_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION i) {
    VHandle* v = Find(h);
    if (!v) return o_GetFileInformationByHandle(h, i);
    *i = {};
    i->dwFileAttributes = v->node.kind == kDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    i->ftCreationTime = i->ftLastAccessTime = i->ftLastWriteTime = g_st.time;
    i->dwVolumeSerialNumber = 0xEC40EC40u;
    i->nFileSizeHigh = DWORD(v->size >> 32), i->nFileSizeLow = DWORD(v->size);
    i->nNumberOfLinks = 1;
    const uint64_t id = IdOf(v->node);
    i->nFileIndexHigh = DWORD(id >> 32), i->nFileIndexLow = DWORD(id);
    return TRUE;
}
BOOL WINAPI H_GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buf, DWORD size) {
    VHandle* v = Find(h);
    if (!v) return o_GetFileInformationByHandleEx(h, cls, buf, size);
    const bool dir = v->node.kind == kDir;
    const DWORD attrs = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    LARGE_INTEGER t;
    t.LowPart = g_st.time.dwLowDateTime, t.HighPart = LONG(g_st.time.dwHighDateTime);
    switch (cls) {
        case FileBasicInfo: {
            if (size < sizeof(FILE_BASIC_INFO)) break;
            auto* b = static_cast<FILE_BASIC_INFO*>(buf);
            b->CreationTime = b->LastAccessTime = b->LastWriteTime = b->ChangeTime = t;
            b->FileAttributes = attrs;
            return TRUE;
        }
        case FileStandardInfo: {
            if (size < sizeof(FILE_STANDARD_INFO)) break;
            auto* s = static_cast<FILE_STANDARD_INFO*>(buf);
            s->AllocationSize.QuadPart = s->EndOfFile.QuadPart = int64_t(v->size);
            s->NumberOfLinks = 1, s->DeletePending = FALSE, s->Directory = dir;
            return TRUE;
        }
        case FileAttributeTagInfo: {
            if (size < sizeof(FILE_ATTRIBUTE_TAG_INFO)) break;
            auto* a = static_cast<FILE_ATTRIBUTE_TAG_INFO*>(buf);
            a->FileAttributes = attrs, a->ReparseTag = 0;
            return TRUE;
        }
        case FileIdInfo: {
            if (size < sizeof(FILE_ID_INFO)) break;
            auto* a = static_cast<FILE_ID_INFO*>(buf);
            *a = {};
            a->VolumeSerialNumber = 0xEC40EC40u;
            const uint64_t id = IdOf(v->node);
            memcpy(a->FileId.Identifier, &id, sizeof id);
            return TRUE;
        }
        case FileNameInfo: {
            const std::wstring tail = v->full.size() > 2 ? v->full.substr(2) : v->full;   // without the drive
            const DWORD need = DWORD(offsetof(FILE_NAME_INFO, FileName) + tail.size() * sizeof(wchar_t));
            if (size < offsetof(FILE_NAME_INFO, FileName)) break;
            auto* a = static_cast<FILE_NAME_INFO*>(buf);
            a->FileNameLength = DWORD(tail.size() * sizeof(wchar_t));
            if (size < need) return SetLastError(ERROR_MORE_DATA), FALSE;
            memcpy(a->FileName, tail.data(), tail.size() * sizeof(wchar_t));
            return TRUE;
        }
        default: return SetLastError(ERROR_INVALID_PARAMETER), FALSE;
    }
    return SetLastError(ERROR_BAD_LENGTH), FALSE;
}
DWORD WINAPI H_GetFinalPathNameByHandleW(HANDLE h, LPWSTR buf, DWORD cch, DWORD flags) {
    VHandle* v = Find(h);
    if (!v) return o_GetFinalPathNameByHandleW(h, buf, cch, flags);
    const std::wstring p = (flags & VOLUME_NAME_NONE) ? v->full.substr(2) : L"\\\\?\\" + v->full;
    if (cch <= p.size()) return DWORD(p.size() + 1);
    memcpy(buf, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
    return DWORD(p.size());
}
BOOL WINAPI H_CloseHandle(HANDLE h) {
    if (g_live.load(std::memory_order_relaxed)) {
        std::unique_ptr<VHandle> gone;
        {
            std::lock_guard<std::mutex> l(g_hmu);
            auto it = g_handles.find(h);
            if (it != g_handles.end()) {
                gone = std::move(it->second);
                g_handles.erase(it);
                --g_live;
            }
        }
        if (gone) return o_CloseHandle(h);
    }
    return o_CloseHandle(h);
}
DWORD Attrs(LPCWSTR name, bool& handled, uint64_t* size = nullptr) {
    handled = false;
    std::wstring full, rel;
    std::shared_lock<std::shared_mutex> l(g_mu);
    if (!Resolve(name, full, rel)) return 0;
    handled = true;
    const Node n = Lookup(rel);
    if (n.kind == kMissing || n.kind == kMissingParent) {
        SetLastError(n.kind == kMissing ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    if (size) *size = SizeOf(n);
    return n.kind == kDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
}
DWORD WINAPI H_GetFileAttributesW(LPCWSTR name) {
    bool handled;
    const DWORD a = Attrs(name, handled);
    return handled ? a : o_GetFileAttributesW(name);
}
BOOL WINAPI H_GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS lvl, LPVOID out) {
    bool handled;
    uint64_t size = 0;
    const DWORD a = Attrs(name, handled, &size);
    if (!handled) return o_GetFileAttributesExW(name, lvl, out);
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    auto* d = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(out);
    d->dwFileAttributes = a;
    d->ftCreationTime = d->ftLastAccessTime = d->ftLastWriteTime = g_st.time;
    d->nFileSizeHigh = DWORD(size >> 32), d->nFileSizeLow = DWORD(size);
    return TRUE;
}
HANDLE FindVirtual(LPCWSTR spec, LPWIN32_FIND_DATAW out, bool& handled) {
    handled = false;
    if (!spec) return INVALID_HANDLE_VALUE;
    std::wstring s = spec;
    const size_t cut = s.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return INVALID_HANDLE_VALUE;
    std::wstring pattern = s.substr(cut + 1), dir = s.substr(0, cut);
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    std::wstring full, rel;
    std::shared_lock<std::shared_mutex> l(g_mu);
    if (!Resolve(dir.c_str(), full, rel)) return INVALID_HANDLE_VALUE;
    handled = true;
    const Node n = Lookup(rel);
    if (n.kind != kDir) return SetLastError(n.kind == kFile ? ERROR_DIRECTORY : ERROR_PATH_NOT_FOUND), INVALID_HANDLE_VALUE;
    if (pattern == L"*.*" || pattern.empty()) pattern = L"*";
    auto f = std::make_unique<VFind>();
    WIN32_FIND_DATAW d;
    auto add = [&](const std::wstring& name, bool isDir, uint64_t size) {
        if (!Wild(pattern.c_str(), name.c_str())) return;
        FillFind(d, name, isDir, size);
        f->items.push_back(d);
    };
    if (n.base < 0) {
        for (const Base& b : g_st.bases) add(b.name, true, 0);
    } else if (n.bucket < 0) {
        for (const Bucket& k : g_st.bases[n.base].buckets) add(k.name, true, 0);
    } else {
        const Base& b = g_st.bases[n.base];
        for (uint32_t fi : b.buckets[n.bucket].files) {
            Node fn{kFile, n.base, n.bucket, int(fi)};
            add(b.files[fi].name, false, SizeOf(fn));
        }
    }
    if (f->items.empty()) return SetLastError(ERROR_FILE_NOT_FOUND), INVALID_HANDLE_VALUE;
    *out = f->items[0];
    f->next = 1;
    const HANDLE h = NewToken();
    std::lock_guard<std::mutex> hl(g_hmu);
    g_finds[h] = std::move(f);
    ++g_live;
    SetLastError(ERROR_SUCCESS);
    return h;
}
HANDLE WINAPI H_FindFirstFileExW(LPCWSTR spec, FINDEX_INFO_LEVELS lvl, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    bool handled;
    const HANDLE h = FindVirtual(spec, static_cast<LPWIN32_FIND_DATAW>(data), handled);
    return handled ? h : o_FindFirstFileExW(spec, lvl, data, op, filter, flags);
}
HANDLE WINAPI H_FindFirstFileW(LPCWSTR spec, LPWIN32_FIND_DATAW data) {
    bool handled;
    const HANDLE h = FindVirtual(spec, data, handled);
    return handled ? h : o_FindFirstFileW(spec, data);
}
BOOL WINAPI H_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data) {
    VFind* f = FindSearch(h);
    if (!f) return o_FindNextFileW(h, data);
    if (f->next >= f->items.size()) return SetLastError(ERROR_NO_MORE_FILES), FALSE;
    *data = f->items[f->next++];
    return TRUE;
}
BOOL WINAPI H_FindClose(HANDLE h) {
    if (g_live.load(std::memory_order_relaxed)) {
        bool mine = false;
        {
            std::lock_guard<std::mutex> l(g_hmu);
            mine = g_finds.erase(h) > 0;
            if (mine) --g_live;
        }
        if (mine) return o_CloseHandle(h);
    }
    return o_FindClose(h);
}
BOOL WINAPI H_CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa) {
    bool handled;
    const DWORD a = Attrs(name, handled);
    if (!handled) return o_CreateDirectoryW(name, sa);
    SetLastError(a != INVALID_FILE_ATTRIBUTES ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED);
    return FALSE;
}

bool g_hooked = false;
std::string g_hook_error;

template <class T>
void Hook(const char* name, T detour, T& orig) {
    T trampoline = nullptr;
    const MH_STATUS s = MH_CreateHookApi(L"kernelbase.dll", name, reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(&trampoline));
    if (s == MH_OK) orig = trampoline;
    else g_hook_error += std::string(name) + "(" + MH_StatusToString(s) + ") ";
}

void InstallHooks() {
    if (g_hooked) return;
    g_hooked = true;
    if (MH_Initialize() != MH_OK) {
        g_hook_error = "MinHook could not start";
        return;
    }
    Hook("CreateFileW", &H_CreateFileW, o_CreateFileW);
    Hook("CreateFile2", &H_CreateFile2, o_CreateFile2);
    Hook("ReadFile", &H_ReadFile, o_ReadFile);
    Hook("SetFilePointerEx", &H_SetFilePointerEx, o_SetFilePointerEx);
    Hook("SetFilePointer", &H_SetFilePointer, o_SetFilePointer);
    Hook("GetFileSizeEx", &H_GetFileSizeEx, o_GetFileSizeEx);
    Hook("GetFileSize", &H_GetFileSize, o_GetFileSize);
    Hook("GetFileType", &H_GetFileType, o_GetFileType);
    Hook("GetFileTime", &H_GetFileTime, o_GetFileTime);
    Hook("GetFileInformationByHandle", &H_GetFileInformationByHandle, o_GetFileInformationByHandle);
    Hook("GetFileInformationByHandleEx", &H_GetFileInformationByHandleEx, o_GetFileInformationByHandleEx);
    Hook("GetFinalPathNameByHandleW", &H_GetFinalPathNameByHandleW, o_GetFinalPathNameByHandleW);
    Hook("CloseHandle", &H_CloseHandle, o_CloseHandle);
    Hook("GetFileAttributesW", &H_GetFileAttributesW, o_GetFileAttributesW);
    Hook("GetFileAttributesExW", &H_GetFileAttributesExW, o_GetFileAttributesExW);
    Hook("FindFirstFileExW", &H_FindFirstFileExW, o_FindFirstFileExW);
    Hook("FindFirstFileW", &H_FindFirstFileW, o_FindFirstFileW);
    Hook("FindNextFileW", &H_FindNextFileW, o_FindNextFileW);
    Hook("FindClose", &H_FindClose, o_FindClose);
    Hook("CreateDirectoryW", &H_CreateDirectoryW, o_CreateDirectoryW);
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) g_hook_error += "enable failed";
}

// ---- reading manifests ----------------------------------------------------------------------------------------------
int VariantRank(const std::string& v) {
    if (v.empty()) return 0;          // what the game runs now
    if (v == ".stock") return 1;      // as shipped
    if (v == ".bak") return 2;
    return 3;
}

uint64_t U64(const std::vector<u8>& b, size_t at) {
    uint64_t v = 0;
    if (at + 8 <= b.size()) memcpy(&v, b.data() + at, 8);
    return v;
}
uint32_t U32(const std::vector<u8>& b, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= b.size()) memcpy(&v, b.data() + at, 4);
    return v;
}

// The manifest's decompressed payload: {"ZSTD"|"NONE", u32, u64 payload size, u64 stored size} + data.
bool ManifestPayload(const fs::path& file, std::vector<u8>& payload, std::string& why) {
    std::vector<u8> bytes;
    {
        const HANDLE h = o_CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return why = "cannot be opened", false;
        LARGE_INTEGER n{};
        o_GetFileSizeEx(h, &n);
        bytes.resize(size_t(std::min<int64_t>(n.QuadPart, 256ll << 20)));
        const bool ok = bytes.empty() || ReadAt(h, 0, bytes.data(), uint32_t(bytes.size()));
        o_CloseHandle(h);
        if (!ok) return why = "cannot be read", false;
    }
    if (bytes.size() < 24) return why = "too short to be a manifest", false;
    const uint64_t declared = U64(bytes, 8), stored = std::min<uint64_t>(U64(bytes, 16), bytes.size() - 24);
    const u8* data = bytes.data() + 24;
    if (memcmp(bytes.data(), "NONE", 4) == 0) {
        payload.assign(data, data + stored);
        return true;
    }
    if (memcmp(bytes.data(), "ZSTD", 4) != 0) return why = "not a manifest (no ZSTD/NONE header)", false;
    // the size the header declares, else the one the zstd frame declares, else grow until it fits
    std::vector<uint64_t> sizes{declared};
    const unsigned long long frame = ZSTD_getFrameContentSize(data, size_t(stored));
    if (frame != ZSTD_CONTENTSIZE_UNKNOWN && frame != ZSTD_CONTENTSIZE_ERROR && frame != declared) sizes.push_back(frame);
    for (uint64_t guess = 1u << 20; guess <= (256ull << 20); guess *= 4) sizes.push_back(guess);
    for (uint64_t n : sizes) {
        if (n == 0 || n > (256ull << 20)) continue;
        payload.resize(size_t(n));
        const size_t got = ZSTD_decompress(payload.data(), payload.size(), data, size_t(stored));
        if (!ZSTD_isError(got)) {
            payload.resize(got);
            return true;
        }
    }
    return why = "does not decompress", false;
}

// The resource/extended/frame tables, read several ways: the editor's strict reading first, then the layouts
// a rewritten manifest may use (tables packed by count, by capacity, by the descriptors' byte totals, frames
// at the end). Each candidate is checked against the packages by the caller.
struct Layout {
    const char* name;
    echo::dist::Manifest m;
};
std::vector<Layout> ManifestLayouts(const std::vector<u8>& p) {
    std::vector<Layout> out;
    constexpr size_t kHeader = 192, kStride[3] = {32, 40, 16}, kDesc[3] = {8, 72, 136};
    if (p.size() < kHeader) return out;
    uint64_t total[3], cap[3], count[3];
    for (int i = 0; i < 3; ++i) total[i] = U64(p, kDesc[i] + 8), cap[i] = U64(p, kDesc[i] + 40), count[i] = U64(p, kDesc[i] + 48);
    auto make = [&](const char* name, uint64_t nres, uint64_t res_at, uint64_t nframes, uint64_t frames_at) {
        if (nres == 0 || nres > 10'000'000 || nframes == 0 || nframes > 10'000'000) return;
        if (res_at + nres * kStride[0] > p.size() || frames_at + nframes * kStride[2] > p.size()) return;
        Layout l{name, {}};
        l.m.package_count = U32(p, 0);
        l.m.block_size = U32(p, 4);
        l.m.resources.resize(size_t(nres));
        for (size_t i = 0; i < l.m.resources.size(); ++i) {
            const size_t at = size_t(res_at) + i * kStride[0];
            l.m.resources[i] = {U64(p, at), U64(p, at + 8), U32(p, at + 16), U32(p, at + 20), U32(p, at + 24), U32(p, at + 28)};
        }
        l.m.frames.resize(size_t(nframes));
        for (size_t i = 0; i < l.m.frames.size(); ++i) {
            const size_t at = size_t(frames_at) + i * kStride[2];
            l.m.frames[i] = {U32(p, at), U32(p, at + 4), U32(p, at + 8), U32(p, at + 12)};
        }
        out.push_back(std::move(l));
    };
    auto strict = echo::dist::read_manifest_payload(std::span<const u8>(p.data(), p.size()));
    if (strict.has_value()) out.push_back({"strict", std::move(strict.value())});
    make("packed by count", count[0], kHeader, count[2], kHeader + count[0] * kStride[0] + count[1] * kStride[1]);
    make("packed by capacity", count[0], kHeader, count[2], kHeader + cap[0] * kStride[0] + cap[1] * kStride[1]);
    make("packed by byte totals", count[0], kHeader, count[2], kHeader + total[0] + total[1]);
    if (count[2] * kStride[2] <= p.size()) make("frames at the end", count[0], kHeader, count[2], p.size() - count[2] * kStride[2]);
    // counts that disagree with capacity: try each as the row count
    if (cap[0] != count[0]) make("capacity as count", cap[0], kHeader, cap[2], kHeader + cap[0] * kStride[0] + cap[1] * kStride[1]);
    return out;
}

// One layout against the packages. Rows whose frame is outside the table or the package files are dropped
// (another source may serve them); the layout is usable when most rows are good and a spread of frames decodes.
// Returns the number of good rows (the caller keeps the layout with the most).
size_t CheckLayout(Source& s, const std::vector<uint64_t>& sizes, std::string& why) {
    if (s.m.resources.empty()) return why = "holds no files", 0;
    const size_t total = s.m.resources.size();
    std::vector<int8_t> frame_ok(s.m.frames.size(), -1);
    size_t past_table = 0, past_frame = 0, past_package = 0;
    std::string example;
    std::vector<echo::dist::ResourceIndexEntry> kept;
    kept.reserve(total);
    for (const auto& r : s.m.resources) {
        if (r.frame_index >= s.m.frames.size()) {
            ++past_table;
            continue;
        }
        const auto& f = s.m.frames[r.frame_index];
        if (uint64_t(r.offset) + r.size > f.decompressed_size) {
            ++past_frame;
            continue;
        }
        int8_t& ok = frame_ok[r.frame_index];
        if (ok < 0) {
            ok = f.decompressed_size <= kMaxFrame && f.package_path_index < sizes.size() &&
                 uint64_t(f.cumulative_offset) + f.compressed_size <= sizes[f.package_path_index];
            if (!ok && example.empty())
                example = "frame " + std::to_string(r.frame_index) + " part " + std::to_string(f.package_path_index) + " @" +
                          std::to_string(f.cumulative_offset) + "+" + std::to_string(f.compressed_size) + " vs part size " +
                          (f.package_path_index < sizes.size() ? std::to_string(sizes[f.package_path_index]) : std::string("none"));
        }
        if (!ok) {
            ++past_package;
            continue;
        }
        kept.push_back(r);
    }
    char b[256];
    snprintf(b, sizeof b, "%zu/%zu rows good (%zu past the frame table, %zu outside their frame, %zu outside the packages%s%s)", kept.size(), total,
             past_table, past_frame, past_package, example.empty() ? "" : ", e.g. ", example.c_str());
    why = b;
    if (kept.size() * 2 < total) return 0;
    std::vector<uint32_t> used;
    for (uint32_t i = 0; i < frame_ok.size(); ++i)
        if (frame_ok[i] == 1) used.push_back(i);
    for (size_t k : {size_t(0), used.size() / 4, used.size() / 2, 3 * used.size() / 4, used.size() - 1}) {
        if (k < used.size() && !DecodeFrame(s, used[k])) return why += "; frame " + std::to_string(used[k]) + " does not decompress", 0;
    }
    s.m.resources = std::move(kept);
    return s.m.resources.size();
}

// Opens and checks one manifest file as a source, trying each table layout until one fits the packages.
std::unique_ptr<Source> OpenSource(const fs::path& file, const fs::path& packages, std::string& why, std::string& how) {
    auto s = std::make_unique<Source>();
    const std::wstring fname = file.filename().wstring();
    const size_t dot = fname.find(L'.');
    s->base = std::string(fname.begin(), dot == std::wstring::npos ? fname.end() : fname.begin() + dot);
    s->variant = dot == std::wstring::npos ? std::string() : std::string(fname.begin() + dot, fname.end());
    s->manifest_file = file;
    s->packages = packages;
    if (s->base.size() != 16 || s->base.find_first_not_of("0123456789abcdef") != std::string::npos) return why = "not a manifest name", nullptr;
    std::vector<u8> payload;
    if (!ManifestPayload(file, payload, why)) return nullptr;
    std::vector<Layout> layouts = ManifestLayouts(payload);
    if (layouts.empty()) return why = "no table layout fits its " + std::to_string(payload.size()) + " bytes", nullptr;
    std::string tried;
    std::unique_ptr<Source> best;
    size_t best_rows = 0;
    for (Layout& l : layouts) {
        if (!s) {
            s = std::make_unique<Source>();
            s->base = best->base, s->variant = best->variant, s->manifest_file = file, s->packages = packages;
        }
        s->m = std::move(l.m);
        if (s->m.package_count == 0 || s->m.package_count > 4096) {
            tried += std::string(tried.empty() ? "" : "; ") + l.name + ": bad package count";
            continue;
        }
        std::vector<uint64_t> sizes;
        std::string missing;
        for (uint32_t i = 0; i < s->m.package_count && missing.empty(); ++i) {
            std::error_code ec;
            const uint64_t n = fs::file_size(packages / (s->base + "_" + std::to_string(i)), ec);
            if (ec) missing = "package " + s->base + "_" + std::to_string(i) + " missing";
            sizes.push_back(n);
        }
        if (!missing.empty()) {
            tried += std::string(tried.empty() ? "" : "; ") + l.name + ": " + missing;
            continue;
        }
        for (HANDLE h : s->parts)
            if (h && h != INVALID_HANDLE_VALUE) o_CloseHandle(h);
        s->parts.assign(sizes.size(), nullptr);
        std::string w;
        const size_t rows = CheckLayout(*s, sizes, w);
        tried += std::string(tried.empty() ? "" : "; ") + l.name + ": " + w;
        if (rows > best_rows) {
            best_rows = rows;
            how = std::string(l.name) + ", " + w;
            best = std::move(s);   // the next layout gets a fresh Source
        }
    }
    if (best) return best;
    why = tried;
    return nullptr;
}

void BuildTree(State& st) {
    // per base: sources in fallback order
    std::map<std::string, std::vector<uint16_t>> by_base;
    for (uint16_t i = 0; i < st.sources.size(); ++i) by_base[st.sources[i]->base].push_back(i);
    for (auto& [name, srcs] : by_base) {
        Base b;
        b.name = Wide(name);
        std::unordered_map<std::wstring, uint32_t> files;   // "bucket\file" -> index
        for (uint16_t si : srcs) {
            const echo::dist::Manifest& m = st.sources[si]->m;
            // later rows win, as in the corpus: walk backwards and keep the first seen per key
            std::unordered_map<uint64_t, std::unordered_map<uint64_t, bool>> taken;
            for (uint32_t row = uint32_t(m.resources.size()); row-- > 0;) {
                const auto& r = m.resources[row];
                bool& t = taken[r.type_hash][r.resource_hash];
                if (t) continue;
                t = true;
                const std::wstring bucket = Wide(echo::dist::corpus_bucket_name(r.type_hash));
                const std::wstring file = Wide(echo::dist::corpus_file_name(r.resource_hash));
                const std::wstring bl = Lower(bucket), fl = Lower(file);
                const std::wstring key = bl + L"\\" + fl;
                auto it = files.find(key);
                if (it == files.end()) {
                    uint32_t ki;
                    auto bi = b.by_bucket.find(bl);
                    if (bi == b.by_bucket.end()) {
                        ki = uint32_t(b.buckets.size());
                        b.buckets.push_back({bucket, {}, {}});
                        b.by_bucket[bl] = ki;
                    } else {
                        ki = bi->second;
                    }
                    const uint32_t fi = uint32_t(b.files.size());
                    b.files.push_back({file, {}});
                    b.buckets[ki].files.push_back(fi);
                    b.buckets[ki].by_name[fl] = fi;
                    it = files.emplace(key, fi).first;
                }
                b.files[it->second].entries.push_back({si, row});
            }
        }
        st.by_base[Lower(b.name)] = uint32_t(st.bases.size());
        st.bases.push_back(std::move(b));
    }
}

}  // namespace

bool Mount(const std::vector<fs::path>& installs, const fs::path& root) {
    InstallHooks();
    State st;
    st.root = fs::absolute(root).wstring();
    while (st.root.size() > 3 && (st.root.back() == L'\\' || st.root.back() == L'/')) st.root.pop_back();
    st.root_lower = Lower(st.root);
    st.installs = installs;
    for (const fs::path& install : installs) {
        const fs::path dir = DataDir(install);
        if (dir.empty()) {
            st.reports.push_back({install.u8string().size() ? std::string(reinterpret_cast<const char*>(install.u8string().c_str())) + ": no game data" : "?", false});
            continue;
        }
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir / "manifests", ec))
            if (e.is_regular_file(ec)) files.push_back(e.path());
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            const std::string va = a.filename().string(), vb = b.filename().string();
            const auto dot = [](const std::string& s) { const size_t d = s.find('.'); return d == std::string::npos ? std::string() : s.substr(d); };
            const int ra = VariantRank(dot(va)), rb = VariantRank(dot(vb));
            if (ra != rb) return ra < rb;
            std::error_code e1, e2;
            return fs::last_write_time(a, e1) > fs::last_write_time(b, e2);
        });
        for (const fs::path& f : files) {
            std::string why, how;
            auto s = OpenSource(f, dir / "packages", why, how);
            const std::string label = install.filename().string() + "\\" + f.filename().string();
            if (!s) {
                st.reports.push_back({label + ": skipped, " + why, false});
                continue;
            }
            st.reports.push_back({label + ": ok (" + how + "), " + std::to_string(s->m.resources.size()) + " files", true});
            if (std::find(st.variants.begin(), st.variants.end(), s->variant) == st.variants.end()) st.variants.push_back(s->variant);
            if (st.sources.size() < 65535) st.sources.push_back(std::move(s));
        }
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (st.time.dwHighDateTime == 0 && o_GetFileAttributesExW((dir / "manifests").c_str(), GetFileExInfoStandard, &a)) st.time = a.ftLastWriteTime;
    }
    BuildTree(st);
    size_t best = 0;
    for (const Base& b : st.bases)
        if (b.files.size() > best) best = b.files.size(), st.corpus = fs::path(st.root) / b.name;
    if (installs.empty()) st.why = "Echo VR was not found. Choose its folder (the one with _data and bin).";
    else if (st.sources.empty()) st.why = "No readable game data in " + installs.front().string() + ".";
    if (!g_hook_error.empty()) st.why += (st.why.empty() ? "" : " ") + std::string("File hooks: ") + g_hook_error;
    st.preferred = st.variants.empty() ? std::string() : st.variants.front();
    {
        std::unique_lock<std::shared_mutex> l(g_mu);
        st.generation = g_st.generation + 1;
        g_st = std::move(st);
    }
    {
        std::lock_guard<std::mutex> l(g_cache_mu);
        g_cache.clear();
        g_lru.clear();
    }
    return !g_st.sources.empty() && g_hook_error.empty();
}

fs::path Corpus() {
    std::shared_lock<std::shared_mutex> l(g_mu);
    return g_st.corpus;
}
std::string Why() {
    std::shared_lock<std::shared_mutex> l(g_mu);
    return g_st.why;
}
std::vector<Report> Reports() {
    std::shared_lock<std::shared_mutex> l(g_mu);
    return g_st.reports;
}
std::vector<fs::path> Installs() {
    std::shared_lock<std::shared_mutex> l(g_mu);
    return g_st.installs;
}
std::vector<std::string> Variants() {
    std::shared_lock<std::shared_mutex> l(g_mu);
    return g_st.variants;
}
std::string Preferred() {
    std::shared_lock<std::shared_mutex> l(g_mu);
    return g_st.preferred;
}
void Prefer(const std::string& variant) {
    std::unique_lock<std::shared_mutex> l(g_mu);
    g_st.preferred = variant;
    for (Base& b : g_st.bases)
        for (FileNode& f : b.files)
            std::stable_partition(f.entries.begin(), f.entries.end(), [&](const Entry& e) { return g_st.sources[e.src]->variant == variant; });
}
uint64_t FilesServed() { return g_served; }
uint64_t ReadFailures() { return g_failures; }

}  // namespace gamefiles
}  // namespace erv
