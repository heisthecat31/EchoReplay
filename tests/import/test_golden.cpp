// P5 (DESIGN-FORMATS §11.2): the composed converter on the two MIT samples, end to end through the CLI a
// user runs: convert, `echodemo verify`, decode through the viewer's reader (`rows`), determinism,
// committed digests, and the §5.4 statistical chirality. Plus the verify split R2 left untested: a
// native capture's gap still fails strict verify while an imported file's gaps are listed as known.

#include "doctest.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <ratio>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#include "import/convert.h"
#include "import/reader.h"

using namespace replay::import;

namespace {

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir(buf);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

struct Run {
    int rc = -1;
    std::string out;
};

Run Tool(const std::wstring& args) {
    Run r;
    const std::wstring cmd = L"\"\"" + ExeDir() + L"echodemo.exe\" " + args + L" 2>&1\"";
    FILE* p = _wpopen(cmd.c_str(), L"rb");
    if (!p) return r;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) r.out.append(buf, n);
    r.rc = _pclose(p);
    return r;
}

constexpr int kMsPerS = std::milli::den;

std::wstring Q(const std::wstring& path) { return L"\"" + path + L"\""; }

std::wstring TempPath(const wchar_t* stem) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"import_golden_" + std::to_wstring(GetCurrentProcessId()) + L"_" + stem;
}

struct TempOutput {
    std::wstring path;
    explicit TempOutput(const wchar_t* stem) : path(TempPath(stem)) {}
    ~TempOutput() {
        DeleteFileW(path.c_str());
        DeleteFileW((path + L".import.json").c_str());
    }
};

std::string ReadAll(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string Sha256Hex(const std::string& bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    unsigned char digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), ULONG(bytes.size()),
                   digest, sizeof digest);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return hex;
}

std::wstring DataFile(const char* name) {
    const std::string dir = IMPORT_TEST_DATA_DIR;
    return std::wstring(dir.begin(), dir.end()) + L"/" + std::wstring(name, name + strlen(name));
}

// The committed digests: `<sample file> <converter version> <sha256 of its output>` per line. Lines
// are only ever appended, one per sample per version, so the file is the output's version history.
std::map<std::string, std::multimap<uint32_t, std::string>> GoldenDigests() {
    std::map<std::string, std::multimap<uint32_t, std::string>> out;
    std::ifstream f(DataFile("golden_digests.txt"));
    std::string name, digest;
    uint32_t version = 0;
    while (f >> name >> version >> digest) out[name].emplace(version, digest);
    return out;
}

struct Csv {
    std::vector<std::string> header;
    std::vector<std::vector<double>> rows;
    size_t Col(const char* name) const {
        const auto it = std::find(header.begin(), header.end(), name);
        REQUIRE_MESSAGE(it != header.end(), name);
        return size_t(it - header.begin());
    }
};

// Numeric cells only; the text `src` column parses as 0 and is not read.
Csv Rows(const std::wstring& file, const wchar_t* what) {
    const Run r = Tool(L"rows " + Q(file) + L" --what " + what);
    REQUIRE_MESSAGE(r.rc == 0, r.out);
    Csv t;
    std::istringstream in(r.out);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("ROWS ", 0) == 0) continue;
        std::vector<std::string> cells;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) cells.push_back(c);
        if (t.header.empty()) {
            t.header = cells;
            continue;
        }
        std::vector<double> row;
        for (const std::string& cell : cells) row.push_back(strtod(cell.c_str(), nullptr));
        t.rows.push_back(std::move(row));
    }
    return t;
}

struct Chirality {
    double upMedian = 0, forwardMean = 0, leftFraction = 0;
    size_t moving = 0;
};

