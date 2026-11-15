#include "level.h"
#include "gamefiles.h"
#include "scriptvm.h"

#include <windows.h>
#include "paths.h"
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>

#include <echo/bakes/bake.hpp>
#include <echo/bakes/bake_inputs.hpp>
#include <echo/importers/palette_index.hpp>
#include <echo/renderer/material_scene.hpp>
#include <echo/renderer/scene.hpp>
#include <ui/bake_job.hpp>
#include <ui/capture_cli.hpp>
#include <ui/controller.hpp>
#include <echo/renderer/canvas_raster.hpp>
#include <ui/canvas_set.hpp>
#include <ui/level_layers.hpp>
#include <ui/mesh_source.hpp>
#include <ui/session.hpp>
#include <ui/state_lighting.hpp>
#include <ui/canvas_rows.hpp>
#include <echo/spec-registry/registry.hpp>
#include <echo/distribution/install_detect.hpp>
#include <echo/scripting/stock_lift.hpp>
#include <echo/scripting/flow_facts.hpp>
#include <echo/scripting/script_module.hpp>
#include <ui/script_export.hpp>
#include <ui/script_instances.hpp>
#include <set>

namespace erv {
namespace {

std::filesystem::path LocalAppData() {
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::filesystem::path EchoConstructCorpus() {
    wchar_t* env = nullptr;
    size_t n = 0;
    if (_wdupenv_s(&env, &n, L"ECHO_CORPUS") == 0 && env && *env) {
        std::filesystem::path p = env;
        free(env);
        return p;
    }
    free(env);
    std::ifstream f(LocalAppData() / "EchoConstruct" / "echo_editor.ini");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("corpus=", 0) == 0) {
            std::string v = line.substr(7);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            const int len = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, nullptr, 0);
            std::wstring w(len > 0 ? len - 1 : 0, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, w.data(), len);
            return w;
        }
    }
    return {};
}

std::string Join(std::span<const echo::Diagnostic> d) {
    std::string s;
    for (const auto& x : d) s += x.code + " " + x.message + "; ";
    return s.empty() ? "unknown" : s;
}

// Box blur, three passes, in place (RGBA).
void Blur(Image& im, int radius) {
    std::vector<uint8_t> tmp(im.rgba.size());
    const int w = int(im.w), h = int(im.h);
    for (int pass = 0; pass < 3; ++pass) {
        for (int dir = 0; dir < 2; ++dir) {
            const int outer = dir ? w : h, inner = dir ? h : w;
            for (int o = 0; o < outer; ++o) {
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    auto px = [&](int i) -> uint8_t& {
                        i = std::clamp(i, 0, inner - 1);
                        const int x = dir ? o : i, y = dir ? i : o;
                        return im.rgba[(size_t(y) * w + x) * 4 + c];
                    };
                    for (int i = -radius; i <= radius; ++i) sum += px(i);
                    for (int i = 0; i < inner; ++i) {
                        const int x = dir ? o : i, y = dir ? i : o;
                        tmp[(size_t(y) * w + x) * 4 + c] = uint8_t(sum / (2 * radius + 1));
                        sum += px(i + radius + 1) - px(i - radius);
                    }
                }
            }
            im.rgba.swap(tmp);
        }
    }
}

Image Downscale(const echo::render::CapturedFrame& f, uint32_t w, uint32_t h) {
    Image im;
    im.w = w, im.h = h;
    im.rgba.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t x0 = x * f.width / w, x1 = std::max(x0 + 1, (x + 1) * f.width / w);
            const uint32_t y0 = y * f.height / h, y1 = std::max(y0 + 1, (y + 1) * f.height / h);
            uint32_t acc[4] = {}, n = 0;
            for (uint32_t yy = y0; yy < y1; ++yy)
                for (uint32_t xx = x0; xx < x1; ++xx, ++n)
                    for (int c = 0; c < 4; ++c) acc[c] += f.rgba[(size_t(yy) * f.width + xx) * 4 + c];
            for (int c = 0; c < 4; ++c) im.rgba[(size_t(y) * w + x) * 4 + c] = uint8_t(acc[c] / std::max(n, 1u));
        }
    return im;
}

bool WriteImage(const std::filesystem::path& p, const Image& im) {
    std::ofstream f(p, std::ios::binary);
    const uint32_t head[3] = {0x54565245u /*ERVT*/, im.w, im.h};
    f.write(reinterpret_cast<const char*>(head), sizeof head);
    f.write(reinterpret_cast<const char*>(im.rgba.data()), std::streamsize(im.rgba.size()));
    return bool(f);
}

}  // namespace

