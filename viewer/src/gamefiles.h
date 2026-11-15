#pragma once
// Echo's own data, read in place: no extract, no copy.
//
// EchoConstruct's loaders read a "corpus" folder (<manifest>\<type bucket>\<resource>.bin). Here that folder
// is virtual: inside this process the Windows file calls (CreateFileW, ReadFile, FindFirstFileExW, ...) are
// answered for paths under the mount root straight from the game's packages: the manifest says which zstd
// frame of which package part holds a resource, the frame is read and decompressed, the resource sliced out.
// Nothing is written anywhere, and every other path goes to Windows untouched.
//
// Every install found and every manifest variant in it (the live one, .stock, .bak, .echocombat, ...) is a
// source. A source that does not parse, points outside its packages, or whose frames do not decompress is
// skipped (Reports() says why). A file is served from the first source that decodes it, else the next one;
// Prefer() moves a variant to the front (the level loader uses it when a map fails with the current order).

#include <stdint.h>

#include <filesystem>
#include <string>
#include <vector>

namespace erv {

// A folder holding _data\<id>\rad15\win10\{manifests,packages}.
bool IsEchoInstall(const std::filesystem::path&);
// The install itself when given one of its subfolders (bin\win10, _data, ...), else empty.
std::filesystem::path EchoInstallFor(const std::filesystem::path&);
// Installs on the fixed drives: drive roots, their top folders, Oculus library paths.
std::vector<std::filesystem::path> FindEchoInstalls();

namespace gamefiles {

struct Report {
    std::string line;
    bool ok = false;
};

// Mounts the installs' data at `root` (a path that need not exist). Safe to call again.
bool Mount(const std::vector<std::filesystem::path>& installs, const std::filesystem::path& root);
std::filesystem::path Corpus();   // the folder EchoConstruct opens: <root>\<largest manifest>
std::string Why();
std::vector<Report> Reports();
std::vector<std::filesystem::path> Installs();

std::vector<std::string> Variants();   // "" (live), ".stock", ".bak", ... in fallback order
void Prefer(const std::string& variant);
std::string Preferred();

uint64_t FilesServed();
uint64_t ReadFailures();

}  // namespace gamefiles
}  // namespace erv