void Rotate(const double q[4], const double v[3], double out[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// DESIGN-FORMATS §5.4, from decoded rows only. H2/H3 put the API head basis on the SimState body
// orientation and a yaw-only frame on hmdOri, so up and forward are read from the player bodies
// (idTag 0) and left from the hand offsets against the hmd frame.
Chirality Measure(const Csv& players, const Csv& bodies) {
    const double X[3] = {1, 0, 0}, Y[3] = {0, 1, 0}, Z[3] = {0, 0, 1};
    Chirality c;
    std::vector<double> ups;
    double forward = 0;
    const size_t tag = bodies.Col("idTag"), ox = bodies.Col("oriX"), vx = bodies.Col("velX");
    for (const std::vector<double>& row : bodies.rows) {
        if (row[tag] != 0) continue;
        const double q[4] = {row[ox], row[ox + 1], row[ox + 2], row[ox + 3]};
        double ay[3], az[3];
        Rotate(q, Y, ay);
        Rotate(q, Z, az);
        ups.push_back(ay[1]);
        const double v[3] = {row[vx], row[vx + 1], row[vx + 2]};
        const double speed = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (speed > 2.0) {
            forward += (az[0] * v[0] + az[1] * v[1] + az[2] * v[2]) / speed;
            ++c.moving;
        }
    }
    if (!ups.empty()) {
        std::nth_element(ups.begin(), ups.begin() + ups.size() / 2, ups.end());
        c.upMedian = ups[ups.size() / 2];
    }
    c.forwardMean = c.moving ? forward / double(c.moving) : 0;

    const size_t qx = players.Col("hmdX"), lx = players.Col("lOffX"), rx = players.Col("rOffX");
    size_t left = 0, both = 0;
    for (const std::vector<double>& row : players.rows) {
        const double q[4] = {row[qx], row[qx + 1], row[qx + 2], row[qx + 3]};
        double ax[3];
        Rotate(q, X, ax);
        const double l = row[lx] * ax[0] + row[lx + 1] * ax[1] + row[lx + 2] * ax[2];
        const double r = row[rx] * ax[0] + row[rx + 1] * ax[1] + row[rx + 2] * ax[2];
        if (l != r) {
            ++both;
            if (l > r) ++left;
        }
    }
    c.leftFraction = both ? double(left) / double(both) : 0;
    return c;
}

// The same forward statistic on the source's own head basis, before any conversion.
double SourceForward(const std::wstring& path) {
    SourceMatch m;
    REQUIRE(ReadSource(path.c_str(), m, nullptr).ok);
    const double Z[3] = {0, 0, 1};
    double sum = 0;
    size_t n = 0;
    for (const SrcPlayer& p : m.players) {
        const double speed = sqrt(double(p.vel[0]) * p.vel[0] + double(p.vel[1]) * p.vel[1] + double(p.vel[2]) * p.vel[2]);
        if (speed <= 2.0) continue;
        const double q[4] = {p.head.q[0], p.head.q[1], p.head.q[2], p.head.q[3]};
        double az[3];
        Rotate(q, Z, az);
        sum += (az[0] * p.vel[0] + az[1] * p.vel[1] + az[2] * p.vel[2]) / speed;
        ++n;
    }
    return n ? sum / double(n) : 0;
}

double DurationS(const Csv& t) {
    const size_t tu = t.Col("tUs");
    double lo = 1e300, hi = -1e300;
    for (const std::vector<double>& row : t.rows) {
        lo = std::min(lo, row[tu]);
        hi = std::max(hi, row[tu]);
    }
    return (hi - lo) / 1e6;
}

size_t Slots(const Csv& t) {
    std::set<int> slots;
    const size_t s = t.Col("slot");
    for (const std::vector<double>& row : t.rows) slots.insert(int(row[s]));
    return slots.size();
}

// A plain-text Spark file: one blue player moving +x at 1 m/s, 30 Hz, with nothing between 2 and 3 s.
std::wstring WriteGappedEchoReplay(const wchar_t* stem) {
    const std::wstring path = TempPath(stem) + L".echoreplay";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0) f = nullptr;
    REQUIRE(f != nullptr);
    const char* basis = "\"forward\":[0,0,1],\"up\":[0,1,0],\"left\":[1,0,0]";
    for (int k = 0; k <= 150; ++k) {
        if (k > 60 && k < 90) continue;
        const double t = k / 30.0;
        const int ms = int(llround(t * kMsPerS));
        fprintf(f,
                "2025-08-03 12:00:%02d.%03d\t{\"map_name\":\"mpl_arena_a\",\"sessionid\":\"S\",\"private_match\":true,"
                "\"game_status\":\"playing\",\"game_clock\":%.3f,\"blue_points\":0,\"orange_points\":0,"
                "\"disc\":{\"position\":[0,0,0],\"velocity\":[0,0,0],%s},\"teams\":[{\"team\":\"BLUE TEAM\","
                "\"players\":[{\"name\":\"p\",\"userid\":7,\"playerid\":0,\"number\":1,\"level\":1,"
                "\"head\":{\"position\":[%.3f,1.6,0],%s},\"body\":{\"position\":[%.3f,1.3,0],%s},"
                "\"lhand\":{\"pos\":[%.3f,1.2,0.2],%s},\"rhand\":{\"pos\":[%.3f,1.2,0.2],%s},"
                "\"velocity\":[1,0,0],\"stats\":{}}]},{\"team\":\"ORANGE TEAM\",\"players\":[]}]}\n",
                ms / kMsPerS, ms % kMsPerS, 300.0 - t, basis, t, basis, t, basis, t + 0.3, basis, t - 0.3, basis);
    }
    fclose(f);
    return path;
}

}  // namespace