std::filesystem::path Levels::CacheDir() {
    std::filesystem::path d = LocalAppData() / "EchoReplayViewer" / "maps";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

bool Levels::ReadImage(const std::filesystem::path& p, Image& im) {
    std::ifstream f(p, std::ios::binary);
    uint32_t head[3] = {};
    if (!f.read(reinterpret_cast<char*>(head), sizeof head) || head[0] != 0x54565245u || head[1] > 8192 || head[2] > 8192) return false;
    im.w = head[1], im.h = head[2];
    im.rgba.resize(size_t(im.w) * im.h * 4);
    return bool(f.read(reinterpret_cast<char*>(im.rgba.data()), std::streamsize(im.rgba.size())));
}

bool Levels::Init(echo::render::PreviewRenderer* r, const std::filesystem::path& corpus) {
    r_ = r;
    corpus_ = corpus;
    current_.clear();
    std::error_code ec;
    if (corpus_.empty() || !std::filesystem::is_directory(corpus_, ec)) {
        why_ = gamefiles::Why().empty() ? "Echo VR's game data could not be read." : gamefiles::Why();
        return false;
    }
    why_.clear();
    return true;
}

// The live manifest first; when a map will not open with it, every other variant the game folder holds.
bool Levels::Load(const std::string& level) {
    if (level == current_) return true;
    if (!r_ || corpus_.empty()) return false;
    const std::string start = gamefiles::Preferred();
    if (TryLoad(level)) return variant_ = start, true;
    std::string first = why_;
    for (const std::string& v : gamefiles::Variants()) {
        if (v == start) continue;
        gamefiles::Prefer(v);
        if (TryLoad(level)) {
            variant_ = v;
            return true;
        }
    }
    gamefiles::Prefer(start);
    why_ = first;
    return false;
}

// The renderer reads the open level's sources after the load too: a sub-level layer's meshes decode through
// them (Preview::extend_library), so the session and its assets live as long as the level is shown.
struct Levels::Loaded {
    echo::ui::EditorSession session;
    std::optional<echo::ui::ViewportAssets> assets;
};
// The arena's screens (SetBoards). The stock canvases are laid out with their designer's placeholder text
// ("00", "PLAYER NAME", "WWWW", "HELLO WORLD"); the game writes the match into them at run time. Here a canvas is
// copied, its texts set from the replay, laid out again and swapped into the bound set; the rest keep their RTs.
struct Levels::Boards {
    using ActorIdT = std::decay_t<decltype(std::declval<echo::model::Document>().actors[0].id)>;
    std::shared_ptr<const echo::render::CanvasSet> base;
    echo::ui::CanvasCorpus* corpus = nullptr;
    std::map<uint32_t, uint64_t> ids;   // set canvas index -> its CCanvasUICR symbol
    struct Hide {
        ActorIdT id{};
        echo::render::Affine world, gone;
    };
    std::vector<Hide> postmatch;        // the end-of-match board, cards and rematch console: out until PostMatch
    int postmatch_shown = -1;
    struct Facing {
        float pos[3]{}, normal[3]{};            // where it is, its local z in the world
        float front = 1;                        // the sign of (eye - pos).normal from which it reads as placed
        int shown = -1;                         // 0 as placed, 1 turned
        // its text: a curved canvas strip drawn at its own transform (CanvasDraw::world), turned with the panel
        std::vector<size_t> draws;
        std::vector<echo::render::Mat4> draw_world, draw_turned;
    };
    std::shared_ptr<const echo::render::CanvasSet> current;   // the set last given to the renderer
    std::vector<Facing> goalboard;      // the goal ends' info boards (kGoalBoard)
    std::string last;                   // what the screens show now
    struct Laid {
        std::string key;
        std::shared_ptr<const echo::render::CanvasQuads> quads;
    };
    std::map<uint64_t, Laid> laid;      // per canvas index (cards: per card), so unchanged screens keep their RTs
    // Echo's own screen scripts (scriptvm.h), and which canvases each scripted actor hosts.
    script::Machine vm;
    std::string lobby;          // the roster the scripts last heard of (teams and names)
    bool match_over = false;    // nevt_match_over sent for the phase shown
    uint32_t known_phase = 0;   // the last phase the recording classified (3..12): Unknown samples keep it
    std::map<uint64_t, std::vector<uint64_t>> canvases_of;   // actor nodeid -> the canvas symbols of its CCanvasUICR rows
    std::string scripts_log;
};

namespace {

constexpr uint64_t kClockCanvas = 0x3bfdab0320f28174ull, kOrangeScore = 0xa48aaeeea097d314ull, kBlueScore = 0xe07d942abcbf908dull;
constexpr uint64_t kLauncher = 0x31166977834d8960ull, kMatchBoard = 0xc38ba95f61643121ull, kPlayerCard = 0xe759aea0a02300d0ull;
// The goal ends' info board: four child-canvas slots (GOAL, the goal's details, MERCY WIN, TEAMS NOT READY), of which
// the game's script (928680.., not run here) shows one at a time and none during play.
constexpr uint64_t kGoalBoard = 0xb52e9efd69343174ull, kGoalSlot = 0x41d2d42d16221416ull, kGoalDetailsSlot = 0x2fd38a8b5f7d5b99ull;
constexpr uint64_t kHelloButton1 = 0x9ad2551eb321ac1cull, kHelloButton2 = 0x1c262d0168a2c144ull, kWallNumber = 0x2f544c95a8289028ull;

bool SetText(echo::codecs::UiCanvas& c, echo::codecs::UiPrimitive& p, std::string_view s) {
    if (p.type != 1 || p.text_slot >= c.u32s.size() || p.text_capacity == 0) return false;
    const size_t off = c.u32s[p.text_slot];
    if (off + p.text_capacity > c.pool.size()) return false;
    const size_t n = std::min<size_t>(s.size(), p.text_capacity - 1);
    for (size_t i = 0; i < p.text_capacity; ++i) c.pool[off + i] = i < n ? s[i] : '\0';
    return true;
}
echo::codecs::UiPrimitive* Prim(echo::codecs::UiCanvas& c, uint64_t name) {
    for (auto& p : c.prims)
        if (p.name == name) return &p;
    return nullptr;
}
void Text(echo::codecs::UiCanvas& c, uint64_t name, std::string_view s) {
    for (auto& p : c.prims)
        if (p.name == name) SetText(c, p, s);
}
void Alpha(echo::codecs::UiCanvas& c, uint64_t name, float a) {
    for (auto& p : c.prims)
        if (p.name == name) p.rgba[3] = a;
}
std::string Num(int v) { return v < 0 ? "-" : std::to_string(v); }
std::string Two(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "%02u", v);
    return b;
}
std::string Upper(std::string s) {
    for (char& ch : s) ch = char(toupper(static_cast<unsigned char>(ch)));
    return s;
}

// The goal details panel (86c6c18f..), filled as the board's script does: "<goal type> : <points> PTS", the scorer,
// the assist line when there was one, the disc speed and throw distance (whole metres above 1, else one decimal),
// and the game clock when it went in. The scorer's heraldry title and number badge are not in the recording.
const char* GoalTypeName(uint32_t t) {
    static const char* const kNames[] = {"", "SLAM DUNK", "INSIDE SHOT", "LONG SHOT", "BOUNCE SHOT", "LONG BOUNCE SHOT",
                                         "HEADBUTT", "LONG HEADBUTT", "BUMPER SHOT", "LONG BUMPER SHOT", "SELF GOAL"};
    return t < std::size(kNames) ? kNames[t] : "";
}
template <class Goal>
void FillGoalDetails(echo::codecs::UiCanvas& c, const Goal& g) {
    char b[64];
    const std::string type = GoalTypeName(g.type);
    snprintf(b, sizeof b, "%u PTS", g.points);
    Text(c, 0xc8c33e483d7c1fbcull, type.empty() ? std::string(b) : type + " : " + b);
    Text(c, 0x44b7b1f7ef99436full, Upper(g.scorer));
    Text(c, 0xe32dc7ced09b48b5ull, "");   // the scorer's heraldry title
    Text(c, 0x41d2c9281721020cull, "");   // their number badge, and its emblem group
    for (uint64_t n : {0x41d2cf3808220b1aull, 0xc8c33e4837720dbbull, 0x7c721cac2c095f0dull, 0x7c721cac2c095f0eull,
                       0x41d2c5320827020cull, 0xc8e8d0b1a880ebfaull, 0xe32dc7dedc8c45bcull})
        Alpha(c, n, 0.f);
    const bool assist = !g.assist.empty();
    Alpha(c, 0x41d2c62e092a140aull, assist ? 1.f : 0.f);   // ASSISTED BY
    Alpha(c, 0xcc9cfa74def9f81aull, assist ? 1.f : 0.f);
    Text(c, 0x41d2c62e092a140aull, assist ? "ASSISTED BY " : "");
    Text(c, 0xcc9cfa74def9f81aull, Upper(g.assist));
    for (uint64_t n : {0x1a2ecedf2373ab10ull, 0xee4a95ba04de3edaull}) Alpha(c, n, 0.f);   // the assister's badge
    Text(c, 0xfa742074bb2f219eull, "");
    snprintf(b, sizeof b, g.speed > 1.f ? "%.0fm/s" : "%.1fm/s", g.speed);
    Text(c, 0x34952bbf4672be7cull, b);
    snprintf(b, sizeof b, g.distance > 1.f ? "%.0fm" : "%.1fm", g.distance);
    Text(c, 0x349528bf4672be7cull, b);
    if (g.clock >= 0) {
        const int ds = int(g.clock * 10.f + 0.5f);
        snprintf(b, sizeof b, "%02d:%02d.%d", ds / 600, (ds / 10) % 60, ds % 10);
        Text(c, 0x349529bf4672be7cull, b);
    } else {
        Text(c, 0x349529bf4672be7cull, "");
    }
}

// A child-canvas prim's canvas: at +120, where the other prim types keep their colour.
uint64_t ChildOf(const echo::codecs::UiPrimitive& p) {
    uint64_t c = 0;
    if (p.type == 7 && p.unnamed.size() >= 128) std::memcpy(&c, p.unnamed.data() + 120, sizeof c);
    return c ? c : (p.type == 7 ? p.resource : 0);
}
void SetChild(echo::codecs::UiPrimitive& p, uint64_t c) {
    if (p.unnamed.size() >= 128) std::memcpy(p.unnamed.data() + 120, &c, sizeof c);
    p.resource = c;
}

using ItemStates = std::map<uint64_t, script::ItemState>;

// The recording's phase (demotypes.h) as the game_state symbol the scripts compare against (gamestate_facts.h:
// each phase's steady "update" state).
uint64_t GameStateSymbol(uint32_t phase) {
    switch (phase) {
        case 3: return 0xFBC46082C080FBCFull;    // team_room_wait_update
        case 4: return 0xD69CC3ED9F1E31BFull;    // pre_match_wait_for_ready_update
        case 5: return 0x7B54C48FED3ECD93ull;    // pre_launch_update
        case 6: return 0xAB08D245B707B56Bull;    // main_loop_update
        case 7: return 0x75C7A4739D7A1739ull;    // point_score_update
        case 8: return 0x67E1E232F03BE11Dull;    // round_over_update
        case 9: return 0xF83FCDBB25BA6F07ull;    // post_match_celebration_update
        case 10: return 0xFC23497054CBCBCFull;   // pre_sudden_death_update
        case 11: return 0x939313B41BE27085ull;   // sudden_death_new_round_update
        case 12: return 0xF09C446F72B86057ull;   // post_sudden_death_update
        default: return 0;
    }
}   // item name -> what the scripts wrote to it

// True when `c` or a canvas under it holds an item `st` names.
// Echo's CSymbol64: a CRC-64 (polynomial 0x95AC9329AC4BC9B5, seed ~0, names lowercased). A script addresses an
// item inside a child canvas as "<slot>.<item>": the item's name hashed on from the slot primitive's hash (the
// match board's rows: teamblue1.playername). The hash is linear, so that name follows from the slot's hash, the
// item's own hash and the length of the item's name, which is tried.
uint64_t Crc64Step(uint64_t h, uint8_t c) {
    static const auto table = [] {
        std::array<uint64_t, 256> t{};
        for (uint64_t i = 0; i < 256; ++i) {
            uint64_t r = 0, a = 0x95AC9329AC4BC9B5ull, b = 2 * i;
            for (; b; b >>= 1, a <<= 1)
                if (b & 1) r ^= a;
            t[i] = r;
        }
        return t;
    }();
    return (h << 8) ^ table[h >> 56] ^ c;
}
uint64_t Crc64Zeros(uint64_t h, int n) {
    for (int k = 0; k < n; ++k) h = Crc64Step(h, 0);
    return h;
}
uint64_t ScopedName(uint64_t slot, uint64_t item, int len) {
    return Crc64Zeros(slot, len + 1) ^ Crc64Zeros(Crc64Step(0, '.'), len) ^ item ^ Crc64Zeros(~0ull, len);
}
// What the scripts wrote to a child canvas's items through the slot primitive `slot` that shows it.
ItemStates Scoped(const echo::codecs::UiCanvas& child, uint64_t slot, const ItemStates& st) {
    ItemStates out;
    for (const auto& p : child.prims)
        for (int len = 1; len <= 48; ++len)
            if (const auto f = st.find(ScopedName(slot, p.name, len)); f != st.end()) {
                out[p.name] = f->second;
                break;
            }
    return out;
}

bool Touches(const std::map<uint64_t, echo::codecs::UiCanvas>& corpus, const echo::codecs::UiCanvas& c, const ItemStates& st, int depth = 0) {
    if (depth > 6) return false;
    for (const auto& p : c.prims) {
        if (st.count(p.name)) return true;
        if (const uint64_t ch = ChildOf(p); ch) {
            const auto f = corpus.find(ch);
            if (f != corpus.end() && Touches(corpus, f->second, st, depth + 1)) return true;
        }
    }
    return false;
}

// What the scripts wrote, applied to a copy of a canvas: texts, alphas, and ShowUINode's hidden items (with what
// hangs under them). A child canvas holding a written item is copied too and given its own symbol in `extra`.
void ApplyScripts(echo::codecs::UiCanvas& c, const ItemStates& st, const std::map<uint64_t, echo::codecs::UiCanvas>& corpus,
                  std::map<uint64_t, echo::codecs::UiCanvas>& extra, uint64_t& next_symbol, int depth = 0) {
    std::vector<bool> hidden(c.prims.size(), false);
    for (size_t k = 0; k < c.prims.size(); ++k) {
        auto& p = c.prims[k];
        const auto s = st.find(p.name);
        if (s == st.end()) continue;
        if (s->second.text && p.type == 1) SetText(c, p, *s->second.text);
        if (s->second.alpha && p.type != 4 && p.type != 7) p.rgba[3] = *s->second.alpha;
        if (s->second.shown && !*s->second.shown) hidden[k] = true;
        if (s->second.alpha && *s->second.alpha <= 0.f) hidden[k] = true;
    }
    for (size_t k = 0; k < c.prims.size(); ++k) {   // under a hidden parent
        int up = c.prims[k].parent;
        for (int hop = 0; !hidden[k] && up >= 0 && size_t(up) < c.prims.size() && hop < 32; ++hop) {
            if (hidden[size_t(up)]) hidden[k] = true;
            up = c.prims[size_t(up)].parent;
        }
    }
    for (size_t k = 0; k < c.prims.size(); ++k) {
        auto& p = c.prims[k];
        if (hidden[k]) {
            if (p.type == 7) SetChild(p, 0);
            else if (p.type == 1) SetText(c, p, "");
            else if (p.type != 4) p.rgba[3] = 0.f;
            continue;
        }
        const uint64_t ch = ChildOf(p);
        if (!ch || depth > 6) continue;
        const auto f = corpus.find(ch);
        if (f == corpus.end()) continue;
        ItemStates inner = Scoped(f->second, p.name, st);   // "<slot>.<item>" names first, then the plain ones
        if (inner.empty() && !Touches(corpus, f->second, st)) continue;
        for (const auto& [k, v] : st) inner.emplace(k, v);
        echo::codecs::UiCanvas copy = f->second;
        ApplyScripts(copy, inner, corpus, extra, next_symbol, depth + 1);
        const uint64_t mine = next_symbol++;
        extra[mine] = std::move(copy);
        SetChild(p, mine);
    }
}

}  // namespace

