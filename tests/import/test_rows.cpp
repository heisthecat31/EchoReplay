#include "doctest.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

// `echodemo rows` against a capture the REAL ChapterBuilder wrote (capture_mkdemo), whose generator
// states every identity field: wire player p carries hdr0 0xA000000p, holds slot p with generation 1
// in the left hand, nothing in the right, anchor p % 8, and a fixed right-hand quaternion; wire body p
// carries hdr0 0xB000000p, a component-id tag and bits11 = p. A decode that splits a header word the
// wrong way, reads a field at the wrong offset or keys a row to the wrong sender fails a row here.

namespace {

std::wstring SiblingExe(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir(buf);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1) + name;
}

struct Run {
    int rc = -1;
    std::string out;
};

// stderr is folded in: the census line is the last thing the tool prints.
Run Capture(const std::wstring& cmdline) {
    Run r;
    const std::wstring cmd = L"\"" + cmdline + L" 2>&1\"";
    FILE* p = _wpopen(cmd.c_str(), L"rb");
    if (!p) return r;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) r.out.append(buf, n);
    r.rc = _pclose(p);
    return r;
}

struct Table {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
    std::string census;

    size_t Col(const char* name) const {
        for (size_t i = 0; i < header.size(); ++i) {
            if (header[i] == name) return i;
        }
        FAIL("no column ", name);
        return 0;
    }
    double Num(size_t row, const char* name) const { return strtod(rows[row][Col(name)].c_str(), nullptr); }
    unsigned long long Int(size_t row, const char* name) const {
        return strtoull(rows[row][Col(name)].c_str(), nullptr, 0);
    }
};

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> cells;
    size_t start = 0;
    for (;;) {
        const size_t comma = line.find(',', start);
        cells.push_back(line.substr(start, comma - start));
        if (comma == std::string::npos) return cells;
        start = comma + 1;
    }
}

Table Parse(const std::string& text) {
    Table t;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;
        if (line.rfind("ROWS ", 0) == 0) t.census = line;
        else if (t.header.empty()) t.header = Split(line);
        else if (!line.empty()) t.rows.push_back(Split(line));
    }
    return t;
}

struct Fixture {
    std::wstring file;
    std::wstring tool = SiblingExe(L"echodemo.exe");

    Fixture() {
        wchar_t dir[MAX_PATH];
        GetTempPathW(MAX_PATH, dir);
        file = std::wstring(dir) + L"import_rows_" + std::to_wstring(GetCurrentProcessId()) + L".echodemo";
        const Run mk = Capture(L"\"" + SiblingExe(L"capture_mkdemo.exe") + L"\" \"" + file +
                               L"\" --frames=600 --players=4");
        REQUIRE_MESSAGE(mk.rc == 0, mk.out);
    }
    ~Fixture() { DeleteFileW(file.c_str()); }

    Table Rows(const wchar_t* what, const wchar_t* extra = L"") const {
        const Run r = Capture(L"\"" + tool + L"\" rows \"" + file + L"\" --what " + what + L" " + extra);
        Table t = Parse(r.out);
        REQUIRE_MESSAGE(r.rc == 0, t.census);
        REQUIRE_MESSAGE(t.census.find("decode_failures=0 blocks_bad=0 chapter_faults=0") != std::string::npos,
                        t.census);
        REQUIRE(t.census == "ROWS " + std::to_string(t.rows.size()) + t.census.substr(t.census.find(' ', 5)));
        return t;
    }
};

float Normalised(float c, float x, float y, float z, float w) { return c / sqrtf(x * x + y * y + z * z + w * w); }

}  // namespace