TEST_CASE("P5 golden: both MIT samples convert, pass verify, decode, repeat byte for byte and match their digests") {
    const std::map<std::string, std::multimap<uint32_t, std::string>> golden = GoldenDigests();
    std::map<std::string, Csv> decoded;
    for (const char* sample : {"sample.echoreplay", "sample.tape"}) {
        const std::string name = sample;
        CAPTURE(name);
        TempOutput a(L"a.echodemo"), b(L"b.echodemo");
        ConvertReport report;
        Progress progress;
        const Result r = Convert(DataFile(sample).c_str(), a.path.c_str(), ConvertOptions{}, report, &progress);
        REQUIRE_MESSAGE(r.ok, r.message);
        CHECK(progress.permille.load() == std::milli::den);
        CHECK(report.sourceHash != 0);
        CHECK(report.ticks > 0);

        const Run verify = Tool(L"verify " + Q(a.path));
        CHECK_MESSAGE(verify.rc == 0, verify.out);
        const Run info = Tool(L"info " + Q(a.path));
        CHECK(info.out.find("IMPORTED") != std::string::npos);

        const std::string sidecar = ReadAll(a.path + L".import.json");
        CHECK(sidecar.find("\"converterVersion\"") != std::string::npos);
        CHECK(sidecar.find("\"requires_allow_gaps\"") != std::string::npos);

        ConvertReport again;
        REQUIRE(Convert(DataFile(sample).c_str(), b.path.c_str(), ConvertOptions{}, again, nullptr).ok);
        const std::string bytes = ReadAll(a.path);
        CHECK(bytes == ReadAll(b.path));
        const std::string digest = Sha256Hex(bytes);
        const auto want = golden.find(sample);
        REQUIRE_MESSAGE(want != golden.end(), "no committed digest; this output is ", digest);
        // One line per version from 1 to kConverterVersion, each a different output: re-recording a
        // digest under the current version, or bumping without a change, breaks the history.
        const std::multimap<uint32_t, std::string>& history = want->second;
        std::set<uint32_t> versions;
        std::set<std::string> outputs;
        for (const auto& [version, recorded] : history) {
            versions.insert(version);
            outputs.insert(recorded);
        }
        CHECK(versions.size() == history.size());
        CHECK(outputs.size() == history.size());
        CHECK(*versions.begin() == 1);
        CHECK(*versions.rbegin() == kConverterVersion);
        CHECK(history.size() == kConverterVersion);
        const auto current = history.find(kConverterVersion);
        CHECK_MESSAGE((current != history.end() && current->second == digest),
                      "output bytes changed: bump kConverterVersion to ", kConverterVersion + 1, " and append `",
                      sample, " ", kConverterVersion + 1, " ", digest, "`; never edit a recorded line");

        const Csv rows = Rows(a.path, L"player");
        CHECK(Slots(rows) == 2);
        CHECK(DurationS(rows) == doctest::Approx(63.3).epsilon(0.03));
        const Chirality c = Measure(rows, Rows(a.path, L"body"));
        MESSAGE(name, " chirality: up ", c.upMedian, " forward ", c.forwardMean, " over ", c.moving, " left ",
                c.leftFraction);
        CHECK(c.upMedian > 0.8);
        CHECK(c.moving > 100);
        // The §5.4 absolute bar (0.3) is set on the owner files and applied by the local gate; this short
        // two-player sample sits lower at its source, so here the output must carry the source's value.
        CHECK(c.forwardMean > 0.15);
        CHECK(c.forwardMean == doctest::Approx(SourceForward(DataFile(sample))).epsilon(0.1));
        CHECK(c.leftFraction >= 0.9);
        decoded[sample] = rows;
    }
    // The same match from two sources: the listed differences (tape stats are event-based, number and
    // level are sourced differently) do not move poses, so the decoded tables agree on shape and span.
    const Csv& api = decoded["sample.echoreplay"];
    const Csv& tape = decoded["sample.tape"];
    CHECK(DurationS(api) == doctest::Approx(DurationS(tape)).epsilon(0.02));
    CHECK(double(api.rows.size()) == doctest::Approx(double(tape.rows.size())).epsilon(0.03));
}