Levels::Levels() = default;
Levels::~Levels() {
    if (r_) r_->set_asset_sources({});
}

bool Levels::TryLoad(const std::string& level) {
    using namespace echo;
    if (r_) r_->set_asset_sources({});   // nothing may read the old level's sources once they go
    boards_.reset();
    live_.reset();
    disc_parts_.clear();
    if (r_) r_->set_hidden_batches({});

    auto loaded = std::make_unique<Loaded>();
    ui::EditorSession& session = loaded->session;
    ui::HeadlessOptions src;
    const std::filesystem::path ec_dir = viewer_paths::EchoConstructData();
    src.corpus = corpus_;
    src.symbol_pool = ec_dir / "recon" / "name_recovery" / "symbol_pool.txt";
    src.level_names = ec_dir / "recon" / "name_recovery" / "level_names.json";
    src.roster = ec_dir / "registry" / "level_roster.tsv";
    ui::load_headless_sources(session, src);
    const Status opened = session.open_stock_level(corpus_, level);
    if (!opened.ok()) {
        why_ = "Could not open " + level + ": " + Join(opened.diagnostics());
        return false;
    }
    // EchoConstruct's capture_cli load_level, unchanged in substance.
    if (Status s = r_->set_scene(render::build_scene(session.document(), 1)); !s.ok()) {
        why_ = Join(s.diagnostics());
        return false;
    }
    ui::EditorSession* sp = &session;
    loaded->assets.emplace(
        session.corpus(), session.settings().level_name,
        [sp](std::vector<u8>& out) { return importers::document_static_instances(sp->document(), out); },
        &session.document());
    ui::ViewportAssets& assets = *loaded->assets;
    const render::MeshScene mesh = render::build_mesh_scene(r_->scene(), assets.level_mesh_sources());
    if (Status s = r_->set_mesh_scene(mesh); !s.ok()) {
        why_ = Join(s.diagnostics());
        return false;
    }
    render::LightmapScene lightmaps = ui::build_bound_lightmaps(session, bakes::BakeState{}, r_->scene(), mesh);
    const bool gi_live = lightmaps.active();
    if (Status s = r_->set_lightmap_scene(std::move(lightmaps)); !s.ok()) {
        why_ = Join(s.diagnostics());
        return false;
    }
    if (Status s = ui::apply_game_look(session, gi_live, *r_); !s.ok()) {
        why_ = Join(s.diagnostics());
        return false;
    }
    if (Status s = r_->set_material_scene(render::build_material_scene(mesh, assets.material_sources())); !s.ok()) {
        why_ = Join(s.diagnostics());
        return false;
    }
    // The sections the game streams in with this level (combat maps' rooms, arena's sub-levels): EchoConstruct's
    // composite layers, read-only, drawn with the main scene.
    layer_names_.clear();
    play_ok_ = false;
    r_->set_asset_sources({assets.mesh_sources(), assets.material_sources()});
    if (const auto req = session.layer_request()) {
        ui::LayerLoad load = ui::load_layers(session.corpus(), session.names().roster_levels(), req->level, session.loaded_children());
        if (!load.layers.empty()) {
            for (const ui::LevelLayer& l : load.layers) layer_names_.push_back(l.name);
            std::vector<render::LiveScene::LayerScene> scenes = ui::viewport_layers(session.corpus(), load.layers);
            // The global multiplayer section holds the game's runtime props (the rematch hologram's glass and light
            // ball, ...) parked at the origin until a script puts them somewhere: never drawn where they wait.
            for (size_t i = 0; i < scenes.size() && i < load.layers.size(); ++i) {
                if (load.layers[i].name != "r14_glb_global_mp") continue;
                for (render::GpuInstance& in : scenes[i].scene.instances) {
                    const float x = in.world[3], y = in.world[7], z = in.world[11];
                    if (x * x + y * y + z * z > 2.5f * 2.5f) continue;
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) in.world[r * 4 + c] = 0.f;
                }
            }
            // where play happens: the sections' extent (the globals are backdrop, the open level may be a sky shell)
            for (size_t i = 0; i < scenes.size() && i < load.layers.size(); ++i) {
                if (load.layers[i].name.rfind("r14_glb", 0) == 0 || scenes[i].scene.placement_actor.empty()) continue;
                const auto& b = scenes[i].scene.bounds;
                if (!(b.hi.x > b.lo.x)) continue;
                if (!play_ok_) play_lo_ = {b.lo.x, b.lo.y, b.lo.z}, play_hi_ = {b.hi.x, b.hi.y, b.hi.z}, play_ok_ = true;
                play_lo_ = {std::min(play_lo_.x, b.lo.x), std::min(play_lo_.y, b.lo.y), std::min(play_lo_.z, b.lo.z)};
                play_hi_ = {std::max(play_hi_.x, b.hi.x), std::max(play_hi_.y, b.hi.y), std::max(play_hi_.z, b.hi.z)};
            }
            r_->set_layers(std::move(scenes));
        }
    }
    // The level's canvases (arena's scoreboard and screens, lobby kiosks): drawn as the game draws them, where the
    // meshes would otherwise show their placeholder textures (a UV test card on the score panels). The raster
    // keeps pointers into the corpus, so it lives as long as the program (never freed: it must outlive the renderer).
    if (render::CanvasRaster* raster = render::canvas_raster_of(*r_)) {
        static ui::CanvasCorpus* canvases = nullptr;
        static std::filesystem::path canvas_root;
        if (!canvases || canvas_root != session.corpus()) {
            canvases = new ui::CanvasCorpus(session.corpus());
            canvas_root = session.corpus();
            raster->set_sources(canvases->sources());
        }
        ui::LevelCanvases lc = ui::bind_level_canvases(*r_, session.document(), *canvases);
        // A screen whose row names a texture its mesh does not sample (the arena's score panels replace 1c8e6db3..,
        // their mesh carries the UV test card 34dfbe67.. in that slot): the RT goes to the one canvas slot the
        // receiver's material does have, other than the stock white. Without it the panels show the test card.
        if (lc.set) {
            constexpr uint64_t kWhite = 0x4ae6b4f073d88ae9ull;
            const auto& mesh = r_->live_scene().mesh();
            const auto& mats = r_->material_scene();
            std::map<uint32_t, std::set<uint64_t>> slots_of;   // placement -> the canvas-slot textures of its receiver batches
            for (size_t bi = 0; bi < mesh.batches.size(); ++bi) {
                const auto& bt = mesh.batches[bi];
                if (!bt.canvas_receiver) continue;
                std::set<uint64_t> tex;
                for (uint32_t t : mats.canvas_slots(bi))
                    if (t < mats.textures.size()) tex.insert(mats.textures[t].asset);
                for (uint32_t k = 0; k < bt.instance_count; ++k)
                    if (bt.first_instance + k < mesh.instances.size()) slots_of[mesh.instances[bt.first_instance + k].pad].insert(tex.begin(), tex.end());
            }
            auto fixed = std::make_shared<render::CanvasSet>(*lc.set);
            bool changed = false;
            for (render::CanvasDraw& d : fixed->draws) {
                if (d.mode != render::CanvasDrawMode::Override || d.curve || d.placements.empty()) continue;
                std::set<uint64_t> have;
                for (uint32_t pl : d.placements)
                    if (const auto s = slots_of.find(pl); s != slots_of.end()) have.insert(s->second.begin(), s->second.end());
                if (have.empty() || have.count(d.texture)) continue;
                have.erase(kWhite);
                if (have.size() != 1) continue;
                d.texture = *have.begin();
                changed = true;
            }
            if (changed) {
                lc.set = fixed;
                r_->set_canvases(fixed);
            }
        }
        boards_ = std::make_unique<Boards>();
        boards_->base = lc.set;
        boards_->corpus = canvases;
        for (const auto& [actor, list] : lc.by_actor)
            for (const ui::BoundCanvas& bc : list) boards_->ids[bc.index] = bc.canvas;
        // Free-standing signs the game only puts up for a moment (the pause notice) or fills before showing (the
        // player rows and banner it spawns): their stock text is a placeholder, so they are not drawn.
        if (lc.set) {
            static constexpr uint64_t kTransient[] = {0x79651460c0976718ull /* PAUSED FOR / UNPAUSING IN */,
                                                      0x1c1650705edcb5f5ull, 0xc84525153330ba68ull /* MR. PLAYER_005 rows */,
                                                      0xd198c01aa9ec5d5eull /* "Text" banner */};
            auto kept = std::make_shared<render::CanvasSet>(*lc.set);
            std::erase_if(kept->draws, [&](const render::CanvasDraw& d) {
                const auto id = boards_->ids.find(d.canvas);
                return d.mode == render::CanvasDrawMode::Free && id != boards_->ids.end() &&
                       std::find(std::begin(kTransient), std::end(kTransient), id->second) != std::end(kTransient);
            });
            if (kept->draws.size() != lc.set->draws.size()) {
                boards_->base = kept;
                r_->set_canvases(kept);
            }
        }
        // The end-of-match group: every actor tree with a screen at the arena's centre (the two match boards, their
        // player cards, the rematch console). The game keeps it away during play; here its placements shrink to a point.
        if (lc.set) {
            const auto& sc = r_->live_scene().scene();
            const auto& acts = session.document().actors;
            std::map<uint32_t, std::array<float, 3>> at;
            for (const auto& in : r_->mesh_scene().instances) at[in.pad] = {in.world[3], in.world[7], in.world[11]};
            auto index_of = [&](const Boards::ActorIdT& id) -> uint32_t {
                for (uint32_t j = 0; j < acts.size(); ++j)
                    if (acts[j].id == id) return j;
                return ~0u;
            };
            std::vector<uint32_t> roots, card_roots;
            for (const auto& dr : lc.set->draws) {
                if (dr.mode != render::CanvasDrawMode::Override) continue;
                const auto cid = boards_->ids.find(dr.canvas);
                const bool card = cid != boards_->ids.end() && cid->second == kPlayerCard;
                for (uint32_t pl : dr.placements) {
                    const auto f = at.find(pl);
                    if (f == at.end() || std::abs(f->second[0]) > 6 || std::abs(f->second[2]) > 3 || std::abs(f->second[1]) > 4) continue;
                    uint32_t a = pl < sc.placement_actor.size() ? sc.placement_actor[pl] : ~0u;
                    if (card && a < acts.size()) card_roots.push_back(a);   // the card's own actor, not its tree's root
                    for (int hop = 0; a < acts.size() && acts[a].parent && hop < 16; ++hop) {
                        const uint32_t up = index_of(*acts[a].parent);
                        if (up == ~0u) break;
                        a = up;
                    }
                    if (a < acts.size()) roots.push_back(a);
                }
            }
            // The player cards' trees (the card and its black backing) stay out: what the game puts on them after a
            // match is not known here, and the viewer's own awards were made up.
            std::sort(card_roots.begin(), card_roots.end());
            card_roots.erase(std::unique(card_roots.begin(), card_roots.end()), card_roots.end());
            std::sort(roots.begin(), roots.end());
            roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
            for (uint32_t a = 0; a < acts.size() && a < sc.actor_world.size(); ++a) {
                uint32_t k = a;
                bool in = false, card = false;
                for (int hop = 0; k < acts.size() && hop < 16; ++hop) {
                    card = card || std::binary_search(card_roots.begin(), card_roots.end(), k);
                    if (std::binary_search(roots.begin(), roots.end(), k)) {
                        in = true;
                        break;
                    }
                    if (!acts[k].parent) break;
                    k = index_of(*acts[k].parent);
                }
                if (!in) continue;
                Boards::Hide h;
                h.id = acts[a].id;
                h.world = sc.actor_world[a];
                h.gone = h.world;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) h.gone.m[r * 4 + c] = 0.f;
                if (card) h.world = h.gone;
                boards_->postmatch.push_back(h);
            }
        }
    }
    if (boards_) {   // the goal ends' info boards, each turned to read the right way round from the camera (FaceViewer)
        const auto& doc = session.document();
        const auto& sc = r_->live_scene().scene();
        std::set<ActorId> hosts;
        for (const ui::CanvasRow& r : ui::canvas_rows(doc).rows)
            if (r.canvas == kGoalBoard) hosts.insert(r.actor);
        for (uint32_t a = 0; a < doc.actors.size() && a < sc.actor_world.size(); ++a) {
            if (!hosts.count(doc.actors[a].id)) continue;
            Boards::Facing f;
            const float* m = sc.actor_world[a].m;
            f.pos[0] = m[3], f.pos[1] = m[7], f.pos[2] = m[11];
            f.normal[0] = m[2], f.normal[1] = m[6], f.normal[2] = m[10];
            // it reads as placed from the arena's side (checked in the viewer at both ends)
            const float toward = -(f.pos[0] * f.normal[0] + f.pos[1] * f.normal[1] + f.pos[2] * f.normal[2]);
            f.front = toward >= 0 ? 1.f : -1.f;
            boards_->goalboard.push_back(f);
        }
        // Each board's text strip, by where it is drawn. The binder makes it a flat plane the size of the goal's
        // barrier (the canvas's receiver), which is bowed: the strip is given the barrier's own arc, from the
        // barrier mesh's extent (chord = its width, sagitta = its depth), so the text lies on it in the frame.
        if (boards_->base) {
            auto bent = std::make_shared<render::CanvasSet>(*boards_->base);
            const auto& mesh = r_->live_scene().mesh();
            for (render::CanvasDraw& dr : bent->draws) {
                const auto id = boards_->ids.find(dr.canvas);
                if (!dr.curve || id == boards_->ids.end() || id->second != kGoalBoard || dr.curve->angle_deg != 0.f) continue;
                float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                for (const auto& bt : mesh.batches) {
                    if (!bt.canvas_receiver) continue;
                    bool mine = false;
                    for (uint32_t k = 0; k < bt.instance_count && !mine; ++k)
                        if (bt.first_instance + k < mesh.instances.size())
                            mine = std::find(dr.placements.begin(), dr.placements.end(), mesh.instances[bt.first_instance + k].pad) != dr.placements.end();
                    if (!mine) continue;
                    for (uint32_t k = 0; k < bt.index_count; ++k) {
                        const size_t ii = size_t(bt.first_index) + k;
                        if (ii >= mesh.indices.size()) break;
                        const int64_t vi = int64_t(mesh.indices[ii]) + bt.base_vertex;
                        if (vi < 0 || size_t(vi) >= mesh.vertices.size()) continue;
                        for (int j = 0; j < 3; ++j) lo[j] = std::min(lo[j], mesh.vertices[size_t(vi)].pos[j]), hi[j] = std::max(hi[j], mesh.vertices[size_t(vi)].pos[j]);
                    }
                }
                const float chord = hi[0] - lo[0], sag = hi[2] - lo[2];
                if (!(chord > 0.1f) || !(sag > 0.01f) || sag > chord) continue;
                dr.curve->angle_deg = 4.f * std::atan(2.f * sag / chord) * 57.29578f;
                dr.curve->planes = 24;
                // The strip bows its middle to its -z with its ends at its origin; the barrier bows the other way
                // (its ends at the back of its depth, its middle at the front). A strip cannot bow to +z, so its
                // depth axis is mirrored (the text reads the same from the front) and its origin moved back by the
                // depth, putting its ends on the barrier's edges and its middle on the barrier's middle.
                auto& m = dr.world.m;
                const bool rows = std::fabs(m[0][3]) + std::fabs(m[1][3]) + std::fabs(m[2][3]) < 1e-6f;
                for (int k = 0; k < 3; ++k) {
                    float& axis = rows ? m[2][k] : m[k][2];
                    float& at = rows ? m[3][k] : m[k][3];
                    at -= sag * axis;
                    axis = -axis;
                }
            }
            boards_->base = bent;
            r_->set_canvases(bent);
        }
        // and the strip as seen from behind: mirrored across its vertical centre line, so the text reads the
        // right way round there while the strip keeps the barrier's shape
        if (boards_->base)
            for (size_t d = 0; d < boards_->base->draws.size(); ++d) {
                const render::CanvasDraw& dr = boards_->base->draws[d];
                const auto id = boards_->ids.find(dr.canvas);
                if (!dr.curve || id == boards_->ids.end() || id->second != kGoalBoard) continue;
                const auto& m = dr.world.m;
                // the translation's row tells the layout: m[3][0..2] (basis vectors in rows) or m[0..2][3]
                const bool rows = std::fabs(m[0][3]) + std::fabs(m[1][3]) + std::fabs(m[2][3]) < 1e-6f;
                const float t[3] = {rows ? m[3][0] : m[0][3], rows ? m[3][1] : m[1][3], rows ? m[3][2] : m[2][3]};
                Boards::Facing* nearest = nullptr;
                float best = 1e30f;
                for (Boards::Facing& f : boards_->goalboard) {
                    const float dx = t[0] - f.pos[0], dy = t[1] - f.pos[1], dz = t[2] - f.pos[2];
                    if (dx * dx + dy * dy + dz * dz < best) best = dx * dx + dy * dy + dz * dz, nearest = &f;
                }
                if (!nearest || best > 25.f) continue;
                render::Mat4 turned = dr.world;
                for (int k = 0; k < 3; ++k)
                    if (rows) turned.m[0][k] = -turned.m[0][k];
                    else turned.m[k][0] = -turned.m[k][0];
                nearest->draws.push_back(d);
                nearest->draw_world.push_back(dr.world);
                nearest->draw_turned.push_back(turned);
            }
        boards_->current = boards_->base;
    }
    if (boards_) StartScripts(session.document());
    {   // the disc: the actor with a grab component nearest the origin (the arena's), and every actor under it
        const auto& doc = session.document();
        const auto& sc = r_->live_scene().scene();
        auto is_grab = [](const auto& act) {
            for (const auto& c : act.components)
                if (const registry::CrLayout* l = registry::cr_layout_for_system(c.system); l && l->type_name == std::string_view("CR15SyncGrabCRWin10")) return true;
            return false;
        };
        uint32_t root = ~0u;
        float best = 1e30f;
        for (uint32_t a = 0; a < doc.actors.size() && a < sc.actor_world.size(); ++a) {
            if (!is_grab(doc.actors[a])) continue;
            const float* m = sc.actor_world[a].m;
            const float d = m[3] * m[3] + m[7] * m[7] + m[11] * m[11];
            if (d < best) best = d, root = a;
        }
        if (root != ~0u) {
            // inverse of the root's world (rotation-scale part inverted generally)
            const float* r = sc.actor_world[root].m;
            const float a00 = r[0], a01 = r[1], a02 = r[2], a10 = r[4], a11 = r[5], a12 = r[6], a20 = r[8], a21 = r[9], a22 = r[10];
            const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
            echo::render::Affine inv;
            if (std::fabs(det) > 1e-12f) {
                const float id = 1.f / det;
                float* q = inv.m;
                q[0] = (a11 * a22 - a12 * a21) * id, q[1] = (a02 * a21 - a01 * a22) * id, q[2] = (a01 * a12 - a02 * a11) * id;
                q[4] = (a12 * a20 - a10 * a22) * id, q[5] = (a00 * a22 - a02 * a20) * id, q[6] = (a02 * a10 - a00 * a12) * id;
                q[8] = (a10 * a21 - a11 * a20) * id, q[9] = (a01 * a20 - a00 * a21) * id, q[10] = (a00 * a11 - a01 * a10) * id;
                for (int i = 0; i < 3; ++i) q[i * 4 + 3] = -(q[i * 4] * r[3] + q[i * 4 + 1] * r[7] + q[i * 4 + 2] * r[11]);
            }
            auto under_root = [&](uint32_t a) {
                for (int hop = 0; a < doc.actors.size() && hop < 16; ++hop) {
                    if (a == root) return true;
                    if (!doc.actors[a].parent) return false;
                    uint32_t up = ~0u;
                    for (uint32_t j = 0; j < doc.actors.size(); ++j)
                        if (doc.actors[j].id == *doc.actors[a].parent) { up = j; break; }
                    a = up;
                }
                return false;
            };
            for (uint32_t a = 0; a < doc.actors.size() && a < sc.actor_world.size(); ++a) {
                if (!under_root(a)) continue;
                DiscPart part{doc.actors[a].id, {}};
                const float* w = sc.actor_world[a].m;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 4; ++j)
                        part.local.m[i * 4 + j] = inv.m[i * 4] * w[j] + inv.m[i * 4 + 1] * w[4 + j] + inv.m[i * 4 + 2] * w[8 + j] + (j == 3 ? inv.m[i * 4 + 3] : 0.f);
                disc_parts_.push_back(part);
            }
        }
    }
    {   // runtime-only pieces the viewer cannot drive, left undrawn:
        // - prp_itt_frisbee_a_outline_mat2, the disc's skinned outline: without its bones it is a 0.5 m ball
        //   (its emissive tint, black in the level, is set by the game at run time);
        // - test_outline_depthfail_blue, the disc's see-through outline, drawn only where walls hide the disc (here it
        //   would cover the disc);
        // - mpl_score_hit_fx_a, the scoring volumes' hit flash, which the game shows only when a goal is scored.
        constexpr uint64_t kRuntimeOnly[] = {0xE7E59CB17F419942ull, 0x89AFFD73CD8B19AFull, 0x0089145DC07E713Full};
        const auto& mesh = r_->live_scene().mesh();
        std::vector<uint32_t> hidden;
        for (uint32_t bi = 0; bi < mesh.batches.size(); ++bi)
            if (std::find(std::begin(kRuntimeOnly), std::end(kRuntimeOnly), uint64_t(mesh.batches[bi].material)) != std::end(kRuntimeOnly)) hidden.push_back(bi);
        r_->set_hidden_batches(hidden);
    }
    if (wchar_t dump[MAX_PATH]{}; GetEnvironmentVariableW(L"ERV_CANVAS_DUMP", dump, MAX_PATH)) {   // testing
        std::ofstream o{std::filesystem::path(dump)};
        const auto& doc = session.document();
        auto name_of = [&](const std::optional<echo::ActorId>& id) -> std::string {
            if (!id) return "-";
            for (const auto& a : doc.actors) if (a.id == *id) return a.name.empty() ? "?" : a.name;
            return "?";
        };
        const ui::CanvasRowSet rows = ui::canvas_rows(doc);
        char buf[512];
        for (const auto& r : rows.rows) {
            snprintf(buf, sizeof buf, "row actor=%s canvas=%016llx mode=%d tex=%016llx recv=%s pls=%zu dup=%u\n", name_of(r.actor).c_str(),
                     (unsigned long long)r.canvas, int(r.mode), (unsigned long long)r.texture, name_of(r.receiver).c_str(), r.placements.size(), r.duplicate_of);
            o << buf;
        }
        for (const auto& s : rows.shared) {
            snprintf(buf, sizeof buf, "shared actor=%s src=%s comp=%016llx srcrow=%d tex=%016llx\n", name_of(s.actor).c_str(), name_of(s.source).c_str(),
                     (unsigned long long)s.source_component, s.source_row ? int(*s.source_row) : -1, (unsigned long long)s.texture);
            o << buf;
        }
        for (const auto& d : rows.issues) o << "issue " << d.code << " " << d.message << "\n";
        const auto& sc = r_->live_scene().scene();
        for (size_t a = 0; a < doc.actors.size(); ++a) {
            const auto& act = doc.actors[a];
            o << "actor " << a << " " << act.name << " parent=" << name_of(act.parent);
            if (a < sc.actor_world.size()) o << " @" << sc.actor_world[a].m[3] << "," << sc.actor_world[a].m[7] << "," << sc.actor_world[a].m[11];
            for (const auto& c : act.components) {
                const registry::CrLayout* l = registry::cr_layout_for_system(c.system);
                o << " " << (l ? std::string(l->type_name) : "?");
                if (!l) continue;
                const std::string_view tn = l->type_name;
                if (tn != "CScriptCRWin10" && tn != "CSharedCanvasUICRWin10" && tn != "CCanvasUICRWin10") continue;
                snprintf(buf, sizeof buf, "{%016llx", (unsigned long long)static_cast<u64>(c.instance_name));
                o << buf;
                for (const auto& [fid, fv] : c.fields) {
                    if (const u64* v = std::get_if<u64>(&fv.v)) snprintf(buf, sizeof buf, " %016llx", (unsigned long long)*v);
                    else if (const Symbol64* v = std::get_if<Symbol64>(&fv.v)) snprintf(buf, sizeof buf, " s%016llx", (unsigned long long)static_cast<u64>(*v));
                    else if (const u32* v = std::get_if<u32>(&fv.v)) snprintf(buf, sizeof buf, " u%u", *v);
                    else if (const f32* v = std::get_if<f32>(&fv.v)) snprintf(buf, sizeof buf, " f%g", *v);
                    else continue;
                    o << buf;
                }
                o << "}";
            }
            o << "\n";
        }
        if (boards_ && boards_->base) {
            const auto& mesh = r_->live_scene().mesh();
            const auto& mats = r_->material_scene();
            for (size_t d = 0; d < boards_->base->draws.size(); ++d) {
                const auto& dr = boards_->base->draws[d];
                const auto id = boards_->ids.find(dr.canvas);
                snprintf(buf, sizeof buf, "draw %zu canvas %u=%016llx mode %d tex %016llx curve %d pls", d, dr.canvas,
                         (unsigned long long)(id != boards_->ids.end() ? id->second : 0), int(dr.mode), (unsigned long long)dr.texture, dr.curve ? 1 : 0);
                o << buf;
                if (dr.curve) {
                    snprintf(buf, sizeof buf, " [curve w %g h %g angle %g planes %u pivot %g,%g world", dr.curve->width, dr.curve->height,
                             dr.curve->angle_deg, dr.curve->planes, dr.curve->pivot[0], dr.curve->pivot[1]);
                    o << buf;
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) o << (c ? "," : " ") << dr.world.m[r][c];
                    o << "]";
                }
                for (uint32_t pl : dr.placements) {
                    o << " " << pl;
                    if (pl < sc.placement_actor.size()) o << "(a" << sc.placement_actor[pl] << ")";
                    for (size_t bi = 0; bi < mesh.batches.size(); ++bi) {
                        const auto& bt = mesh.batches[bi];
                        bool has = false;
                        for (uint32_t k = 0; k < bt.instance_count; ++k)
                            if (bt.first_instance + k < mesh.instances.size() && mesh.instances[bt.first_instance + k].pad == pl) has = true;
                        if (!has) continue;
                        o << "[b" << bi << (bt.canvas_receiver ? "R" : "") << " uvset" << int(bt.uv_set);
                        if (bi < mats.batch_pass.size()) {
                            const auto& bp = mats.batch_pass[bi];
                            const auto sc2 = echo::bakes::shader_uv_scale(bp.key_set, bp.key_element);
                            snprintf(buf, sizeof buf, " ss %016llx/%u prog %d uvscale %g,%g", (unsigned long long)bp.key_set, bp.key_element,
                                     bp.shader_program ? 1 : 0, sc2.x, sc2.y);
                            o << buf;
                            const auto& l0 = bp.layer0_emissive.uv;
                            snprintf(buf, sizeof buf, " mat %016llx mt %u bm %u lawflags %x l0uv %g,%g,%g,%g", (unsigned long long)bt.material, unsigned(bp.material_type), unsigned(bp.blend_mode),
                                     bi < mats.batch_law.size() ? unsigned(mats.batch_law[bi].flags) : 0u, l0[0], l0[1], l0[2], l0[3]);
                            o << buf;
                        }
                        {
                            float lo[4] = {1e9f, 1e9f, 1e9f, 1e9f}, hi[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
                            for (uint32_t k = 0; k < bt.index_count; ++k) {
                                const size_t ii = size_t(bt.first_index) + k;
                                if (ii >= mesh.indices.size()) break;
                                const int64_t vi = int64_t(mesh.indices[ii]) + bt.base_vertex;
                                if (vi < 0 || size_t(vi) >= mesh.vertices.size()) continue;
                                const auto& v = mesh.vertices[size_t(vi)];
                                const float q[4] = {v.uv[0], v.uv[1], v.uv1[0], v.uv1[1]};
                                for (int j = 0; j < 4; ++j) lo[j] = std::min(lo[j], q[j]), hi[j] = std::max(hi[j], q[j]);
                            }
                            snprintf(buf, sizeof buf, " uv0 [%g,%g]x[%g,%g] uv1 [%g,%g]x[%g,%g]", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
                            o << buf;
                        }
                        o << ":";
                        for (uint32_t t : mats.canvas_slots(bi))
                            if (t < mats.textures.size()) { snprintf(buf, sizeof buf, " %016llx", (unsigned long long)mats.textures[t].asset); o << buf; }
                        o << "]";
                    }
                }
                o << "\n";
            }
            for (const auto& [tex, b] : mats.unsampled_textures) { snprintf(buf, sizeof buf, "unsampled %016llx batch %u\n", (unsigned long long)tex, b); o << buf; }
        }
        // the stock scripts the screens' actors run, lifted to graphs (with Raw parts kept)
        {
            std::set<u64> assets;
            const auto& acts = doc.actors;
            for (const auto& act : acts)
                for (const auto& c : act.components) {
                    const registry::CrLayout* l = registry::cr_layout_for_system(c.system);
                    if (!l || l->type_name != std::string_view("CScriptCRWin10")) continue;
                    const auto ref = model::FieldAccess::resolve(c.system, "scriptasset");
                    if (!ref.ok()) continue;
                    const model::FieldValue* v = model::FieldAccess::read(c, ref.value());
                    if (const u64* a = v ? std::get_if<u64>(&v->v) : nullptr) assets.insert(*a);
                }
            const echo::script::NodeCatalog* cat = ui::script_catalog();
            o << "scripts " << assets.size() << " catalog " << (cat ? "ok" : "missing") << "\n";
            for (const u64 asset : assets) {
                if (!cat) break;
                char hashed[24];
                snprintf(hashed, sizeof hashed, "0x%016llX.bin", (unsigned long long)asset);
                const std::filesystem::path folder = corpus_ / "StreamingScriptWin10";
                const std::filesystem::path named = folder / dist::corpus_file_name(asset);
                std::ifstream in(std::filesystem::exists(named) ? named : folder / hashed, std::ios::binary);
                if (!in) { o << "script " << hashed << " missing\n"; continue; }
                std::vector<u8> bytes{std::istreambuf_iterator<char>(in), {}};
                const echo::script::StockLift lift = echo::script::lift_stock_script(std::move(bytes), asset, *cat, true);
                snprintf(hashed, sizeof hashed, "%016llx", (unsigned long long)asset);
                o << "script " << hashed << " graph " << lift.graph.size() << " reason " << lift.reason << "\n";
                std::ofstream g{std::filesystem::path(dump).replace_filename(std::string("script_") + hashed + ".txt")};
                g << lift.graph;
            }
        }
        if (boards_ && boards_->corpus)
            for (const auto& [idx, cid] : [&] {
                     std::map<uint32_t, uint64_t> all = boards_->ids;
                     std::vector<uint64_t> todo;
                     for (const auto& kv : all) todo.push_back(kv.second);
                     std::set<uint64_t> seen(todo.begin(), todo.end());
                     uint32_t n = 1000;
                     while (!todo.empty()) {
                         const uint64_t c = todo.back();
                         todo.pop_back();
                         const auto f = boards_->corpus->canvases.find(c);
                         if (f == boards_->corpus->canvases.end()) continue;
                         for (const auto& p : f->second.prims) {
                             u64 child = 0;
                             if (p.type == 7 && p.unnamed.size() >= 128) std::memcpy(&child, p.unnamed.data() + 120, 8);
                             if (child && seen.insert(child).second) all[n++] = child, todo.push_back(child);
                         }
                     }
                     return all;
                 }()) {
                const auto f = boards_->corpus->canvases.find(cid);
                if (f == boards_->corpus->canvases.end()) continue;
                const auto& cv = f->second;
                snprintf(buf, sizeof buf, "canvas %u %016llx prims=%zu size %ux%u\n", idx, (unsigned long long)cid, cv.prims.size(), cv.width, cv.height);
                o << buf;
                for (size_t k = 0; k < cv.prims.size(); ++k) {
                    const auto& p = cv.prims[k];
                    std::string text;
                    if (p.type == 1 && p.text_slot < cv.u32s.size()) {
                        const size_t off = cv.u32s[p.text_slot];
                        for (size_t i = 0; i < p.text_capacity && off + i < cv.pool.size() && cv.pool[off + i]; ++i) text += char(cv.pool[off + i]);
                    }
                    u64 child = 0;
                    if (p.type == 7 && p.unnamed.size() >= 128) std::memcpy(&child, p.unnamed.data() + 120, 8);
                    snprintf(buf, sizeof buf, "  %zu type %u name %016llx a %g rect %g,%g,%g,%g par %d child %016llx '%s'\n", k, unsigned(p.type), (unsigned long long)p.name, p.rgba[3], p.rect[0], p.rect[1], p.rect[2], p.rect[3], p.parent, (unsigned long long)child, text.c_str());
                    o << buf;
                }
            }
    }
    live_ = std::move(loaded);
    current_ = level;
    return true;
}

