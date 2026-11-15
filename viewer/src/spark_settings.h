// What the viewer takes from Spark's settings (%APPDATA%\IgniteVR\Spark\settings.json), when Spark is installed:
//   echoVRPath  ...\bin\win10\echovr.exe   -> the Echo VR install to read maps from
//   saveFolder  Spark's replay folder       -> the library folder (Spark points EchoReplay's recordings there)
// Read once, read-only. A missing file or key just means "no answer"; the viewer's own search still runs.
#pragma once

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace spark_settings {

inline std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

inline std::filesystem::path File() {
    const std::filesystem::path roaming = KnownFolder(FOLDERID_RoamingAppData);
    return roaming.empty() ? roaming : roaming / "IgniteVR" / "Spark" / "settings.json";
}

inline std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// The string value of a top-level key ("" when it is missing or not a string). Spark writes a flat object, so the
// first "key": "..." pair is the one.
inline std::wstring String(const char* key) {
    std::ifstream f(File(), std::ios::binary);
    if (!f) return {};
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string quoted = std::string("\"") + key + "\"";
    size_t i = text.find(quoted);
    if (i == std::string::npos) return {};
    i += quoted.size();
    auto ws = [&] { while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i; };
    ws();
    if (i >= text.size() || text[i] != ':') return {};
    ++i;
    ws();
    if (i >= text.size() || text[i] != '"') return {};
    ++i;
    std::string out;
    while (i < text.size() && text[i] != '"') {
        char c = text[i++];
        if (c != '\\' || i >= text.size()) {
            out += c;
            continue;
        }
        c = text[i++];
        switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > text.size()) return {};
                const unsigned cp = unsigned(std::stoul(text.substr(i, 4), nullptr, 16));
                i += 4;
                // UTF-8 for one BMP code point (paths don't need surrogate pairs in practice)
                if (cp < 0x80) out += char(cp);
                else if (cp < 0x800) out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
                else out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
                break;
            }
            default: out += c; break;   // \" \\ \/
        }
    }
    return Wide(out);
}

// echovr.exe as Spark knows it (empty when Spark isn't installed or the path isn't set).
inline std::filesystem::path EchoVRExe() { return String("echoVRPath"); }

// Spark's replay folder: saveFolder, or Spark's own default (Documents\Spark\replays) when it's "none". Empty when
// that folder doesn't exist.
inline std::filesystem::path ReplayFolder() {
    std::error_code ec;
    if (!std::filesystem::exists(File(), ec)) return {};
    std::filesystem::path p = String("saveFolder");
    if (p.empty() || p == L"none") p = KnownFolder(FOLDERID_Documents) / "Spark" / "replays";
    return std::filesystem::is_directory(p, ec) ? p : std::filesystem::path{};
}

}  // namespace spark_settings