TEST_CASE("P5 verify: a native capture's gap fails strict verify; an imported file's gaps are known") {
    TempOutput native(L"native.echodemo");
    const std::wstring mk = L"\"\"" + ExeDir() + L"capture_mkdemo.exe\" " + Q(native.path) +
                            L" --frames=900 --players=2 --drop-marks=3 2>&1\"";
    FILE* p = _wpopen(mk.c_str(), L"rb");
    REQUIRE(p != nullptr);
    char sink[4096];
    while (fread(sink, 1, sizeof sink, p) > 0) {
    }
    REQUIRE(_pclose(p) == 0);

    const Run strict = Tool(L"verify " + Q(native.path));
    CHECK(strict.rc != 0);
    CHECK_MESSAGE(strict.out.find("FAIL:") != std::string::npos, strict.out);
    CHECK(strict.out.find("known import gap") == std::string::npos);
    const Run allowed = Tool(L"verify " + Q(native.path) + L" --allow-gaps");
    CHECK_MESSAGE(allowed.rc == 0, allowed.out);

    const std::wstring source = WriteGappedEchoReplay(L"gapped");
    TempOutput imported(L"imported.echodemo");
    ConvertReport report;
    const Result r = Convert(source.c_str(), imported.path.c_str(), ConvertOptions{}, report, nullptr);
    DeleteFileW(source.c_str());
    REQUIRE_MESSAGE(r.ok, r.message);
    REQUIRE(report.gapList.size() == 1);
    CHECK(report.gapList[0].atUs == doctest::Approx(2000000).epsilon(0.02));
    CHECK(report.gapList[0].lengthUs == doctest::Approx(1000000).epsilon(0.02));
    CHECK(ReadAll(imported.path + L".import.json").find("\"lengthS\"") != std::string::npos);

    const Run listed = Tool(L"verify " + Q(imported.path));
    CHECK_MESSAGE(listed.rc == 0, listed.out);
    CHECK_MESSAGE(listed.out.find("known import gap") != std::string::npos, listed.out);
    CHECK_MESSAGE(listed.out.find("FAIL:") == std::string::npos, listed.out);
}

TEST_CASE("P5 convert refuses with a cause and leaves no output behind") {
    TempOutput out(L"refused.echodemo");
    ConvertReport report;
    CHECK(Convert(L"x.butter", out.path.c_str(), ConvertOptions{}, report, nullptr).code == ResultCode::BadArguments);
    CHECK(Convert(nullptr, out.path.c_str(), ConvertOptions{}, report, nullptr).code == ResultCode::BadArguments);
    const Result missing = Convert(TempPath(L"absent.tape").c_str(), out.path.c_str(), ConvertOptions{}, report, nullptr);
    CHECK_FALSE(missing.ok);
    CHECK(ExitCodeFor(missing) == 3);

    Progress cancelled;
    cancelled.cancel.store(true);
    const Result c = Convert(DataFile("sample.tape").c_str(), out.path.c_str(), ConvertOptions{}, report, &cancelled);
    CHECK(c.code == ResultCode::Cancelled);
    CHECK(GetFileAttributesW(out.path.c_str()) == INVALID_FILE_ATTRIBUTES);

    ConvertOptions badRate;
    badRate.rateHz = 0;
    CHECK(Convert(DataFile("sample.tape").c_str(), out.path.c_str(), badRate, report, nullptr).code ==
          ResultCode::BadArguments);
    CHECK(GetFileAttributesW(out.path.c_str()) == INVALID_FILE_ATTRIBUTES);

    const Run usage = Tool(L"convert onlyone.tape");
    CHECK(usage.rc == 2);
}

TEST_CASE("M2: a line over 1 MB is refused before its JSON is parsed") {
    TempOutput big(L"long_line.echoreplay");
    {
        std::ofstream f(big.path, std::ios::binary);
        f << "2026/01/01 00:00:00.000\t{\"x\":\"" << std::string(size_t(1) << 20, 'a') << "\"}\n";
    }
    SourceMatch m;
    const Result r = ReadSource(big.path.c_str(), m, nullptr);
    CHECK(r.code == ResultCode::Corrupt);
    CHECK_MESSAGE(std::string(r.message).find("longer than 1 MB") != std::string::npos, r.message);
}

TEST_CASE("M2: past the model budget a conversion is refused with a sentence and leaves nothing") {
    for (const char* sample : {"sample.echoreplay", "sample.tape"}) {
        CAPTURE(sample);
        TempOutput out(L"budget.echodemo");
        ConvertOptions options;
        options.maxModelBytes = 1;
        ConvertReport report;
        const Result r = Convert(DataFile(sample).c_str(), out.path.c_str(), options, report, nullptr);
        CHECK(r.code == ResultCode::TooLarge);
        CHECK_MESSAGE(std::string(r.message).find("echodemo convert") != std::string::npos, r.message);
        CHECK(::GetFileAttributesW(out.path.c_str()) == INVALID_FILE_ATTRIBUTES);

        options.maxModelBytes = uint64_t(160) << 20;
        CHECK(Convert(DataFile(sample).c_str(), out.path.c_str(), options, report, nullptr).ok);
    }
}