// A view from inside the space, high at one end of its long axis, looking down its length: how the maps
// read in game, rather than the shell seen from outside.
echo::render::FlyCamera Levels::Overview(float aspect) const {
    echo::render::FlyCamera cam;
    cam.fovy = 1.2f;
    if (!r_) return cam;
    const echo::render::MeshScene& mesh = r_->mesh_scene();
    if (!mesh.has_bounds && !play_ok_) return cam;
    struct { echo::Vec3 lo, hi; } b{};
    if (play_ok_) b.lo = {play_lo_.x, play_lo_.y, play_lo_.z}, b.hi = {play_hi_.x, play_hi_.y, play_hi_.z};
    else b.lo = {mesh.bounds.lo.x, mesh.bounds.lo.y, mesh.bounds.lo.z}, b.hi = {mesh.bounds.hi.x, mesh.bounds.hi.y, mesh.bounds.hi.z};
    const float cx = (b.lo.x + b.hi.x) / 2, cy = (b.lo.y + b.hi.y) / 2, cz = (b.lo.z + b.hi.z) / 2;
    const float ex = (b.hi.x - b.lo.x) / 2, ey = (b.hi.y - b.lo.y) / 2, ez = (b.hi.z - b.lo.z) / 2;
    // stock shells carry far-off backdrop geometry: keep inside a sensible play-space size
    const float lx = std::min(ex, 60.f), lz = std::min(ez, 60.f), ly = std::min(ey, 25.f);
    const bool along_z = lz >= lx;
    const float back = (along_z ? lz : lx) * 0.62f;
    cam.eye = {along_z ? cx : cx - back, cy + ly * 0.28f, along_z ? cz - back : cz};
    const float tx = cx - cam.eye.x, ty = (cy - ly * 0.15f) - cam.eye.y, tz = cz - cam.eye.z;
    cam.yaw = atan2f(tx, tz);
    cam.pitch = atan2f(ty, sqrtf(tx * tx + tz * tz));
    (void)aspect;
    return cam;
}

