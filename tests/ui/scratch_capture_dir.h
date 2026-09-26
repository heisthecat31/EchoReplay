#pragma once

// * A TEST MUST NEVER WRITE INTO THE OWNER'S REAL Documents\EchoReplay.
//
// 34 fixture files (clickme/a_here/b_far/c_bad plus f00..f29) have been found in the owner's own
// capture folder twice now, and both times for the same reason: the test asked the PRODUCT where
// captures live -- ui::ReplayDirectory(), i.e. replay::ResolveCaptureDir -- and wrote wherever it
// was told. That answer is the owner's Documents folder unless a redirect the test cannot see is
// both present and correct, so a stale binary, an unset redirect or a corrupted literal in the
// redirect arm all end the same way: fixtures in the owner's captures.
//
// The test decides the location here instead, and points the panel at THAT string through
// `capture.dir` -- the shipped config row a real install uses to move its captures. There is still
// exactly one path rule (ResolveCaptureDir's config arm); this is not a second one. Fixture asserts
// the two agree, so a drift goes RED instead of landing in Documents.

#include <windows.h>

#include <cstring>
#include <string>

#include "bootstrap/paths.h"
#include "bootstrap/services.h"

namespace testscratch {

// `<root>\EchoReplay` -- the same leaf every arm of ResolveCaptureDir builds.
inline std::wstring TestScratchCaptureDir(const wchar_t* root) {
    return std::wstring(root) + L"\\EchoReplay";
}

// Answers `capture.dir` with the scratch directory and every other key with the caller's default,
// so a fixture carrying this config behaves exactly like one carrying no config at all.
class ScratchConfig : public replay::IConfig {
public:
    explicit ScratchConfig(const std::wstring& dir) {
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr,
                                            nullptr);
        if (n > 1) {
            dir_.resize(static_cast<size_t>(n) - 1);
            ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, &dir_[0], n, nullptr, nullptr);
        }
    }

    bool GetBool(const char*, bool dflt) const override { return dflt; }
    int64_t GetInt(const char*, int64_t dflt) const override { return dflt; }
    double GetDouble(const char*, double dflt) const override { return dflt; }

    const char* GetString(const char* key, const char* dflt) const override {
        if (key != nullptr && std::strcmp(key, replay::kCaptureDirKey) == 0 && !dir_.empty()) {
            return dir_.c_str();
        }
        if (key != nullptr && std::strcmp(key, replay::kImportDirsKey) == 0 && importDirsSet) {
            return importDirs.c_str();
        }
        return dflt;
    }

    // `import.dirs`, answered only once a case sets it; unset it is the caller's default.
    std::string importDirs;
    bool importDirsSet = false;

    bool Has(const char* key) const override {
        return key != nullptr && std::strcmp(key, replay::kCaptureDirKey) == 0 && !dir_.empty();
    }

private:
    std::string dir_;
};

}  // namespace testscratch