TEST_CASE("rows decodes wire players with the identity the writer gave them") {
    Fixture fx;
    const Table t = fx.Rows(L"player");
    REQUIRE(t.rows.size() > 100);
    std::map<unsigned, unsigned long long> lastT;
    const float rq[4] = {Normalised(-0.3f, -0.3f, 0.5f, 0.1f, 0.7f), Normalised(0.5f, -0.3f, 0.5f, 0.1f, 0.7f),
                         Normalised(0.1f, -0.3f, 0.5f, 0.1f, 0.7f), Normalised(0.7f, -0.3f, 0.5f, 0.1f, 0.7f)};
    size_t wire = 0;
    for (size_t i = 0; i < t.rows.size(); ++i) {
        // Sampled rows are the recorder's own pawns and carry no header; only wire rows name a sender.
        if (t.rows[i][t.Col("src")] != "wire") continue;
        ++wire;
        const unsigned slot = unsigned(t.Int(i, "slot"));
        CHECK(slot < 4);
        CHECK(t.Int(i, "gen") == 0xA000);
        CHECK(t.Int(i, "holdLSlot") == slot);
        CHECK(t.Int(i, "holdLGen") == 1);
        CHECK(t.Int(i, "holdRSlot") == UINT16_MAX);  // the writer's empty hand
        CHECK(t.Int(i, "anchor") == slot % 8);
        CHECK(fabs(t.Num(i, "rqX") - rq[0]) < 2e-3);
        CHECK(fabs(t.Num(i, "rqY") - rq[1]) < 2e-3);
        CHECK(fabs(t.Num(i, "rqZ") - rq[2]) < 2e-3);
        CHECK(fabs(t.Num(i, "rqW") - rq[3]) < 2e-3);
        const unsigned long long bits = t.Int(i, "bits28");
        CHECK((bits & 0xFFF) == (bits >> 16));  // the writer's word is k * 0x00010001
        const unsigned long long tUs = t.Int(i, "tUs");
        CHECK(tUs >= lastT[slot]);
        lastT[slot] = tUs;
    }
    CHECK(lastT.size() == 4);
    CHECK(wire > 100);
}

TEST_CASE("rows splits a wire body's tagged id and its sender") {
    Fixture fx;
    const Table t = fx.Rows(L"body");
    size_t wire = 0;
    for (size_t i = 0; i < t.rows.size(); ++i) {
        if (t.rows[i][t.Col("src")] != "wire") continue;
        ++wire;
        const unsigned long long id = t.Int(i, "idValue");
        CHECK(t.Int(i, "idTag") == 1);
        CHECK(id < 4);
        CHECK(t.Int(i, "owner") == (0xB0000000ull | id));
        CHECK(t.Int(i, "bits11") == id);
    }
    CHECK(wire > 100);
}

TEST_CASE("rows places events inside their chapter and names the netvars") {
    Fixture fx;
    const Table ev = fx.Rows(L"event");
    REQUIRE(ev.rows.size() == 6);  // the six lobby bodies the writer feeds on frame 1
    for (size_t i = 0; i < ev.rows.size(); ++i) {
        CHECK(ev.rows[i][ev.Col("tap")].empty());  // none of the four decoded feeds
        CHECK(ev.Int(i, "bytes") > 0);
        CHECK(ev.Int(i, "tUs") >= ev.Int(i, "stampUs"));
    }

    const Table nv = fx.Rows(L"netvar");
    size_t timers = 0;
    for (size_t i = 0; i < nv.rows.size(); ++i) {
        if (nv.rows[i][nv.Col("name")] != "game_timer") continue;
        ++timers;
        CHECK(nv.Int(i, "hash") == 0xB2996AD484F1D1E8ull);
        CHECK(nv.Num(i, "value") >= 0.0);
        CHECK(nv.Num(i, "value") <= 300.0);
    }
    CHECK(timers > 0);
}

TEST_CASE("rows filters by capture time and refuses an unknown kind") {
    Fixture fx;
    const Table all = fx.Rows(L"player");
    const unsigned long long first = all.Int(0, "tUs");
    const double fromS = double(first) / 1e6 + 5.0;
    const Table late = fx.Rows(L"player", (L"--from " + std::to_wstring(fromS)).c_str());
    CHECK(late.rows.size() > 0);
    CHECK(late.rows.size() < all.rows.size());
    for (size_t i = 0; i < late.rows.size(); ++i) CHECK(double(late.Int(i, "tUs")) >= fromS * 1e6 - 1.0);

    const Run bad = Capture(L"\"" + fx.tool + L"\" rows \"" + fx.file + L"\" --what nonsense");
    CHECK(bad.rc == 2);
}