std::filesystem::path Levels::ArtFile(const std::string& level) { return CacheDir() / (level + ".erva"); }

bool Levels::HasPreview(const std::string& level) {
    std::error_code ec;
    return std::filesystem::exists(ArtFile(level), ec);
}

bool Levels::MakePreview(const std::string& level) {
    if (!Load(level)) return false;
    r_->set_game_view(true);   // no editor markers in the art
    r_->wait_for_shading();
    const echo::render::FlyCamera cam = Overview(16.f / 9.f);
    auto frame = r_->capture(cam, 1920, 1080, {.selection_outline = false});
    r_->set_game_view(false);
    if (!frame.ok()) {
        why_ = Join(frame.diagnostics());
        return false;
    }
    const std::filesystem::path out = ArtFile(level), tmp = std::filesystem::path(out).concat(L".tmp");
    if (!WriteImage(tmp, Downscale(frame.value(), 960, 540))) return false;
    std::error_code ec;
    std::filesystem::rename(tmp, out, ec);
    return !ec;
}

const std::string& Levels::ScriptsLog() const {
    static const std::string none;
    return boards_ ? boards_->scripts_log : none;
}

// The level's stock scripts that drive its screens, lifted from the game's own containers by EchoConstruct's lifter
// (cached under %LOCALAPPDATA%\EchoReplayViewer\script_lifts per container hash), one running instance per CScriptCR
// row with that row's parameters. A script with a Raw part the VM cannot run from its record (script::Runnable), or one
// that writes to no screen, is not run.
void Levels::StartScripts(const echo::model::Document& doc) {
    using namespace echo;
    Boards& b = *boards_;
    b.vm.Clear();
    b.lobby.clear();
    b.match_over = false;
    b.canvases_of.clear();
    b.scripts_log.clear();
    std::map<ActorId, u64> nodeid_of;
    for (const model::Actor& a : doc.actors) nodeid_of[a.id] = static_cast<u64>(a.nodeid);
    const ui::CanvasRowSet rows = ui::canvas_rows(doc);
    for (const ui::CanvasRow& r : rows.rows)
        if (r.canvas) b.canvases_of[nodeid_of[r.actor]].push_back(r.canvas);

    std::string why;
    const echo::script::NodeCatalog* cat = ui::script_catalog(&why);
    if (!cat) {
        b.scripts_log = "screen scripts: not run (" + why + ")\n";
        return;
    }
    const std::filesystem::path cache_dir = CacheDir().parent_path() / "script_lifts";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    const std::string version = echo::script::stock_lift_version() + " keep-raw r2";   // r2: function outputs set from a constant or in place
    std::map<u64, std::shared_ptr<const script::Graph>> graphs;
    auto load = [&](u64 asset) -> std::shared_ptr<const script::Graph> {
        char hashed[24], hex[20];
        snprintf(hashed, sizeof hashed, "0x%016llX.bin", (unsigned long long)asset);
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)asset);
        const std::filesystem::path folder = corpus_ / "StreamingScriptWin10";
        const std::filesystem::path named = folder / dist::corpus_file_name(asset);
        std::ifstream in(std::filesystem::exists(named, ec) ? named : folder / hashed, std::ios::binary);
        if (!in) {
            b.scripts_log += std::string("  ") + hex + ": no container\n";
            return nullptr;
        }
        std::vector<u8> bytes{std::istreambuf_iterator<char>(in), {}};
        if (wchar_t probe[MAX_PATH]{}; GetEnvironmentVariableW(L"ERV_LIFT_PROBE", probe, MAX_PATH)) {   // testing
            const std::filesystem::path dir(probe);
            if (auto mr = echo::script::ScriptModule::open(bytes, hex); mr.ok()) {
                const auto* m = &mr.value();
                const echo::script::FlowNodes fn = echo::script::lift_flow_nodes(*m);
                std::ofstream o{dir / (std::string(hex) + ".txt")};
                o << "reason " << fn.reason << "\n";
                for (const auto& u : fn.unrecognized) {
                    const auto rva = m->symbol_rva(u);
                    o << "unrecognized " << u << " rva " << std::hex << (rva ? *rva : 0) << std::dec << "\n";
                    if (auto body = m->function_body(u); body.ok()) {
                        std::ofstream bo{dir / (std::string(hex) + "_" + u + ".bin"), std::ios::binary};
                        bo.write(reinterpret_cast<const char*>(body.value().data()), std::streamsize(body.value().size()));
                    }
                    for (const auto& r : echo::script::body_references(*m, u)) o << "  ref " << r << "\n";
                }
                for (const auto& row : echo::script::flow_node_rows(fn)) o << row << "\n";
                for (const auto& [sym, rva] : m->symbols()) {   // every node body's rva, and its bytes
                    if (!sym.starts_with("execute_node_") && !sym.starts_with("expression_")) continue;
                    o << "sym " << sym << " " << std::hex << rva << std::dec << "\n";
                    if (auto body = m->function_body(sym); body.ok()) {
                        std::ofstream bo{dir / (std::string(hex) + "_" + sym + ".bin"), std::ios::binary};
                        bo.write(reinterpret_cast<const char*>(body.value().data()), std::streamsize(body.value().size()));
                    }
                }
                std::ofstream{dir / (std::string(hex) + ".dll"), std::ios::binary}.write(reinterpret_cast<const char*>(m->dll().data()), std::streamsize(m->dll().size()));
                std::ofstream{dir / (std::string(hex) + ".pdb"), std::ios::binary}.write(reinterpret_cast<const char*>(m->pdb().data()), std::streamsize(m->pdb().size()));
            }
        }
        const std::string sha = echo::script::container_sha256(bytes);
        const std::filesystem::path file = echo::script::stock_lift_cache_file(cache_dir, sha, asset);
        std::optional<echo::script::StockLift> lift = echo::script::read_stock_lift_cache(file, version);
        if (!lift || lift->asset != asset) {
            lift = echo::script::lift_stock_script(std::move(bytes), asset, *cat, true);
            echo::script::write_stock_lift_cache(file, version, *lift);
        }
        std::shared_ptr<const script::Graph> g = lift->graph.empty() ? nullptr : script::ParseGraph(lift->graph);
        if (!g) {
            b.scripts_log += std::string("  ") + hex + ": not lifted (" + lift->reason.substr(0, 160) + ")\n";
            return nullptr;
        }
        if (!script::WritesScreens(*g)) return nullptr;
        if (std::string raw; !script::Runnable(*g, &raw)) {
            b.scripts_log += std::string("  ") + hex + ": writes screens but carries a Raw " + raw + " part this does not run; not run\n";
            return nullptr;
        }
        b.scripts_log += std::string("  ") + hex + ": runs (" + std::to_string(script::NodeCount(*g)) + " nodes" +
                         (script::Whole(*g) ? "" : ", some from their Raw records") + ")\n";
        return g;
    };
    auto value_of = [](const echo::script::ScriptParamValue& v) -> script::Value {
        return std::visit(
            [](const auto& x) -> script::Value {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, f32>) return script::Value::R(x);
                else if constexpr (std::is_same_v<T, i64>) return script::Value::I(x);
                else if constexpr (std::is_same_v<T, u64>) return script::Value::S(x);
                else if constexpr (std::is_same_v<T, bool>) return script::Value::B(x);
                else if constexpr (std::is_same_v<T, std::string>) return script::Value::T(x);
                else if constexpr (std::is_same_v<T, echo::script::ActorRefValue>) return script::Value::S(x.actor_name);
                else if constexpr (std::is_same_v<T, echo::script::ConnectionValue>) return script::Value::S(x.node_name);
                else if constexpr (std::is_same_v<T, std::array<f32, 2>> || std::is_same_v<T, std::array<f32, 3>> ||
                                   std::is_same_v<T, std::array<f32, 4>>) return script::Value::R(x[0]);
                else return script::Value{};
            },
            v);
    };
    for (const model::Actor& a : doc.actors) {
        for (const echo::script::ScriptInstanceView& v : echo::script::script_instances(doc, a.id)) {
            auto g = graphs.find(v.asset);
            if (g == graphs.end()) g = graphs.emplace(v.asset, load(v.asset)).first;
            if (!g->second) continue;
            script::Params p;
            for (const echo::script::ScriptParam& prm : v.params) {
                if (prm.source != echo::script::ScriptParam::Source::Row) continue;
                p[prm.name] = value_of(prm.value);
                p[prm.id] = p[prm.name];
            }
            b.vm.Add(g->second, static_cast<u64>(a.nodeid), p);
        }
    }
    b.scripts_log = "screen scripts: " + std::to_string(b.vm.Instances()) + " instances\n" + b.scripts_log;
}

Image Levels::Blurred(const Image& src) {
    Image im;
    im.w = std::max(1u, src.w / 2), im.h = std::max(1u, src.h / 2);
    im.rgba.resize(size_t(im.w) * im.h * 4);
    for (uint32_t y = 0; y < im.h; ++y)
        for (uint32_t x = 0; x < im.w; ++x)
            for (int c = 0; c < 4; ++c) {
                const size_t a = (size_t(y * 2) * src.w + x * 2) * 4 + c;
                im.rgba[(size_t(y) * im.w + x) * 4 + c] =
                    uint8_t((src.rgba[a] + src.rgba[a + 4] + src.rgba[a + src.w * 4] + src.rgba[a + src.w * 4 + 4]) / 4);
            }
    Blur(im, 5);
    return im;
}

void Levels::SetDisc(bool have, const float pos[3], const float rot[4]) {
    if (!r_ || disc_parts_.empty()) return;
    echo::render::Affine d;   // the recorded disc's pose
    if (have) {
        const float x = rot[0], y = rot[1], z = rot[2], w = rot[3];
        const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) d.m[i * 4 + j] = R[i * 3 + j];
            d.m[i * 4 + 3] = pos[i];
        }
    } else {
        for (float& v : d.m) v = 0.f;   // zero scale: not drawn
    }
    for (const DiscPart& p : disc_parts_) {
        echo::render::Affine w;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j)
                w.m[i * 4 + j] = d.m[i * 4] * p.local.m[j] + d.m[i * 4 + 1] * p.local.m[4 + j] + d.m[i * 4 + 2] * p.local.m[8 + j] + (j == 3 ? d.m[i * 4 + 3] : 0.f);
        r_->set_instance_world(p.id, echo::render::mat4_from_affine(w));
    }
}

// The goal boards' text strips in `set`, each as its board faces now.
void Levels::TurnGoalText(echo::render::CanvasSet& set) const {
    for (const Boards::Facing& f : boards_->goalboard)
        for (size_t k = 0; k < f.draws.size(); ++k)
            if (f.draws[k] < set.draws.size()) set.draws[f.draws[k]].world = f.shown == 1 ? f.draw_turned[k] : f.draw_world[k];
}

void Levels::FaceViewer(const float eye[3]) {
    if (!boards_ || !r_) return;
    bool text = false;
    for (Boards::Facing& f : boards_->goalboard) {
        const float side = (eye[0] - f.pos[0]) * f.normal[0] + (eye[1] - f.pos[1]) * f.normal[1] + (eye[2] - f.pos[2]) * f.normal[2];
        const int want = side * f.front >= 0 ? 0 : 1;
        if (want == f.shown) continue;
        f.shown = want;
        text = text || !f.draws.empty();
    }
    if (text && boards_->current) {
        auto set = std::make_shared<echo::render::CanvasSet>(*boards_->current);
        TurnGoalText(*set);
        boards_->current = set;
        r_->set_canvases(std::move(set));
    }
}

void Levels::SetBoards(const BoardState& st) {
    using namespace echo;
    if (!boards_ || !boards_->base || !boards_->corpus || !r_) return;
    Boards& b = *boards_;
    ui::CanvasCorpus& corpus = *b.corpus;

    if (!st.valid) return;   // not the arena, or no game state recorded: the screens stay as they are
    const bool post = st.phase == 8 || st.phase == 9;   // RoundOver (a one-round match ends there), PostMatch
    if (int(post) != b.postmatch_shown) {
        for (const auto& h : b.postmatch) r_->set_instance_world(h.id, render::mat4_from_affine(post ? h.world : h.gone));
        b.postmatch_shown = int(post);
    }

    // what each screen says
    char clock[32];
    {
        const float c = std::max(0.f, st.clock);
        const int cs = int(c * 100.f + 0.5f);
        snprintf(clock, sizeof clock, "%02d:%02d:%02d", cs / 6000, (cs / 100) % 60, cs % 100);
    }
    char launch[16] = "";
    if ((st.phase == 4 || st.phase == 5) && st.countdown > 0.f) snprintf(launch, sizeof launch, "%04.1f", st.countdown);
    std::vector<BoardPlayer> team[2];
    for (const BoardPlayer& p : st.players)
        if (p.team < 2) team[p.team].push_back(p);
    for (auto& t : team)
        std::stable_sort(t.begin(), t.end(), [](const BoardPlayer& x, const BoardPlayer& y) { return x.points != y.points ? x.points > y.points : x.assists > y.assists; });
    const bool sudden = st.phase >= 10 && st.phase <= 12;
    // point_score_update: the moments after a goal (an unclassified sample keeps the last known phase)
    // The goal boards (928680.., which does not run here, as its graph reads): "GOAL" for the first seconds after
    // a goal (its DelayNode of 3 s), then that goal's details, kept until the next goal. Nothing before the first.
    constexpr float kGoalBanner = 3.f;
    const BoardState::Goal& gl = st.goal;
    std::string goal_key = "-";
    if (gl.have && gl.since < kGoalBanner) goal_key = "goal";
    else if (gl.have) {
        char sp[96];
        snprintf(sp, sizeof sp, "|%u|%u|%g|%g|%g", gl.points, gl.type, gl.speed, gl.distance, gl.clock);
        goal_key = "details|" + gl.scorer + "|" + gl.assist + sp;
    }

    // Echo's own screen scripts: the net variables they read, from the recording
    {
        std::map<uint64_t, script::Value> net;
        // A sample the recorder could not classify (phase 0..2) is not a game state of its own: the game's
        // net variable still holds the last one, so the screens keep it (Playing when nothing came before).
        if (st.phase >= 3 && st.phase <= 12) b.known_phase = st.phase;
        const uint32_t phase = b.known_phase ? b.known_phase : 6;
        net[0xB2996AD399FDC0FFull] = script::Value::S(GameStateSymbol(phase));               // game_state
        net[0xB2996AD484F1D1E8ull] = script::Value::R(std::max(0.f, st.clock));            // game_timer
        net[0x3E766A9AFE6C97B0ull] = script::Value::R(std::max(0.f, st.countdown));        // countdown_timer
        net[0x1C97052A21E09F11ull] = script::Value::I(st.blue);                             // blue_score
        net[0x2ED07B1C8C27548Dull] = script::Value::I(st.orange);                           // red_score
        // the lobby (R15NetTeamSlotToUserIdExpression and the rest): each team in the recording's order
        std::vector<script::Player> lobby;
        std::string roster;
        int64_t slots[2] = {0, 0};
        for (const BoardPlayer& p : st.players) {
            if (p.team >= 2) continue;
            script::Player x;
            x.team = p.team;
            x.slot = slots[p.team]++;
            x.name = p.name;
            x.ping = std::max(0, p.ping);
            x.stats[0x41D2D732132D130Dull] = p.points;
            x.stats[0x2FD69C8C57674399ull] = p.assists;
            x.stats[0xE32DC7C9D89941A3ull] = std::max(0, p.saves);
            x.stats[0xE32DC7C9CD9A4AA3ull] = p.stuns;
            roster += std::to_string(p.team) + p.name + "|";
            lobby.push_back(std::move(x));
        }
        b.vm.SetPlayers(std::move(lobby));
        b.vm.Tick(net);
        // the game events the board scripts listen for, from the recording's changes
        if (roster != b.lobby) {
            b.lobby = roster;
            b.vm.SendEvent(0x5ED56ECDBE7C16D0ull);   // delegate_onlobbymemberjoined
        }
        const bool over = phase == 8 || phase == 9;  // RoundOver, PostMatch
        if (over && !b.match_over) b.vm.SendEvent(0x0772D3C5391BA59Eull);   // nevt_match_over
        b.match_over = over;
    }
    if (wchar_t dump[MAX_PATH]{}; GetEnvironmentVariableW(L"ERV_CANVAS_DUMP", dump, MAX_PATH)) {   // testing
        std::ofstream o{std::filesystem::path(dump).replace_extension(".vm.txt")};
        o << b.scripts_log << "phase " << st.phase << " clock " << st.clock << " countdown " << st.countdown << '\n';
        for (const auto& u : b.vm.Unknown()) o << "unknown node " << u << '\n';
        for (const auto& [k, v] : b.vm.Items()) {
            char h[96];
            snprintf(h, sizeof h, "actor %016llx comp %016llx item %016llx hosts %zu:", (unsigned long long)k.actor, (unsigned long long)k.component,
                     (unsigned long long)k.item, b.canvases_of.count(k.actor) ? b.canvases_of[k.actor].size() : size_t(0));
            o << h;
            if (v.text) o << " text '" << *v.text << "'";
            if (v.shown) o << " shown " << *v.shown;
            if (v.alpha) o << " alpha " << *v.alpha;
            o << '\n';
        }
    }
    // what they wrote, per canvas symbol (every actor hosting that canvas)
    std::map<uint64_t, ItemStates> scripted;
    for (const auto& [k, v] : b.vm.Items()) {
        const auto host = b.canvases_of.find(k.actor);
        if (host == b.canvases_of.end()) continue;
        for (const uint64_t c : host->second) scripted[c][k.item] = v;
    }
    auto scripted_key = [&](uint64_t canvas) {
        std::string out;
        const auto f = scripted.find(canvas);
        if (f == scripted.end()) return out;
        for (const auto& [item, v] : f->second) {
            char h[24];
            snprintf(h, sizeof h, "|%llx:", (unsigned long long)item);
            out += h;
            if (v.text) out += "t" + *v.text;
            if (v.shown) out += *v.shown ? "s1" : "s0";
            if (v.alpha) out += "a" + std::to_string(*v.alpha);
        }
        return out;
    };

    std::string key = std::string(clock) + "|" + launch + "|" + std::to_string(st.blue) + "|" + std::to_string(st.orange) + "|" +
                      std::to_string(st.round) + "|" + (sudden ? "s" : "") + "|vm" + std::to_string(b.vm.Generation());
    for (const auto& t : team)
        for (const BoardPlayer& p : t)
            key += "|" + p.name + "," + std::to_string(p.points) + "," + std::to_string(p.assists) + "," + std::to_string(p.stuns);
    if (key == b.last) return;
    b.last = key;

    std::map<uint64_t, codecs::UiCanvas> rows;   // the board's player rows, one canvas copy each
    render::CanvasFonts fonts;
    fonts.font = [&](u64 s) -> const codecs::TtFont* {
        const auto f = corpus.fonts.find(s);
        return f == corpus.fonts.end() ? nullptr : &f->second;
    };
    render::CanvasChildren children;
    children.canvas = [&](u64 s) -> const codecs::UiCanvas* {
        if (const auto r = rows.find(s); r != rows.end()) return &r->second;
        const auto c = corpus.canvases.find(s);
        return c == corpus.canvases.end() ? nullptr : &c->second;
    };
    auto lay = [&](const codecs::UiCanvas& c, uint64_t key_symbol) -> std::shared_ptr<const render::CanvasQuads> {
        Result<render::CanvasQuads> r = render::layout_canvas(c, fonts, children);
        if (!r.ok()) return nullptr;
        auto q = std::make_shared<render::CanvasQuads>(std::move(r.value()));
        q->canvas = key_symbol;
        return q;
    };

    std::string people;
    for (const auto& t : team)
        for (const BoardPlayer& p : t) people += "|" + p.name + "," + std::to_string(p.points) + "," + std::to_string(p.assists) + "," + std::to_string(p.stuns);
    // the cached layout for `slot` when its key is unchanged, else `build`'s
    auto cached = [&](uint64_t slot, const std::string& k, auto&& build) -> std::shared_ptr<const render::CanvasQuads> {
        Boards::Laid& l = b.laid[slot];
        if (l.quads && l.key == k) return l.quads;
        auto q = build();
        if (q) l = {k, q};
        return q;
    };
    auto set = std::make_shared<render::CanvasSet>(*b.base);
    uint64_t next_symbol = 0xEB0C000000000000ull;   // the scripted child-canvas copies
    for (size_t i = 0; i < b.base->canvases.size(); ++i) {
        const auto idit = b.ids.find(uint32_t(i));
        if (idit == b.ids.end() || !b.base->canvases[i]) continue;
        const uint64_t id = idit->second;
        const auto src = corpus.canvases.find(id);
        if (src == corpus.canvases.end()) continue;
        const uint64_t key_symbol = b.base->canvases[i]->canvas;
        std::string k;
        switch (id) {
            case kClockCanvas: k = clock; break;
            case kOrangeScore: k = Two(st.orange); break;
            case kBlueScore: k = Two(st.blue); break;
            case kLauncher: k = launch; break;
            case kGoalBoard: k = goal_key; break;
            case kMatchBoard: k = Two(st.blue) + Two(st.orange) + std::to_string(st.round) + (sudden ? "s" : "") + people; break;
            default: break;
        }
        const auto sst_it = scripted.find(id);
        static const ItemStates kNoStates;
        const ItemStates& sst = sst_it == scripted.end() ? kNoStates : sst_it->second;
        k += scripted_key(id);
        if (id != kPlayerCard) {
            const auto q = cached(i, k, [&]() -> std::shared_ptr<const render::CanvasQuads> {
        codecs::UiCanvas c = src->second;
        switch (id) {
            case kClockCanvas: Text(c, 0xd90f94f6485c35e0ull, clock); break;
            case kOrangeScore: Text(c, 0x2ed07b1c8c27548dull, Two(st.orange)); break;
            case kBlueScore: Text(c, 0x1c97052a21e09f11ull, Two(st.blue)); break;
            case kLauncher: Text(c, 0xd90f94f6485c35e0ull, launch); break;
            case kHelloButton1:
            case kHelloButton2: Text(c, 0xc8c33e482e7606a4ull, ""); break;
            case kWallNumber: Text(c, 0x41d2c9281721020cull, ""); break;
            case kGoalBoard: {
                const bool banner = goal_key == "goal", details = goal_key.rfind("details", 0) == 0;
                for (auto& p : c.prims) {
                    if (p.type != 7) continue;
                    if (banner && p.name == kGoalSlot) continue;
                    if (details && p.name == kGoalDetailsSlot) {
                        const auto dc = corpus.canvases.find(ChildOf(p));
                        if (dc == corpus.canvases.end()) {
                            SetChild(p, 0);
                            continue;
                        }
                        codecs::UiCanvas d = dc->second;
                        FillGoalDetails(d, gl);
                        const u64 mine = 0xEB0D000000000000ull;
                        rows[mine] = std::move(d);
                        SetChild(p, mine);
                        continue;
                    }
                    SetChild(p, 0);
                }
                return lay(c, key_symbol);
            }
            case kMatchBoard: {
                Text(c, 0x1c97052a21e09f11ull, Two(st.blue));
                Text(c, 0x2ed07b1c8c27548dull, Two(st.orange));
                Text(c, 0x51c03abe1ba460cdull, "");   // (NOT READY): a lobby state
                Text(c, 0x989091a13b836b5dull, "");
                Text(c, 0x5a76a739a0832ec0ull, "");
                Text(c, 0x1325abff76339012ull, "");
                Text(c, 0xdad765e36e5b7e62ull, "ROUND " + std::to_string(st.round + 1));   // the recording counts from 0
                // Echo's board script (57a56db2) fills the overtime panel and the player rows itself
                // (ApplyScripts below); what follows is the fallback for when it does not run.
                if (!sst.empty()) break;
                // the overtime panel shows only in sudden death
                for (uint64_t n : {0x25653046ecfd578dull, 0x510f94a6e06c0c7dull, 0xcdd5e9fd9df4e4f6ull, 0x53f0bc1f8ba57d86ull, 0x183136eb34e85132ull})
                    if (!sudden) Alpha(c, n, 0.f);
                Text(c, 0x183136eb34e85132ull, std::to_string(st.round + 1));
                // the player rows: each its own copy of the row canvas, filled from the top by points
                const auto rects = codecs::ui_canvas_layout(c);
                float split = 1e9f;   // the orange block's top
                if (const auto* o = Prim(c, 0xb600c6d74e0c1c4cull)) split = rects[size_t(o - c.prims.data())][1];
                std::vector<std::pair<float, size_t>> slots[2];
                for (size_t k = 0; k < c.prims.size(); ++k)
                    if (c.prims[k].type == 7) slots[rects[k][1] >= split ? 1 : 0].push_back({rects[k][1], k});
                for (int t = 0; t < 2; ++t) {
                    std::sort(slots[t].begin(), slots[t].end());
                    for (size_t r = 0; r < slots[t].size(); ++r) {
                        codecs::UiPrimitive& slot = c.prims[slots[t][r].second];
                        u64 row_symbol = 0;
                        std::memcpy(&row_symbol, slot.unnamed.data() + 120, sizeof row_symbol);
                        const auto rc = corpus.canvases.find(row_symbol);
                        if (rc == corpus.canvases.end()) continue;
                        codecs::UiCanvas row = rc->second;
                        const auto rr = codecs::ui_canvas_layout(row);
                        std::vector<std::pair<float, size_t>> texts;
                        for (size_t k = 0; k < row.prims.size(); ++k) {
                            if (row.prims[k].type != 1) continue;
                            if (row.prims[k].name == 0x41d2c9281721020cull) {   // the number badge left of the name
                                SetText(row, row.prims[k], "");
                                continue;
                            }
                            texts.push_back({rr[k][0], k});   // then the name, points, assists, saves, stuns, ping
                        }
                        std::sort(texts.begin(), texts.end());
                        const BoardPlayer* p = r < team[t].size() ? &team[t][r] : nullptr;
                        const std::string cols[6] = {p ? Upper(p->name) : "", p ? Num(p->points) : "", p ? Num(p->assists) : "",
                                                     p ? Num(p->saves) : "", p ? Num(p->stuns) : "", p ? Num(p->ping) : ""};
                        for (size_t k = 0; k < texts.size(); ++k) SetText(row, row.prims[texts[k].second], k < 6 ? cols[k] : "");
                        const u64 mine = 0xEB0A2D0000000000ull | (u64(t) << 8) | r;
                        rows[mine] = std::move(row);
                        std::memcpy(slot.unnamed.data() + 120, &mine, sizeof mine);
                    }
                }
                break;
            }
            default:
                if (sst.empty()) return nullptr;
                break;
        }
        if (!sst.empty()) ApplyScripts(c, sst, corpus.canvases, rows, next_symbol);
        return lay(c, key_symbol);
            });
            if (q) set->canvases[i] = q;
            continue;
        }
        // the player cards are kept out (see the end-of-match group in TryLoad)
    }
    TurnGoalText(*set);
    b.current = set;
    r_->set_canvases(std::move(set));
}

}  // namespace erv
