// EchoReplayViewer: the window, the start menu and the player.
//
//   menu    blurred map backgrounds cycling behind a replay browser (pick a folder, filter, search, watch)
//   player  the level as EchoConstruct renders it, every player and the disc from the recording, a timeline
//           with goals / stuns / bookmarks, 0.1x-4x either way, frame steps, five cameras, names, voice markers

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <lucide_icons.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <echo/renderer/preview.hpp>
#include <ui/capture_cli.hpp>

#include "avatar.h"
#include "figure.h"
#include "gpu.h"
#include "gamefiles.h"
#include "voice.h"
#include "level.h"
#include "replay.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;
using namespace erv;

namespace {

// ---- small helpers ---------------------------------------------------------------------------------------------
std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string Clock(double s, bool tenths = false) {
    if (s < 0) s = 0;
    char b[32];
    const int m = int(s / 60);
    if (tenths) snprintf(b, sizeof b, "%d:%04.1f", m, s - m * 60);
    else snprintf(b, sizeof b, "%d:%02d", m, int(s) % 60);
    return b;
}
fs::path AppDir() {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = fs::path(p) / "EchoReplayViewer";
    CoTaskMemFree(p);
    std::error_code ec;
    fs::create_directories(out, ec);
    return out;
}
std::wstring IniPath() { return (AppDir() / "viewer.ini").wstring(); }
std::wstring IniGet(const wchar_t* key, const wchar_t* def = L"") {
    wchar_t b[2048];
    GetPrivateProfileStringW(L"viewer", key, def, b, 2048, IniPath().c_str());
    return b;
}
void IniSet(const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(L"viewer", key, v.c_str(), IniPath().c_str()); }

ImU32 Rgba(float r, float g, float b, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32({r, g, b, a}); }
// team colours as the arena shows them
const ImVec4 kBlue{0.25f, 0.62f, 1.f, 1.f}, kOrange{1.f, 0.58f, 0.16f, 1.f}, kGrey{0.75f, 0.78f, 0.82f, 1.f};
const ImVec4 kAccent{0.30f, 0.86f, 1.f, 1.f};
ImVec4 TeamCol(uint32_t team) { return team == 0 ? kBlue : team == 1 ? kOrange : kGrey; }
uint32_t ToWire(ImVec4 c) {   // 0xAABBGGRR
    return (uint32_t(c.w * 255) << 24) | (uint32_t(c.z * 255) << 16) | (uint32_t(c.y * 255) << 8) | uint32_t(c.x * 255);
}

bool PickFolder(HWND owner, std::wstring& out) {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return false;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Choose a replay folder");
    if (FAILED(d->Show(owner))) return false;
    ComPtr<IShellItem> it;
    PWSTR p = nullptr;
    if (FAILED(d->GetResult(&it)) || FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}
bool PickFile(HWND owner, std::wstring& out) {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return false;
    const COMDLG_FILTERSPEC f[] = {{L"Replays", L"*.echodemo;*.json;*.echoreplay;*.tape"}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, f);
    d->SetTitle(L"Open a replay");
    if (FAILED(d->Show(owner))) return false;
    ComPtr<IShellItem> it;
    PWSTR p = nullptr;
    if (FAILED(d->GetResult(&it)) || FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

// ---- the replay library (the menu's list) ------------------------------------------------------------------------
enum class Kind { Demo, Clip, Legacy };
struct Entry {
    fs::path path;
    Kind kind = Kind::Demo;
    std::string file, level, pretty, when;
    uint64_t level_id = 0;
    uint64_t bytes = 0;
    std::atomic<int> peeked{0};   // 0 not yet, 1 ok, -1 failed
    ErvInfo info{};
    double sort_key = 0;
};

class Library {
public:
    void Scan(const fs::path& dir) {
        Stop();
        std::lock_guard<std::mutex> l(m_);
        entries_.clear();
        dir_ = dir;
        std::error_code ec;
        auto add = [&](const fs::path& p) {
            std::wstring ext = p.extension().wstring();
            for (auto& c : ext) c = wchar_t(towlower(c));
            Kind k;
            if (ext == L".echodemo") k = Kind::Demo;
            else if (ext == L".json") k = Kind::Clip;
            else if (ext == L".echoreplay" || ext == L".tape") k = Kind::Legacy;
            else return;
            if (k == Kind::Demo && p.parent_path().filename() == L"clips") return;   // clip parts: listed via their .json
            auto e = std::make_unique<Entry>();
            e->path = p;
            e->kind = k;
            e->file = U8(p.filename().wstring());
            e->bytes = fs::file_size(p, ec);
            // EchoReplay names: 2026-11-14_02-33-25_<level>_<gametype>.echodemo
            const std::string f = e->file;
            if (f.size() >= 19 && f[4] == '-' && f[10] == '_') {
                e->when = f.substr(0, 10) + "  " + f.substr(11, 2) + ":" + f.substr(14, 2);
                e->sort_key = atof((f.substr(0, 4) + f.substr(5, 2) + f.substr(8, 2) + f.substr(11, 2) + f.substr(14, 2) + f.substr(17, 2)).c_str());
                if (k == Kind::Demo && f.size() > 36) e->level_id = strtoull(f.substr(20, 16).c_str(), nullptr, 16);
            } else {
                auto t = fs::last_write_time(p, ec);
                e->sort_key = double(t.time_since_epoch().count()) / 1e7;
                e->when = "";
            }
            if (k == Kind::Clip) e->level = "", e->pretty = "EDITH clip";
            else if (k == Kind::Legacy) e->pretty = "Older replay (" + U8(ext.substr(1)) + ")";
            if (e->level_id) {
                e->level = LevelName(e->level_id);
                e->pretty = PrettyLevel(e->level);
            }
            entries_.push_back(std::move(e));
        };
        for (const auto& it : fs::directory_iterator(dir, ec)) if (it.is_regular_file(ec)) add(it.path());
        for (const auto& it : fs::directory_iterator(dir / "clips", ec)) if (it.is_regular_file(ec)) add(it.path());
        std::sort(entries_.begin(), entries_.end(), [](auto& a, auto& b) { return a->sort_key > b->sort_key; });
        stop_ = false;
        worker_ = std::thread([this] { Peek(); });
    }
    void Stop() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
    }
    ~Library() { Stop(); }
    std::vector<std::unique_ptr<Entry>>& entries() { return entries_; }
    const fs::path& dir() const { return dir_; }

private:
    void Peek() {
        for (size_t i = 0; i < entries_.size() && !stop_; ++i) {
            Entry& e = *entries_[i];
            if (e.kind != Kind::Demo) {
                e.peeked = 1;
                continue;
            }
            ErvInfo info{};
            if (erv_peek(e.path.c_str(), &info)) {
                e.info = info;
                if (!e.level_id) {
                    e.level_id = info.level_id;
                    e.level = LevelName(info.level_id);
                    e.pretty = PrettyLevel(e.level);
                }
                e.peeked = 1;
            } else {
                e.peeked = -1;
            }
        }
    }
    std::mutex m_;
    std::vector<std::unique_ptr<Entry>> entries_;
    fs::path dir_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
};

// ---- the app -------------------------------------------------------------------------------------------------------
enum class Screen { Menu, Loading, Player };
enum class CamMode { Free, Follow, Pov, Disc, Top };
const char* kCamNames[] = {"Free cam", "Follow", "POV", "Disc", "Overhead"};

struct MapArt {
    std::string level;
    Texture sharp, blur;
};

struct App {
    HWND hwnd = nullptr;
    echo::render::PreviewPtr renderer;
    Gpu gpu;
    Levels levels;
    bool levels_ok = false;
    Library lib;
    ImFont *font = nullptr, *font_small = nullptr, *font_big = nullptr, *font_title = nullptr, *font_mono = nullptr;
    ImFont *font_h1 = nullptr, *font_hero = nullptr, *font_label = nullptr, *font_icon_big = nullptr;
    float dpi = 1.f;

    Screen screen = Screen::Menu;
    fs::path pending_open;
    std::string loading_text, status, error_text;
    double error_until = 0;

    // menu
    std::vector<MapArt> arts;   // per map: the sharp frame (cards, hero) and a blurred one (background)
    HANDLE preview_proc = nullptr;
    double next_poll = 0;
    std::string preview_progress;
    int menu_frames = 0;
    std::unordered_map<int, float> hover;   // card hover animation, 0..1
    int tab = 0, sort_mode = 0;
    char search[128] = {};
    int selected = -1;
    std::vector<std::wstring> recent;

    // player
    Replay replay;
    double t = 0, speed = 1;
    bool playing = true, loop = false;
    int part = 0;
    echo::render::FlyCamera cam;
    CamMode mode = CamMode::Free;
    int focus = -1;   // player index in the current part
    float orbit_yaw = 0, orbit_pitch = 0.25f, orbit_dist = 3.5f;
    // spectator options (Echo VR's spectator settings, roughly)
    float mouse_sens = 1.f, free_smooth = 8.f, follow_smooth = 6.f, follow_turn = 3.f, follow_height = 0.35f, pov_smooth = 4.f;
    bool invert_y = false;
    V3 free_vel{0, 0, 0}, sm_dir{0, 0, 1};
    float sm_heading = 0;
    int last_mode = -1, last_focus = -2;
    bool show_names = true, show_bodies = true, xray = false, show_trails = true, disc_trail = true, show_ui = true;
    bool show_players = true, show_events = true;
    float thickness = 1.f, fov_deg = 75.f, trail_s = 1.5f;
    int forward_axis = 2;
    std::vector<bool> hidden;
    std::vector<double> bookmarks;
    float cam_speed = 8.f;
    bool rmb = false;
    POINT last_mouse{};
    bool mouse_lock = false;            // free look: the mouse turns the camera with no button held (click the view, Esc releases)
    float raw_dx = 0, raw_dy = 0;       // raw mouse motion since the last frame, while looking
    bool raw_rel = false;               // a relative raw report arrived this frame
    float abs_dx = 0, abs_dy = 0;       // absolute raw reports (Virtual Desktop, RDP, tablets), as pixel deltas
    bool abs_seen = false, abs_have = false;
    float abs_x = 0, abs_y = 0;
    float pov_off_yaw = 0, pov_off_pitch = 0;   // POV: where the mouse turned your head, eased back when idle
    double pov_look_at = 0;
    bool panel_players = true, panel_view = true;
    int follow_view = 0;                // Follow: 0 behind, 1 overhead, 2 side
    std::wstring shot_path;
    std::wstring shot_views;   // --views <file>: lines "name eye_x eye_y eye_z at_x at_y at_z", each captured beside the shot
    int shot_frames = 0;
    int closeup = -1;
    std::vector<WireVert> wires;
    // real chassis (avatar.h): loaded once the game files are mounted, drawn instead of the wire bodies
    std::vector<Avatar> avatars;
    bool avatars_tried = false, show_models = true;
    double skin_ms = 0, boards_ms = 0;
    std::vector<echo::render::ActorMeshVertex> actor_v;
    std::vector<uint32_t> actor_i;
    std::vector<std::vector<uint32_t>> actor_parts;   // indices per material, joined into actor_i
    std::vector<echo::render::ActorDraw> actor_draws;
    std::vector<size_t> avatar_material_base;           // each avatar's first material
    std::vector<echo::render::ActorTexture> actor_textures;
    std::vector<echo::render::ActorMaterial> actor_materials;
    bool actor_materials_pending = false;
    double last_frame = 0;
    float hud_fade = 1.f;
    double last_input = 0;
};
App g;

double Now() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void Fail(const std::string& s) {
    g.error_text = s;
    g.error_until = Now() + 8;
}

// ---- fonts and style ---------------------------------------------------------------------------------------------
// Inter for text, JetBrains Mono for clocks, Lucide icons merged into each: the files EchoConstruct ships (ui\).
fs::path ExeDir() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    return fs::path(b).parent_path();
}

void SetupStyle() {
    ImGuiIO& io = ImGui::GetIO();
    const float s = g.dpi;
    const fs::path ui = ExeDir() / "ui";
    const std::string inter = U8((ui / "Inter-Regular.ttf").wstring()), inter_sb = U8((ui / "Inter-SemiBold.ttf").wstring());
    const std::string mono = U8((ui / "JetBrainsMono-Regular.ttf").wstring()), icons = U8((ui / "lucide.ttf").wstring());
    auto exists = [](const std::string& f) { return GetFileAttributesW(W(f).c_str()) != INVALID_FILE_ATTRIBUTES; };
    static const ImWchar icon_range[] = {ICON_MIN_LC, ICON_MAX_LC, 0};
    auto font = [&](const std::string& file, const char* fallback, float px, bool with_icons = true) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        ImFont* f = nullptr;
        if (exists(file)) f = io.Fonts->AddFontFromFileTTF(file.c_str(), px * s, &cfg);
        else if (GetFileAttributesA(fallback) != INVALID_FILE_ATTRIBUTES) f = io.Fonts->AddFontFromFileTTF(fallback, px * s, &cfg);
        if (!f) return nullptr;
        if (with_icons && exists(icons)) {
            ImFontConfig ic;
            ic.MergeMode = true;
            ic.PixelSnapH = true;
            ic.GlyphOffset = {0, px * s * 0.12f};
            ic.GlyphMinAdvanceX = px * s;
            io.Fonts->AddFontFromFileTTF(icons.c_str(), px * s * 0.95f, &ic, icon_range);
        }
        return f;
    };
    g.font = font(inter, "C:\\Windows\\Fonts\\segoeui.ttf", 15.5f);
    if (!g.font) g.font = io.Fonts->AddFontDefault();
    g.font_small = font(inter, "C:\\Windows\\Fonts\\segoeui.ttf", 13.f);
    g.font_label = font(inter_sb, "C:\\Windows\\Fonts\\seguisb.ttf", 11.5f);
    g.font_big = font(inter_sb, "C:\\Windows\\Fonts\\seguisb.ttf", 17.f);
    g.font_h1 = font(inter_sb, "C:\\Windows\\Fonts\\seguisb.ttf", 28.f);
    g.font_hero = font(inter_sb, "C:\\Windows\\Fonts\\seguisb.ttf", 46.f);
    g.font_title = font(inter_sb, "C:\\Windows\\Fonts\\segoeuib.ttf", 22.f);
    g.font_mono = font(mono, "C:\\Windows\\Fonts\\consola.ttf", 14.f, false);
    g.font_icon_big = exists(icons) ? io.Fonts->AddFontFromFileTTF(icons.c_str(), 34.f * s, nullptr, icon_range) : nullptr;
    for (ImFont** f : {&g.font_small, &g.font_label, &g.font_big, &g.font_h1, &g.font_hero, &g.font_title, &g.font_mono, &g.font_icon_big})
        if (!*f) *f = g.font;

    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 14, st.ChildRounding = 10, st.FrameRounding = 8, st.PopupRounding = 10, st.GrabRounding = 8;
    st.ScrollbarRounding = 8, st.TabRounding = 8;
    st.WindowBorderSize = 0, st.FrameBorderSize = 0, st.ChildBorderSize = 0, st.PopupBorderSize = 1;
    st.WindowPadding = {16, 14}, st.FramePadding = {12, 7}, st.ItemSpacing = {8, 8}, st.ItemInnerSpacing = {8, 6};
    st.ScrollbarSize = 10, st.GrabMinSize = 10;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = {0.055f, 0.065f, 0.085f, 0.90f};
    c[ImGuiCol_ChildBg] = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_PopupBg] = {0.07f, 0.08f, 0.105f, 0.98f};
    c[ImGuiCol_Border] = {1.f, 1.f, 1.f, 0.08f};
    c[ImGuiCol_FrameBg] = {1.f, 1.f, 1.f, 0.06f};
    c[ImGuiCol_FrameBgHovered] = {1.f, 1.f, 1.f, 0.09f};
    c[ImGuiCol_FrameBgActive] = {1.f, 1.f, 1.f, 0.12f};
    c[ImGuiCol_Button] = {1.f, 1.f, 1.f, 0.07f};
    c[ImGuiCol_ButtonHovered] = {1.f, 1.f, 1.f, 0.13f};
    c[ImGuiCol_ButtonActive] = {1.f, 1.f, 1.f, 0.18f};
    c[ImGuiCol_Header] = {0.30f, 0.86f, 1.f, 0.16f};
    c[ImGuiCol_HeaderHovered] = {1.f, 1.f, 1.f, 0.08f};
    c[ImGuiCol_HeaderActive] = {0.30f, 0.86f, 1.f, 0.28f};
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = {0.6f, 0.95f, 1.f, 1.f};
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_Tab] = {1.f, 1.f, 1.f, 0.04f};
    c[ImGuiCol_TabHovered] = {0.30f, 0.86f, 1.f, 0.30f};
    c[ImGuiCol_TabSelected] = {0.30f, 0.86f, 1.f, 0.22f};
    c[ImGuiCol_Separator] = {1.f, 1.f, 1.f, 0.07f};
    c[ImGuiCol_Text] = {0.93f, 0.95f, 0.98f, 1.f};
    c[ImGuiCol_TextDisabled] = {0.54f, 0.59f, 0.66f, 1.f};
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = {1.f, 1.f, 1.f, 0.10f};
    c[ImGuiCol_ScrollbarGrabHovered] = {1.f, 1.f, 1.f, 0.18f};
    c[ImGuiCol_ScrollbarGrabActive] = {1.f, 1.f, 1.f, 0.25f};
    c[ImGuiCol_NavHighlight] = {0.30f, 0.86f, 1.f, 0.6f};
    st.ScaleAllSizes(g.dpi);
}

bool AccentButton(const char* label, ImVec2 size = {0, 0}) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.24f, 0.78f, 0.98f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.42f, 0.86f, 1.f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.55f, 0.92f, 1.f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.02f, 0.06f, 0.10f, 1.f});
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// ---- map art ---------------------------------------------------------------------------------------------------------
// Static art shipped beside the exe: maps\<level>.png for a particular map, else maps\arena.png, combat.png, lobby.png
// for its type. Nothing is rendered at run time.
bool LoadPng(const fs::path& file, Image& im) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    im.w = w, im.h = h;
    im.rgba.resize(size_t(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(im.rgba.size()), im.rgba.data()));
}

void LoadBackgrounds() {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ExeDir() / "maps", ec)) {
        if (_wcsicmp(e.path().extension().c_str(), L".png") != 0) continue;
        const std::string name = U8(e.path().stem().wstring());
        bool have = false;
        for (auto& a : g.arts) have |= a.level == name;
        if (have) continue;
        Image im;
        if (!LoadPng(e.path(), im)) continue;
        MapArt art;
        art.level = name;
        const Image soft = Levels::Blurred(im);
        if (g.gpu.Upload(im.rgba.data(), im.w, im.h, art.sharp) && g.gpu.Upload(soft.rgba.data(), soft.w, soft.h, art.blur))
            g.arts.push_back(std::move(art));
    }
}
const MapArt* ArtFor(const std::string& level) {
    const MapArt* best = nullptr;   // the map's own art, or its parent map's (fission_cargobay -> fission)
    for (auto& a : g.arts)
        if (a.level.rfind("mpl_", 0) == 0 && level.rfind(a.level, 0) == 0 && (!best || a.level.size() > best->level.size())) best = &a;
    if (best) return best;
    const char* type = level == "mpl_arena_a" || level.rfind("mpl_lobby_b_arena", 0) == 0 ? "arena"
                       : level.rfind("mpl_combat", 0) == 0                                ? "combat"
                       : level.rfind("mpl_lobby", 0) == 0                                 ? "lobby"
                                                                                          : nullptr;
    if (!type) return nullptr;
    for (auto& a : g.arts)
        if (a.level == type) return &a;
    return nullptr;
}
void StartPreviewMaker() {
    if (g.preview_proc || !g.levels_ok) return;
    bool missing = false;
    for (const auto& [id, name] : KnownMaps()) missing |= !Levels::HasPreview(name);
    if (!missing) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --make-previews";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        g.preview_proc = pi.hProcess;
    }
}

void TickPreviews(double now) {
    if (!g.preview_proc || now < g.next_poll) return;
    g.next_poll = now + 0.5;
    LoadBackgrounds();
    std::ifstream f(AppDir() / "previews.progress");
    std::getline(f, g.preview_progress);
    if (WaitForSingleObject(g.preview_proc, 0) == WAIT_OBJECT_0) {
        CloseHandle(g.preview_proc);
        g.preview_proc = nullptr;
        g.preview_progress.clear();
        LoadBackgrounds();
    }
}

// The menu's backdrop: blurred map art, crossfading every 9 s with a slow push-in and drift.
void DrawBackground(ImDrawList* dl, ImVec2 size, double now) {
    dl->AddRectFilled({0, 0}, size, Rgba(0.025f, 0.03f, 0.045f));
    if (!g.arts.empty()) {
        const double period = 9.0, fade = 2.0;
        const size_t n = g.arts.size();
        const size_t i = size_t(now / period) % n;
        const double into = fmod(now, period);
        auto draw = [&](const Texture& tex, float alpha, double phase, int k) {
            const float t01 = float(phase / (period + fade));
            const float zoom = 1.08f + 0.08f * t01;
            const float aspect = size.x / size.y, ta = float(tex.w) / float(tex.h);
            float uw = 1.f / zoom, vh = 1.f / zoom;
            if (aspect > ta) vh *= ta / aspect;
            else uw *= aspect / ta;
            const float dir = (k & 1) ? -1.f : 1.f;
            const float panx = dir * 0.03f * (t01 - 0.5f), pany = 0.015f * (t01 - 0.5f);
            const ImVec2 uv0{0.5f - uw / 2 + panx, 0.5f - vh / 2 + pany}, uv1{0.5f + uw / 2 + panx, 0.5f + vh / 2 + pany};
            dl->AddImage(tex.id, {0, 0}, size, uv0, uv1, Rgba(1, 1, 1, alpha));
        };
        draw(g.arts[i].blur, 1.f, into + fade, int(i));
        if (into > period - fade) draw(g.arts[(i + 1) % n].blur, float((into - (period - fade)) / fade), into - (period - fade), int(i + 1));
    }
    // tint and vignette: navy wash, darker to the left (the rail) and the bottom
    dl->AddRectFilled({0, 0}, size, Rgba(0.02f, 0.035f, 0.07f, 0.45f));
    dl->AddRectFilledMultiColor({0, 0}, {size.x * 0.55f, size.y}, Rgba(0, 0, 0, 0.55f), Rgba(0, 0, 0, 0.0f), Rgba(0, 0, 0, 0.0f), Rgba(0, 0, 0, 0.55f));
    dl->AddRectFilledMultiColor({0, size.y * 0.45f}, size, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0.6f), Rgba(0, 0, 0, 0.6f));
}

// ---- opening a replay -----------------------------------------------------------------------------------------------
fs::path ConvertLegacy(const fs::path& in) {
    const fs::path out = AppDir() / "imported" / (in.stem().wstring() + L".echodemo");
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (fs::exists(out, ec)) return out;
    const std::wstring exe = std::wstring(ERV_ECHOREPLAY_BIN) + L"\\echodemo.exe";
    std::wstring cmd = L"\"" + exe + L"\" convert \"" + in.wstring() + L"\" \"" + out.wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return {};
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0 && fs::exists(out, ec) ? out : fs::path{};
}

void ResetCamera() {
    g.cam = g.levels.Overview(float(g.gpu.width) / float(std::max(1u, g.gpu.height)));
    g.mode = CamMode::Free;
    g.focus = -1;
}

bool EnsurePartLevel() {
    if (g.replay.parts.empty()) return false;
    const Part& p = g.replay.parts[g.part];
    if (p.level_name.empty()) return true;   // unknown map: players only
    if (g.levels.current() == p.level_name) return true;
    if (!g.levels_ok) return false;
    if (!g.levels.Load(p.level_name)) {
        Fail("Could not load " + p.level_name + ": " + g.levels.why());
        return false;
    }
    return true;
}

// Opening runs on a worker thread (converting, reading the parts, loading the map: up to half a minute for the combat
// maps and the lobby) so the window keeps drawing the loading screen and answering Windows. While it runs the worker
// owns the replay it builds and g.levels; the loading screen touches neither, and nothing renders the level until
// FinishOpen hands the result over on the UI thread.
void Seek(double t);

struct OpenJob {
    fs::path file;
    std::thread worker;
    std::atomic<bool> done{false};
    std::atomic<const char*> stage{"Reading the replay"};
    std::string level_label;   // "Loading <map>" once the replay is read (set before stage points at it)
    Replay replay;
    int forward_axis = 0;
    std::string error;         // fatal: back to the menu
    std::string level_error;   // the map would not load: the replay still plays (players only)
};
std::unique_ptr<OpenJob> g_open;

void OpenWork(OpenJob& j) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // (the map's textures can go through WIC)
    fs::path path = j.file;
    std::wstring ext = path.extension().wstring();
    for (auto& c : ext) c = wchar_t(towlower(c));
    if (ext == L".echoreplay" || ext == L".tape") {
        j.stage = "Converting the old format";
        path = ConvertLegacy(j.file);
        if (path.empty()) j.error = "Could not convert " + U8(j.file.filename().wstring()) + " (needs EchoReplay's echodemo.exe)";
    }
    if (j.error.empty() && !j.replay.Load(path)) j.error = "Could not open " + U8(j.file.filename().wstring()) + ": " + j.replay.error;
    if (j.error.empty()) {
        j.forward_axis = GuessForwardAxis(j.replay);
        const std::string level = j.replay.parts.empty() ? std::string() : j.replay.parts[0].level_name;
        if (!level.empty() && g.levels_ok && g.levels.current() != level) {
            j.level_label = "Loading " + PrettyLevel(level);
            j.stage = j.level_label.c_str();
            if (!g.levels.Load(level)) j.level_error = "Could not load " + level + ": " + g.levels.why();
        }
    }
    CoUninitialize();
    j.done = true;
}

void StartOpen(const fs::path& file) {
    g_open = std::make_unique<OpenJob>();
    g_open->file = file;
    g_open->worker = std::thread(OpenWork, std::ref(*g_open));
}

// On the UI thread, once the worker is done: the replay goes live (or the menu shows why it could not).
void FinishOpen(double seek) {
    std::unique_ptr<OpenJob> j = std::move(g_open);
    j->worker.join();
    if (!j->error.empty()) {
        Fail(j->error);
        g.screen = Screen::Menu;
        return;
    }
    g.replay = std::move(j->replay);
    g.t = 0, g.part = 0, g.playing = true, g.speed = 1;
    g.bookmarks.clear();
    g.forward_axis = j->forward_axis;
    if (!j->level_error.empty()) Fail(j->level_error);
    ResetCamera();
    g.screen = Screen::Player;
    IniSet(L"last_file", j->file.wstring());
    if (seek > 0) Seek(seek);
}

void RequestOpen(const fs::path& p) {
    if (g_open) return;   // one at a time: the current open finishes first
    g.pending_open = p;
    g.loading_text = "Loading " + U8(p.filename().wstring());
    g.screen = Screen::Loading;
}

// ---- the menu --------------------------------------------------------------------------------------------------------
void SetFolder(const std::wstring& dir) {
    IniSet(L"folder", dir);
    g.recent.erase(std::remove(g.recent.begin(), g.recent.end(), dir), g.recent.end());
    g.recent.insert(g.recent.begin(), dir);
    if (g.recent.size() > 6) g.recent.resize(6);
    std::wstring joined;
    for (auto& r : g.recent) joined += r + L"|";
    IniSet(L"recent", joined);
    g.selected = -1;
    g.lib.Scan(dir);
}

void MountGame();

enum Cat { kCatArena, kCatCombat, kCatLobby, kCatClip, kCatLegacy, kCatOther };
Cat CategoryOf(const Entry& e) {
    if (e.kind == Kind::Clip) return kCatClip;
    if (e.kind == Kind::Legacy) return kCatLegacy;
    if (e.level == "mpl_arena_a") return kCatArena;
    if (e.level.rfind("mpl_combat", 0) == 0) return kCatCombat;
    if (e.level.rfind("mpl_lobby", 0) == 0) return kCatLobby;
    return kCatOther;
}
ImVec4 CatCol(Cat c) {
    switch (c) {
        case kCatArena: return kAccent;
        case kCatCombat: return {1.f, 0.42f, 0.34f, 1};
        case kCatLobby: return {0.56f, 0.88f, 0.50f, 1};
        case kCatClip: return {0.98f, 0.42f, 0.80f, 1};
        case kCatLegacy: return {0.66f, 0.68f, 0.74f, 1};
        default: return kGrey;
    }
}
const char* CatName(Cat c) {
    switch (c) {
        case kCatArena: return "ARENA";
        case kCatCombat: return "COMBAT";
        case kCatLobby: return "SOCIAL LOBBY";
        case kCatClip: return "EDITH CLIP";
        case kCatLegacy: return "OLDER FORMAT";
        default: return "REPLAY";
    }
}
const char* CatIcon(Cat c) {
    switch (c) {
        case kCatArena: return ICON_LC_DISC;
        case kCatCombat: return ICON_LC_SWORDS;
        case kCatLobby: return ICON_LC_USERS;
        case kCatClip: return ICON_LC_CLAPPERBOARD;
        case kCatLegacy: return ICON_LC_ARCHIVE;
        default: return ICON_LC_FILM;
    }
}

struct NavItem {
    const char* icon;
    const char* name;
};
const NavItem kNav[] = {{ICON_LC_HOUSE, "All replays"}, {ICON_LC_DISC, "Arena"},       {ICON_LC_SWORDS, "Combat"},
                        {ICON_LC_USERS, "Social Lobby"}, {ICON_LC_CLAPPERBOARD, "EDITH clips"}, {ICON_LC_ARCHIVE, "Older formats"}};

bool InTab(const Entry& e, int tab) {
    const Cat c = CategoryOf(e);
    switch (tab) {
        case 1: return c == kCatArena;
        case 2: return c == kCatCombat;
        case 3: return c == kCatLobby;
        case 4: return c == kCatClip;
        case 5: return c == kCatLegacy;
        default: return true;
    }
}
bool Matches(const Entry& e) {
    if (!InTab(e, g.tab)) return false;
    if (g.search[0]) {
        std::string hay = e.file + " " + e.pretty + " " + e.when + " " + CatName(CategoryOf(e));
        std::string needle = g.search;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
        if (hay.find(needle) == std::string::npos) return false;
    }
    return true;
}

std::string Size(uint64_t b) {
    char s[32];
    if (b > (1u << 30)) snprintf(s, sizeof s, "%.1f GB", b / 1073741824.0);
    else if (b > (1u << 20)) snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    else snprintf(s, sizeof s, "%.0f KB", b / 1024.0);
    return s;
}

// "Today", "Yesterday", "Thursday 12 November" from an entry's "YYYY-MM-DD  HH:MM".
std::string DayLabel(const std::string& when) {
    if (when.size() < 10) return "Other";
    SYSTEMTIME now;
    GetLocalTime(&now);
    auto days = [](int y, int m, int d) {   // days since a fixed epoch (civil)
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
        const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe;
    };
    const int y = atoi(when.substr(0, 4).c_str()), m = atoi(when.substr(5, 2).c_str()), d = atoi(when.substr(8, 2).c_str());
    const int diff = days(now.wYear, now.wMonth, now.wDay) - days(y, m, d);
    if (diff == 0) return "Today";
    if (diff == 1) return "Yesterday";
    static const char* wd[] = {"Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday"};
    static const char* mo[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    const int dn = days(y, m, d);
    std::string s = std::string(wd[((dn % 7) + 7) % 7]) + " " + std::to_string(d) + " " + mo[std::clamp(m, 1, 12) - 1];
    if (y != now.wYear) s += " " + std::to_string(y);
    return s;
}

ImU32 U32c(ImVec4 c, float a = -1) { return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, a < 0 ? c.w : a}); }
ImVec2 TextSize(ImFont* f, const char* t) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, t); }

// Cover-fit UVs for an image of aspect `ta` in a box of aspect `ba`.
void Cover(float ta, float ba, ImVec2& uv0, ImVec2& uv1, float zoom = 1.f) {
    float uw = 1.f / zoom, vh = 1.f / zoom;
    if (ba > ta) vh *= ta / ba;
    else uw *= ba / ta;
    uv0 = {0.5f - uw / 2, 0.5f - vh / 2}, uv1 = {0.5f + uw / 2, 0.5f + vh / 2};
}

// A map image (or a tinted placeholder with the category icon) filling a rounded box.
void DrawThumb(ImDrawList* dl, const Entry& e, ImVec2 a, ImVec2 b, float round, ImDrawFlags corners, float zoom = 1.f, bool sharp = true) {
    const MapArt* art = ArtFor(e.level);
    const Cat c = CategoryOf(e);
    const ImVec4 col = CatCol(c);
    if (art) {
        const Texture& t = sharp ? art->sharp : art->blur;
        ImVec2 uv0, uv1;
        Cover(float(t.w) / t.h, (b.x - a.x) / (b.y - a.y), uv0, uv1, zoom);
        dl->AddImageRounded(t.id, a, b, uv0, uv1, Rgba(1, 1, 1), round, corners);
    } else {
        dl->AddRectFilledMultiColor(a, b, U32c({col.x * 0.30f, col.y * 0.30f, col.z * 0.30f, 1}), U32c({col.x * 0.12f, col.y * 0.12f, col.z * 0.16f, 1}),
                                    U32c({0.03f, 0.04f, 0.06f, 1}), U32c({col.x * 0.18f, col.y * 0.18f, col.z * 0.22f, 1}));
        // AddRectFilledMultiColor has no rounding: round it off with the panel colour at the corners
        dl->AddRect(a, b, Rgba(0.04f, 0.05f, 0.07f, 1), round, corners, 2.5f);
        const char* ic = CatIcon(c);
        ImFont* f = g.font_icon_big;
        const float px = std::min(b.y - a.y, b.x - a.x) * 0.36f;
        const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0, ic);
        dl->AddText(f, px, {(a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2}, U32c(col, 0.55f), ic);
    }
}

void Chip(ImDrawList* dl, ImVec2& at, const char* text, ImVec4 col, bool solid = false) {
    const float s = g.dpi;
    const ImVec2 ts = TextSize(g.font_label, text);
    const ImVec2 a = at, b{at.x + ts.x + 16 * s, at.y + ts.y + 8 * s};
    dl->AddRectFilled(a, b, solid ? U32c(col, 0.95f) : U32c(col, 0.18f), 6 * s);
    dl->AddText(g.font_label, g.font_label->FontSize, {a.x + 8 * s, a.y + 4 * s}, solid ? Rgba(0.02f, 0.04f, 0.07f) : U32c(col), text);
    at.x = b.x + 6 * s;
}

// A flat button: icon + label, ghost style (outline on hover), returns clicked.
bool GhostButton(const char* label, ImVec2 size = {0, 0}) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{1, 1, 1, 0.06f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1, 1, 1, 0.12f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{1, 1, 1, 0.18f});
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

void ChooseEcho() {
    std::wstring d;
    if (!PickFolder(g.hwnd, d)) return;
    const fs::path install = EchoInstallFor(d);
    if (install.empty()) {
        Fail("That folder is not an Echo VR install: it needs _data\\...\\rad15\\win10 with manifests and packages.");
        return;
    }
    IniSet(L"echo", install.wstring());
    MountGame();
    g.arts.clear();
    LoadBackgrounds();
}

void Rail(ImVec2 size, double now) {
    const float s = g.dpi, w = 256 * s;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled({0, 0}, {w, size.y}, Rgba(0.02f, 0.025f, 0.04f, 0.78f));
    dl->AddLine({w, 0}, {w, size.y}, Rgba(1, 1, 1, 0.06f));

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({w, size.y});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0, 0, 0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{18 * s, 26 * s});
    ImGui::Begin("##rail", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* wl = ImGui::GetWindowDrawList();
    // logo
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float box = 40 * s;
        wl->AddRectFilledMultiColor(p, {p.x + box, p.y + box}, U32c(kAccent), U32c({0.45f, 0.55f, 1.f, 1}), U32c({0.65f, 0.40f, 1.f, 1}), U32c(kAccent));
        wl->AddRect(p, {p.x + box, p.y + box}, Rgba(0.02f, 0.025f, 0.04f, 1), 10 * s, 0, 3 * s);
        const ImVec2 ts = TextSize(g.font_big, ICON_LC_PLAY);
        wl->AddText(g.font_big, g.font_big->FontSize, {p.x + (box - ts.x) / 2 + 1 * s, p.y + (box - ts.y) / 2}, Rgba(0.02f, 0.04f, 0.08f), ICON_LC_PLAY);
        wl->AddText(g.font_title, g.font_title->FontSize, {p.x + box + 12 * s, p.y - 1 * s}, Rgba(1, 1, 1), "Echo Replay");
        wl->AddText(g.font_label, g.font_label->FontSize, {p.x + box + 13 * s, p.y + g.font_title->FontSize + 1 * s}, U32c(kAccent, 0.9f),
                    "V I E W E R");
        ImGui::Dummy({box, box + 22 * s});
    }
    // library
    ImGui::PushFont(g.font_label);
    ImGui::TextDisabled("LIBRARY");
    ImGui::PopFont();
    ImGui::Dummy({0, 2 * s});
    auto& es = g.lib.entries();
    for (int i = 0; i < 6; ++i) {
        int count = 0;
        for (auto& e : es) count += InTab(*e, i);
        if (i == 5 && count == 0 && g.tab != 5) continue;   // hide "Older formats" when there are none
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float iw = ImGui::GetContentRegionAvail().x, ih = 40 * s;
        const bool clicked = ImGui::InvisibleButton("##nav", {iw, ih});
        const bool hov = ImGui::IsItemHovered(), on = g.tab == i;
        if (clicked) g.tab = i, g.selected = -1;
        if (on) {
            wl->AddRectFilled(p, {p.x + iw, p.y + ih}, U32c(kAccent, 0.14f), 9 * s);
            wl->AddRectFilled({p.x, p.y + 10 * s}, {p.x + 3 * s, p.y + ih - 10 * s}, U32c(kAccent), 2 * s);
        } else if (hov) {
            wl->AddRectFilled(p, {p.x + iw, p.y + ih}, Rgba(1, 1, 1, 0.05f), 9 * s);
        }
        const ImU32 tc = on ? Rgba(1, 1, 1) : hov ? Rgba(0.9f, 0.93f, 0.97f) : Rgba(0.68f, 0.73f, 0.80f);
        const float ty = p.y + (ih - g.font->FontSize) / 2;
        wl->AddText(g.font, g.font->FontSize, {p.x + 14 * s, ty}, on ? U32c(kAccent) : tc, kNav[i].icon);
        wl->AddText(g.font, g.font->FontSize, {p.x + 44 * s, ty}, tc, kNav[i].name);
        const std::string n = std::to_string(count);
        const ImVec2 ns = TextSize(g.font_small, n.c_str());
        wl->AddRectFilled({p.x + iw - ns.x - 22 * s, p.y + (ih - ns.y) / 2 - 3 * s}, {p.x + iw - 8 * s, p.y + (ih + ns.y) / 2 + 3 * s},
                          on ? U32c(kAccent, 0.22f) : Rgba(1, 1, 1, 0.06f), 8 * s);
        wl->AddText(g.font_small, g.font_small->FontSize, {p.x + iw - ns.x - 15 * s, p.y + (ih - ns.y) / 2}, on ? U32c(kAccent) : Rgba(0.6f, 0.65f, 0.72f), n.c_str());
        ImGui::PopID();
        ImGui::Dummy({0, 1 * s});
    }

    // folder + Echo cards, pinned to the bottom
    const float cardsH = 236 * s;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 16 * s, size.y - cardsH - 20 * s));
    auto card = [&](const char* label, const char* icon, ImVec4 dot, const std::string& title, const std::string& sub, const char* id) -> int {
        int action = 0;
        ImGui::PushFont(g.font_label);
        ImGui::TextDisabled("%s", label);
        ImGui::PopFont();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cw = ImGui::GetContentRegionAvail().x, ch = 84 * s;
        wl->AddRectFilled(p, {p.x + cw, p.y + ch}, Rgba(1, 1, 1, 0.045f), 10 * s);
        wl->AddRect(p, {p.x + cw, p.y + ch}, Rgba(1, 1, 1, 0.06f), 10 * s);
        wl->AddText(g.font, g.font->FontSize, {p.x + 12 * s, p.y + 11 * s}, U32c(dot), icon);
        wl->PushClipRect({p.x, p.y}, {p.x + cw - 10 * s, p.y + ch}, true);
        wl->AddText(g.font_big, g.font_big->FontSize, {p.x + 38 * s, p.y + 9 * s}, Rgba(1, 1, 1), title.c_str());
        wl->AddText(g.font_small, g.font_small->FontSize, {p.x + 12 * s, p.y + 34 * s}, Rgba(0.58f, 0.63f, 0.70f), sub.c_str());
        wl->PopClipRect();
        ImGui::SetCursorScreenPos({p.x + 10 * s, p.y + ch - 32 * s});
        ImGui::PushID(id);
        ImGui::PushFont(g.font_small);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10 * s, 4 * s});
        if (GhostButton(ICON_LC_FOLDER_OPEN "  Change")) action = 1;
        ImGui::SameLine(0, 6 * s);
        if (GhostButton(ICON_LC_MAXIMIZE "  Open")) action = 2;
        ImGui::PopStyleVar();
        ImGui::PopFont();
        ImGui::PopID();
        ImGui::SetCursorScreenPos({p.x, p.y + ch + 14 * s});
        return action;
    };
    const std::wstring dir = g.lib.dir().wstring();
    const std::string fname = U8(fs::path(dir).filename().wstring());
    const int fa = card("REPLAY FOLDER", ICON_LC_FOLDER, kAccent, fname.empty() ? U8(dir) : fname, U8(dir), "folder");
    if (fa == 1) ImGui::OpenPopup("##folders");
    if (fa == 2) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (ImGui::BeginPopup("##folders")) {
        if (ImGui::Selectable(ICON_LC_FOLDER_OPEN "  Browse for a folder...")) {
            std::wstring d;
            if (PickFolder(g.hwnd, d)) SetFolder(d);
        }
        if (g.recent.size() > 1) {
            ImGui::Separator();
            ImGui::TextDisabled("Recent");
            for (size_t i = 0; i < g.recent.size(); ++i) {
                if (g.recent[i] == dir) continue;
                ImGui::PushID(int(i));
                if (ImGui::Selectable((std::string(ICON_LC_FOLDER "  ") + U8(g.recent[i])).c_str())) SetFolder(g.recent[i]);
                ImGui::PopID();
            }
        }
        ImGui::EndPopup();
    }
    const auto installs = gamefiles::Installs();
    const bool echo_ok = g.levels_ok;
    const std::string etitle = echo_ok ? "Reading game files" : "Echo VR not found";
    const std::string esub = echo_ok && !installs.empty() ? U8(installs.front().wstring()) : "Choose your Echo VR folder";
    const int ea = card("ECHO VR", echo_ok ? ICON_LC_CIRCLE_CHECK : ICON_LC_TRIANGLE_ALERT, echo_ok ? ImVec4{0.4f, 0.9f, 0.5f, 1} : ImVec4{1, 0.65f, 0.3f, 1}, etitle, esub, "echo");
    if (ea == 1) ChooseEcho();
    if (ea == 2 && !installs.empty()) ShellExecuteW(nullptr, L"open", installs.front().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (ImGui::IsItemHovered() && !echo_ok) ImGui::SetTooltip("%s", g.levels.why().c_str());
    (void)now;
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Hero(Entry& e, ImVec2 a, ImVec2 b) {
    const float s = g.dpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 18 * s;
    DrawThumb(dl, e, a, b, r, ImDrawFlags_RoundCornersAll, 1.0f);
    // legibility: dark from the left and the bottom
    dl->AddRectFilledMultiColor(a, {a.x + (b.x - a.x) * 0.70f, b.y}, Rgba(0.02f, 0.03f, 0.05f, 0.88f), Rgba(0.02f, 0.03f, 0.05f, 0.0f),
                                Rgba(0.02f, 0.03f, 0.05f, 0.0f), Rgba(0.02f, 0.03f, 0.05f, 0.88f));
    dl->AddRectFilledMultiColor({a.x, a.y + (b.y - a.y) * 0.5f}, b, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0.45f), Rgba(0, 0, 0, 0.45f));
    dl->AddRect(a, b, Rgba(1, 1, 1, 0.08f), r, 0, 1.f);

    const Cat c = CategoryOf(e);
    const float pad = 34 * s;
    ImVec2 at{a.x + pad, a.y + pad};
    Chip(dl, at, CatName(c), CatCol(c), true);
    if (e.peeked == 1 && e.info.voice_count) Chip(dl, at, ICON_LC_MIC " VOICE", {0.45f, 0.95f, 0.55f, 1});
    if (e.peeked == 1 && e.info.recovered) Chip(dl, at, ICON_LC_TRIANGLE_ALERT " RECOVERED", {1, 0.78f, 0.3f, 1});
    float y = a.y + pad + 34 * s;
    const std::string title = e.pretty.empty() ? e.file : e.pretty;
    dl->AddText(g.font_hero, g.font_hero->FontSize, {a.x + pad - 2 * s, y}, Rgba(1, 1, 1), title.c_str());
    y += g.font_hero->FontSize + 8 * s;
    std::string meta;
    if (!e.when.empty()) meta += std::string(ICON_LC_CALENDAR "  ") + DayLabel(e.when) + ", " + e.when.substr(12) + "        ";
    if (e.peeked == 1 && e.kind == Kind::Demo) meta += std::string(ICON_LC_CLOCK "  ") + Clock(e.info.duration_s) + "        ";
    meta += std::string(ICON_LC_HARD_DRIVE "  ") + Size(e.bytes);
    dl->AddText(g.font, g.font->FontSize, {a.x + pad, y}, Rgba(0.82f, 0.86f, 0.92f), meta.c_str());
    y += g.font->FontSize + 26 * s;

    ImGui::SetCursorScreenPos({a.x + pad, y});
    ImGui::PushFont(g.font_big);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{26 * s, 13 * s});
    if (AccentButton(ICON_LC_PLAY "   Watch replay")) RequestOpen(e.path);
    ImGui::SameLine(0, 10 * s);
    if (GhostButton(ICON_LC_FOLDER_OPEN "   Show file")) {
        const std::wstring arg = L"/select,\"" + e.path.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    // file name, small, bottom right
    const ImVec2 fs2 = TextSize(g.font_small, e.file.c_str());
    dl->AddText(g.font_small, g.font_small->FontSize, {b.x - fs2.x - 22 * s, b.y - fs2.y - 18 * s}, Rgba(1, 1, 1, 0.45f), e.file.c_str());
}

// One replay card: map image with duration badge and tags, map name and time below. Returns true on double-click.
void Card(int index, Entry& e, ImVec2 p, float w, float thumbH, double now) {
    const float s = g.dpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float textH = 56 * s, h = thumbH + textH;
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(index);
    const bool clicked = ImGui::InvisibleButton("##card", {w, h});
    const bool hov = ImGui::IsItemHovered();
    if (clicked) g.selected = index;
    if (hov && ImGui::IsMouseDoubleClicked(0)) RequestOpen(e.path);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopID();
    float& anim = g.hover[index];
    anim += (float(hov) - anim) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
    const bool sel = g.selected == index;
    const float lift = 4 * s * anim;
    const ImVec2 a{p.x, p.y - lift}, b{p.x + w, p.y + thumbH - lift};
    const float r = 12 * s;
    if (anim > 0.01f) dl->AddRectFilled({a.x - 2, a.y + 6 * s}, {b.x + 2, b.y + 14 * s}, Rgba(0, 0, 0, 0.35f * anim), r);   // shadow
    DrawThumb(dl, e, a, b, r, ImDrawFlags_RoundCornersAll, 1.f + 0.05f * anim);
    dl->AddRectFilledMultiColor({a.x, a.y + thumbH * 0.55f}, b, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0.55f), Rgba(0, 0, 0, 0.55f));
    const Cat c = CategoryOf(e);
    const ImVec4 col = CatCol(c);
    // tags, top left
    ImVec2 at{a.x + 10 * s, a.y + 10 * s};
    if (c == kCatClip) Chip(dl, at, "CLIP", col, true);
    if (e.peeked == 1 && e.info.voice_count) Chip(dl, at, ICON_LC_MIC, {0.45f, 0.95f, 0.55f, 1});
    if (e.peeked == 1 && e.info.recovered) Chip(dl, at, ICON_LC_TRIANGLE_ALERT, {1, 0.78f, 0.3f, 1});
    if (e.peeked == -1) Chip(dl, at, "UNREADABLE", {1, 0.4f, 0.4f, 1}, true);
    // duration, bottom right
    std::string dur = e.peeked == 1 && e.kind == Kind::Demo ? Clock(e.info.duration_s) : e.peeked == 0 && e.kind == Kind::Demo ? "..." : "";
    if (!dur.empty()) {
        const ImVec2 ts = TextSize(g.font_mono, dur.c_str());
        const ImVec2 da{b.x - ts.x - 18 * s, b.y - ts.y - 14 * s};
        dl->AddRectFilled(da, {b.x - 8 * s, b.y - 8 * s}, Rgba(0.02f, 0.03f, 0.05f, 0.82f), 6 * s);
        dl->AddText(g.font_mono, g.font_mono->FontSize, {da.x + 5 * s, da.y + 3 * s}, Rgba(1, 1, 1), dur.c_str());
    }
    // play button on hover
    if (anim > 0.02f) {
        const ImVec2 cc{(a.x + b.x) / 2, (a.y + b.y) / 2};
        const float rad = 24 * s * (0.85f + 0.15f * anim);
        dl->AddCircleFilled(cc, rad, U32c(kAccent, 0.95f * anim), 40);
        const ImVec2 ts = TextSize(g.font_big, ICON_LC_PLAY);
        dl->AddText(g.font_big, g.font_big->FontSize, {cc.x - ts.x / 2 + 2 * s, cc.y - ts.y / 2}, Rgba(0.02f, 0.05f, 0.09f, anim), ICON_LC_PLAY);
    }
    // outline: selected solid accent, hover soft
    if (sel) dl->AddRect({a.x - 1, a.y - 1}, {b.x + 1, b.y + 1}, U32c(kAccent), r + 1, 0, 2.5f * s);
    else if (anim > 0.01f) dl->AddRect(a, b, Rgba(1, 1, 1, 0.25f * anim), r, 0, 1.5f * s);
    // text
    const float ty = b.y + 10 * s;
    dl->AddCircleFilled({p.x + 5 * s, ty + g.font_big->FontSize / 2}, 4 * s, U32c(col));
    dl->PushClipRect({p.x, ty - 4}, {p.x + w, ty + textH}, true);
    const std::string title = e.pretty.empty() ? e.file : e.pretty;
    dl->AddText(g.font_big, g.font_big->FontSize, {p.x + 16 * s, ty}, Rgba(1, 1, 1), title.c_str());
    std::string sub = e.when.size() > 12 ? e.when.substr(12) : e.file;
    sub += "   " + Size(e.bytes);
    dl->AddText(g.font_small, g.font_small->FontSize, {p.x + 16 * s, ty + g.font_big->FontSize + 4 * s}, Rgba(0.58f, 0.63f, 0.70f), sub.c_str());
    dl->PopClipRect();
    (void)now;
}

void MenuScreen(double now) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float s = g.dpi;
    DrawBackground(ImGui::GetBackgroundDrawList(), size, now);
    Rail(size, now);

    const float railW = 256 * s, left = railW + 40 * s, right = size.x - 40 * s;
    auto& es = g.lib.entries();
    std::vector<int> shown;
    for (int i = 0; i < int(es.size()); ++i)
        if (Matches(*es[i])) shown.push_back(i);
    if (g.sort_mode == 1) std::reverse(shown.begin(), shown.end());
    if (g.sort_mode == 2) std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) { return es[a]->info.duration_s > es[b]->info.duration_s; });
    if (g.sort_mode == 3) std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) { return es[a]->bytes > es[b]->bytes; });
    if (g.selected >= 0 && std::find(shown.begin(), shown.end(), g.selected) == shown.end()) g.selected = -1;

    ImGui::SetNextWindowPos({railW, 0});
    ImGui::SetNextWindowSize({size.x - railW, size.y});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0, 0, 0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{40 * s, 28 * s});
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- header: title + count, then search / sort / open on the right
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(g.font_h1, g.font_h1->FontSize, p, Rgba(1, 1, 1), kNav[g.tab].name);
        char sub[512];
        snprintf(sub, sizeof sub, "%zu replay%s   %s   %s", shown.size(), shown.size() == 1 ? "" : "s", "\xc2\xb7", U8(g.lib.dir().wstring()).c_str());
        dl->AddText(g.font_small, g.font_small->FontSize, {p.x + 1 * s, p.y + g.font_h1->FontSize + 4 * s}, Rgba(0.58f, 0.63f, 0.70f), sub);

        const float searchW = std::min(340 * s, (right - left) * 0.34f), h = 40 * s;
        float x = right - searchW - 2 * 44 * s - 150 * s - 3 * 8 * s;
        ImGui::SetCursorScreenPos({x, p.y + 4 * s});
        // search field with an icon inside
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(sp, {sp.x + searchW, sp.y + h}, Rgba(1, 1, 1, 0.07f), 12 * s);
        dl->AddText(g.font, g.font->FontSize, {sp.x + 14 * s, sp.y + (h - g.font->FontSize) / 2}, Rgba(0.6f, 0.65f, 0.72f), ICON_LC_SEARCH);
        ImGui::SetCursorScreenPos({sp.x + 40 * s, sp.y + (h - ImGui::GetFrameHeight()) / 2});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4{0, 0, 0, 0});
        ImGui::SetNextItemWidth(searchW - 76 * s);
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##search", "Search maps, dates, files   (Ctrl+F)", g.search, sizeof g.search);
        ImGui::PopStyleColor(3);
        if (g.search[0]) {
            ImGui::SetCursorScreenPos({sp.x + searchW - 34 * s, sp.y + (h - ImGui::GetFrameHeight()) / 2});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
            if (ImGui::Button(ICON_LC_X "##clr")) g.search[0] = 0;
            ImGui::PopStyleColor();
        }
        x += searchW + 8 * s;
        // sort
        ImGui::SetCursorScreenPos({x, sp.y});
        static const char* sorts[] = {"Newest first", "Oldest first", "Longest first", "Largest first"};
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{14 * s, (h - g.font->FontSize) / 2});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * s);
        if (GhostButton((std::string(ICON_LC_ARROW_DOWN_WIDE_NARROW "  ") + sorts[g.sort_mode]).c_str(), {150 * s, h})) ImGui::OpenPopup("##sort");
        if (ImGui::BeginPopup("##sort")) {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(sorts[i], g.sort_mode == i)) g.sort_mode = i;
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 8 * s);
        if (GhostButton(ICON_LC_FILE_VIDEO "##open", {44 * s, h})) {
            std::wstring f;
            if (PickFile(g.hwnd, f)) RequestOpen(f);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open a replay file (or drop one on the window)");
        ImGui::SameLine(0, 8 * s);
        if (GhostButton(ICON_LC_REFRESH_CW "##refresh", {44 * s, h})) SetFolder(g.lib.dir().wstring());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan the folder");
        ImGui::PopStyleVar(2);
        ImGui::SetCursorScreenPos({p.x, p.y + g.font_h1->FontSize + 34 * s});
    }

    // ---- body (scrolls): hero, then cards grouped by day
    ImGui::BeginChild("##body", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x;
    if (shown.empty()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cy = p.y + 140 * s;
        const char* ic = es.empty() ? ICON_LC_FOLDER_OPEN : ICON_LC_SEARCH;
        const ImVec2 is = TextSize(g.font_icon_big, ic);
        dl->AddCircleFilled({p.x + W / 2, cy}, 54 * s, Rgba(1, 1, 1, 0.05f), 48);
        dl->AddText(g.font_icon_big, g.font_icon_big->FontSize, {p.x + W / 2 - is.x / 2, cy - is.y / 2}, U32c(kAccent, 0.85f), ic);
        const char* t1 = es.empty() ? "No replays here yet" : "Nothing matches";
        const char* t2 = es.empty() ? "Choose the folder EchoReplay saves to (Documents\\EchoReplay), or any folder of .echodemo files."
                                    : "Try another search or category.";
        ImVec2 ts = TextSize(g.font_h1, t1);
        dl->AddText(g.font_h1, g.font_h1->FontSize, {p.x + (W - ts.x) / 2, cy + 74 * s}, Rgba(1, 1, 1), t1);
        ts = TextSize(g.font, t2);
        dl->AddText(g.font, g.font->FontSize, {p.x + (W - ts.x) / 2, cy + 74 * s + g.font_h1->FontSize + 10 * s}, Rgba(0.6f, 0.65f, 0.72f), t2);
        if (es.empty()) {
            ImGui::SetCursorScreenPos({p.x + W / 2 - 90 * s, cy + 150 * s});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{20 * s, 11 * s});
            if (AccentButton(ICON_LC_FOLDER_OPEN "   Choose folder", {180 * s, 0})) {
                std::wstring d;
                if (PickFolder(g.hwnd, d)) SetFolder(d);
            }
            ImGui::PopStyleVar();
        }
    } else {
        // hero: the selected replay, else the newest shown
        if (!g.search[0]) {
            Entry& he = *es[g.selected >= 0 ? g.selected : shown.front()];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float hh = std::clamp(W * 0.30f, 260 * s, 380 * s);
            Hero(he, p, {p.x + W, p.y + hh});
            ImGui::SetCursorScreenPos({p.x, p.y + hh + 30 * s});
            ImGui::Dummy({W, 0});
        }
        // grid
        const float gap = 22 * s, minW = 250 * s;
        const int cols = std::max(1, int((W + gap) / (minW + gap)));
        const float cw = (W - gap * (cols - 1)) / cols, th = cw * 9.f / 16.f, ch = th + 56 * s;
        const float scrollY = ImGui::GetScrollY(), viewH = ImGui::GetWindowHeight();
        const float originY = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y + scrollY;   // content y of the grid start
        float y = 0;
        size_t k = 0;
        const bool by_day = g.sort_mode <= 1;
        while (k < shown.size()) {
            const std::string day = by_day ? DayLabel(es[shown[k]]->when) : std::string(g.sort_mode == 2 ? "Longest first" : "Largest first");
            size_t end = k;
            while (end < shown.size() && (!by_day || DayLabel(es[shown[end]]->when) == day)) ++end;
            // header
            {
                const ImVec2 hp{ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x, ImGui::GetWindowPos().y + originY + y - scrollY};
                dl->AddText(g.font_big, g.font_big->FontSize, hp, Rgba(1, 1, 1), day.c_str());
                const std::string n = std::to_string(end - k);
                const float tw = TextSize(g.font_big, day.c_str()).x;
                dl->AddText(g.font_small, g.font_small->FontSize, {hp.x + tw + 10 * s, hp.y + 3 * s}, Rgba(0.55f, 0.6f, 0.67f), n.c_str());
                y += g.font_big->FontSize + 16 * s;
            }
            for (size_t i = k; i < end; i += cols) {
                const float rowTop = originY + y;
                if (rowTop + ch > scrollY - 40 * s && rowTop < scrollY + viewH + 40 * s) {
                    for (int cidx = 0; cidx < cols && i + cidx < end; ++cidx) {
                        const int ei = shown[i + cidx];
                        const ImVec2 cp{ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x + cidx * (cw + gap), ImGui::GetWindowPos().y + rowTop - scrollY};
                        Card(ei, *es[ei], cp, cw, th, now);
                    }
                }
                y += ch + gap;
            }
            y += 14 * s;
            k = end;
        }
        ImGui::SetCursorPosY(originY + y);
        ImGui::Dummy({W, 20 * s});
    }
    // keyboard: Enter watches the selection, arrows move it
    if (!ImGui::GetIO().WantTextInput && !shown.empty()) {
        auto it = std::find(shown.begin(), shown.end(), g.selected);
        int pos = it == shown.end() ? -1 : int(it - shown.begin());
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) pos = std::min(int(shown.size()) - 1, pos + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) pos = std::max(0, pos - 1);
        if (pos >= 0) g.selected = shown[pos];
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && g.selected >= 0) RequestOpen(es[g.selected]->path);
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // ---- map art progress toast (bottom right)
    TickPreviews(now);
    if (g.preview_proc) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const std::string t = g.preview_progress.empty() ? std::string("Preparing map art") : "Preparing map art   " + g.preview_progress;
        const ImVec2 ts = TextSize(g.font_small, t.c_str());
        const ImVec2 b{size.x - 24 * s, size.y - 24 * s}, a{b.x - ts.x - 52 * s, b.y - ts.y - 22 * s};
        fg->AddRectFilled(a, b, Rgba(0.06f, 0.07f, 0.10f, 0.94f), 12 * s);
        fg->AddRect(a, b, Rgba(1, 1, 1, 0.08f), 12 * s);
        const ImVec2 c{a.x + 20 * s, (a.y + b.y) / 2};
        const float ang = float(now * 5);
        fg->PathArcTo(c, 7 * s, ang, ang + 4.2f, 20);
        fg->PathStroke(U32c(kAccent), 0, 2.2f * s);
        fg->AddText(g.font_small, g.font_small->FontSize, {a.x + 38 * s, a.y + 11 * s}, Rgba(0.85f, 0.9f, 0.95f), t.c_str());
    }
}

// ---- the player ---------------------------------------------------------------------------------------------------
Part& CurPart() { return g.replay.parts[g.part]; }
double LocalT() { return g.t - CurPart().offset; }

void Seek(double t) {
    g.t = std::clamp(t, 0.0, g.replay.duration);
    const int p = g.replay.PartAt(g.t);
    if (p != g.part) {
        g.part = p;
        g.focus = -1;
        if (!EnsurePartLevel()) {}
    }
}

bool Project(V3 p, ImVec2& out) {
    const float aspect = float(g.gpu.width) / float(std::max(1u, g.gpu.height));
    const echo::render::Mat4 m = g.cam.view_proj(aspect);
    const float v[4] = {p.x, p.y, p.z, 1.f};
    float c[4];
    for (int r = 0; r < 4; ++r) c[r] = m.m[r][0] * v[0] + m.m[r][1] * v[1] + m.m[r][2] * v[2] + m.m[r][3] * v[3];
    if (c[3] <= 0.01f) return false;
    out = {(c[0] / c[3] * 0.5f + 0.5f) * g.gpu.width, (1.f - (c[1] / c[3] * 0.5f + 0.5f)) * g.gpu.height};
    return true;
}

void SetCamFromDir(V3 eye, V3 dir) {
    g.cam.eye = {eye.x, eye.y, eye.z};
    dir = Norm(dir);
    g.cam.yaw = atan2f(dir.x, dir.z);
    g.cam.pitch = asinf(std::clamp(dir.y, -0.99f, 0.99f));
}

// Cameras after Echo VR's spectator: Free cam (fly, smoothed), Follow (a chase camera behind the player's
// facing, smoothed, wheel = distance), POV (the player's head, with smoothing so it does not shake), plus a disc
// chase and an overhead view. Right mouse held: look / orbit.
float Damp(float k, float dt) { return 1.f - expf(-k * dt); }
float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

void UpdateCamera(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    g.cam.fovy = g.fov_deg * 3.14159265f / 180.f;
    g.cam.znear = 0.05f;
    float dx = 0, dy = 0;
    if (g.rmb || g.mouse_lock) {
        // relative raw input when the mouse sends it; absolute raw (streamed desktops) as deltas; else the
        // cursor's own motion, put back each frame
        POINT pt;
        GetCursorPos(&pt);
        if (g.raw_rel) dx = g.raw_dx, dy = g.raw_dy;
        else if (g.abs_seen) dx = g.abs_dx, dy = g.abs_dy;
        else dx = float(pt.x - g.last_mouse.x), dy = float(pt.y - g.last_mouse.y);
        if (!g.abs_seen && (pt.x != g.last_mouse.x || pt.y != g.last_mouse.y)) SetCursorPos(g.last_mouse.x, g.last_mouse.y);
        if (g.abs_seen) g.last_mouse = pt;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    g.raw_dx = g.raw_dy = g.abs_dx = g.abs_dy = 0;
    g.raw_rel = g.abs_seen = false;
    const float look = 0.0030f * g.mouse_sens;
    const float ysign = g.invert_y ? -1.f : 1.f;
    const Part& part = CurPart();
    PlayerPose pose{};
    const bool have = g.focus >= 0 && g.focus < int(part.players.size()) && SamplePlayer(part.players[g.focus], LocalT(), pose);
    if ((g.mode == CamMode::Follow || g.mode == CamMode::Pov) && !have) {
        if (g.mode == CamMode::Pov || g.focus < 0) g.mode = CamMode::Free;
    }
    // a cut (new mode or player) snaps; otherwise smoothing carries over
    const bool cut = int(g.mode) != g.last_mode || g.focus != g.last_focus;
    g.last_mode = int(g.mode), g.last_focus = g.focus;
    const V3 eye{g.cam.eye.x, g.cam.eye.y, g.cam.eye.z};
    switch (g.mode) {
        case CamMode::Free: {
            g.cam.yaw = WrapPi(g.cam.yaw - dx * look);
            g.cam.pitch = std::clamp(g.cam.pitch - dy * look * ysign, -1.55f, 1.55f);
            V3 want{0, 0, 0};
            if (!io.WantCaptureKeyboard) {
                const echo::Vec3 f = g.cam.forward(), r = g.cam.right();
                const V3 F{f.x, f.y, f.z}, R{r.x, r.y, r.z};
                if (ImGui::IsKeyDown(ImGuiKey_W)) want = want + F;
                if (ImGui::IsKeyDown(ImGuiKey_S)) want = want - F;
                if (ImGui::IsKeyDown(ImGuiKey_D)) want = want + R;
                if (ImGui::IsKeyDown(ImGuiKey_A)) want = want - R;
                if (ImGui::IsKeyDown(ImGuiKey_E)) want = want + V3{0, 1, 0};
                if (ImGui::IsKeyDown(ImGuiKey_Q)) want = want - V3{0, 1, 0};
                if (Len(want) > 0) want = Norm(want) * (g.cam_speed * (io.KeyShift ? 3.f : 1.f) * (io.KeyCtrl ? 0.3f : 1.f));
            }
            g.free_vel = Lerp(g.free_vel, want, Damp(g.free_smooth, dt));
            g.cam.eye.x += g.free_vel.x * dt, g.cam.eye.y += g.free_vel.y * dt, g.cam.eye.z += g.free_vel.z * dt;
            break;
        }
        case CamMode::Follow: {
            g.orbit_yaw = WrapPi(g.orbit_yaw - dx * look);
            g.orbit_pitch = std::clamp(g.orbit_pitch + dy * look * ysign, -1.2f, 1.3f);
            V3 fwd, up, right;
            HeadAxes(pose.head_q, g.forward_axis, fwd, up, right);
            // the player's heading, smoothed so head turns do not whip the camera around
            const float heading = atan2f(fwd.x, fwd.z);
            if (cut) g.sm_heading = heading;
            g.sm_heading = WrapPi(g.sm_heading + WrapPi(heading - g.sm_heading) * Damp(g.follow_turn, dt));
            // behind them, straight above them (their forward up the screen), or level from their right side
            const float yaw = g.sm_heading + g.orbit_yaw + (g.follow_view == 2 ? -1.5708f : 0.f);
            const float pitch = g.follow_view == 1 ? std::clamp(1.40f + g.orbit_pitch * 0.25f, 0.8f, 1.54f)
                                : g.follow_view == 2 ? std::clamp(g.orbit_pitch - 0.25f, -1.2f, 1.3f) : g.orbit_pitch;
            const float dist = g.follow_view == 1 ? g.orbit_dist * 1.8f : g.orbit_dist;
            const V3 dir{sinf(yaw) * cosf(pitch), -sinf(pitch), cosf(yaw) * cosf(pitch)};
            const V3 target = pose.head + V3{0, g.follow_view == 1 ? 0.f : g.follow_height, 0};
            const V3 want = target - dir * dist;
            const V3 pos = cut ? want : Lerp(eye, want, Damp(g.follow_smooth, dt));
            SetCamFromDir(pos, target - pos);
            break;
        }
        case CamMode::Pov: {
            V3 fwd, up, right;
            HeadAxes(pose.head_q, g.forward_axis, fwd, up, right);
            const V3 want_eye = pose.head + fwd * 0.08f;
            if (cut) g.sm_dir = fwd, g.pov_off_yaw = g.pov_off_pitch = 0;
            g.sm_dir = Norm(Lerp(g.sm_dir, fwd, g.pov_smooth <= 0 ? 1.f : Damp(30.f / g.pov_smooth, dt)));
            // the mouse turns your head away from theirs; leave it and it eases back after a moment
            if (dx != 0 || dy != 0) {
                g.pov_off_yaw = std::clamp(g.pov_off_yaw - dx * look, -2.6f, 2.6f);
                g.pov_off_pitch = std::clamp(g.pov_off_pitch - dy * look * ysign, -1.3f, 1.3f);
                g.pov_look_at = Now();
            } else if (Now() - g.pov_look_at > 1.5) {
                const float k = Damp(3.f, dt);
                g.pov_off_yaw -= g.pov_off_yaw * k, g.pov_off_pitch -= g.pov_off_pitch * k;
            }
            const float pyaw = atan2f(g.sm_dir.x, g.sm_dir.z) + g.pov_off_yaw;
            const float ppitch = std::clamp(asinf(std::clamp(g.sm_dir.y, -1.f, 1.f)) + g.pov_off_pitch, -1.55f, 1.55f);
            SetCamFromDir(want_eye, V3{sinf(pyaw) * cosf(ppitch), sinf(ppitch), cosf(pyaw) * cosf(ppitch)});
            break;
        }
        case CamMode::Disc: {
            V3 pos, vel;
            Q4 ori;
            if (SampleDisc(part, LocalT(), pos, ori, vel)) {
                g.orbit_yaw = WrapPi(g.orbit_yaw - dx * look);
                g.orbit_pitch = std::clamp(g.orbit_pitch + dy * look * ysign, -1.2f, 1.3f);
                // behind the disc's travel when it moves, else where the mouse puts it
                float yaw = g.orbit_yaw;
                if (Len(V3{vel.x, 0, vel.z}) > 1.f) yaw += atan2f(vel.x, vel.z);
                const V3 dir{sinf(yaw) * cosf(g.orbit_pitch), -sinf(g.orbit_pitch), cosf(yaw) * cosf(g.orbit_pitch)};
                const V3 want = pos - dir * g.orbit_dist;
                const V3 p2 = cut ? want : Lerp(eye, want, Damp(g.follow_smooth, dt));
                SetCamFromDir(p2, pos - p2);
            }
            break;
        }
        case CamMode::Top: {
            V3 c{0, 0, 0};
            const echo::render::MeshScene& mesh = g.renderer->mesh_scene();
            if (mesh.has_bounds) c = {(mesh.bounds.lo.x + mesh.bounds.hi.x) / 2, (mesh.bounds.lo.y + mesh.bounds.hi.y) / 2, (mesh.bounds.lo.z + mesh.bounds.hi.z) / 2};
            g.orbit_yaw = WrapPi(g.orbit_yaw - dx * look);
            g.cam.eye = {c.x, c.y + g.orbit_dist * 6.f, c.z};
            g.cam.yaw = g.orbit_yaw;
            g.cam.pitch = -1.55f;
            break;
        }
    }
}
int LastToucher(const Part& part, double t, double& when, double back);
// The chassis models, from the game files (ini avatar_models=name|name, avatar_hand=0..3). avatar.log says what loaded.
void EnsureAvatars() {
    if (g.avatars_tried || !g.levels_ok) return;
    g.avatars_tried = true;
    std::wstring list = IniGet(L"avatar_models", L"cst_body_s11_a");
    const int hand = _wtoi(IniGet(L"avatar_hand", L"0").c_str());
    std::string report;
    for (size_t a = 0; a <= list.size();) {
        size_t b = list.find(L'|', a);
        if (b == std::wstring::npos) b = list.size();
        if (b > a) {
            Avatar av;
            av.hand_frame = hand;
            av.tri_budget = _wtoi(IniGet(L"avatar_tris", L"0").c_str());
            av.max_texture = std::max(64, _wtoi(IniGet(L"avatar_texture", L"2048").c_str()));
            if (wchar_t e[16]{}; GetEnvironmentVariableW(L"ERV_AVATAR_TRIS", e, 16)) av.tri_budget = _wtoi(e);   // testing
            const bool ok = av.Load(gamefiles::Corpus(), U8(list.substr(a, b - a)));
            report += (ok ? "loaded " : "FAILED ") + av.Describe() + "\n";
            if (ok) {
                g.avatar_material_base.push_back(g.actor_materials.size());
                av.TakeMaterials(g.actor_textures, g.actor_materials);
                g.actor_materials_pending = true;
                g.avatars.push_back(std::move(av));
            }
        }
        a = b + 1;
    }
    if (FILE* f = nullptr; _wfopen_s(&f, (AppDir() / "avatar.log").c_str(), L"w") == 0 && f) fputs(report.c_str(), f), fclose(f);
}
// The arena's own screens: score, clock, launch countdown, and the end-of-match board once the match is over.
void UpdateArenaBoards(const Part& part, double lt) {
    if (!g.levels_ok || part.level_name != "mpl_arena_a" || g.levels.current() != part.level_name) return;
    Levels::BoardState bs;
    if (const State* st = StateAt(part, lt)) {
        bs.valid = true;
        bs.phase = st->phase;
        bs.round = st->round;
        bs.blue = st->blue;
        bs.orange = st->orange;
        bs.clock = st->clock;
        bs.countdown = st->countdown;
        // A recording without countdownTimer (its presence bit clear): the launch countdown runs to the round's
        // first Playing state, so the time left until that state stands in for it.
        if (st->phase == 5 && st->countdown <= 0.f) {
            for (const State* s = st + 1; s < part.states.data() + part.states.size(); ++s) {
                if (s->phase == 5 || s->phase <= 2) continue;   // RoundStart again, or a sample the recorder left unclassified
                if (s->phase == 6) bs.countdown = float(std::max(0.0, s->t - lt));
                break;
            }
        }
    }
    if (!bs.valid) return;
    bs.players.resize(part.players.size());
    for (size_t i = 0; i < part.players.size(); ++i) {
        bs.players[i].name = part.players[i].name;
        bs.players[i].team = part.players[i].team;
    }
    for (const Event& e : part.events) {
        if (e.t > lt) continue;
        if (e.kind == ERV_EVENT_GOAL) {
            if (e.a >= 0 && size_t(e.a) < bs.players.size()) bs.players[e.a].points += int(e.points);
            if (e.b >= 0 && size_t(e.b) < bs.players.size()) bs.players[e.b].assists += 1;
        } else if (e.kind == ERV_EVENT_STUN) {
            if (e.b >= 0 && size_t(e.b) < bs.players.size()) bs.players[e.b].stuns += 1;
        }
    }
    // Only the people in the match at this moment: one who left is off the board, and one who rejoined (a second
    // track under the same name) is one row, their stats summed, on the team they are on now.
    {
        std::vector<Levels::BoardPlayer> now;
        std::vector<bool> here;
        for (size_t i = 0; i < bs.players.size(); ++i) {
            const PlayerTrack& tr = part.players[i];
            const bool in = tr.first <= lt + 0.5 && lt <= tr.last + 0.5;
            const Levels::BoardPlayer& p = bs.players[i];
            auto same = std::find_if(now.begin(), now.end(), [&](const Levels::BoardPlayer& x) { return x.name == p.name; });
            if (same == now.end()) {
                now.push_back(p);
                here.push_back(in);
                continue;
            }
            same->points += p.points, same->assists += p.assists, same->stuns += p.stuns;
            if (in) same->team = p.team, here[size_t(same - now.begin())] = true;
        }
        bs.players.clear();
        for (size_t i = 0; i < now.size(); ++i)
            if (here[i]) bs.players.push_back(now[i]);
    }
    // only the people playing: spectators and the unassigned are not on the board
    std::erase_if(bs.players, [](const Levels::BoardPlayer& p) { return p.team > 1 || p.name.empty(); });
    // the latest goal, for the goal ends' info boards
    for (auto e = part.events.rbegin(); e != part.events.rend(); ++e) {
        if (e->t > lt || e->kind != ERV_EVENT_GOAL) continue;
        auto& gl = bs.goal;
        gl.have = true;
        gl.since = float(lt - e->t);
        if (e->a >= 0 && size_t(e->a) < part.players.size()) gl.scorer = part.players[e->a].name;
        if (e->b >= 0 && size_t(e->b) < part.players.size() && e->b != e->a) gl.assist = part.players[e->b].name;
        gl.points = e->points;
        gl.type = e->goal_type;
        gl.speed = e->f0;
        gl.distance = e->f1;
        // the clock as the goal went in: the last sample at or before it that holds a running game
        for (const State& s : part.states) {
            if (s.t > e->t + 1e-3) break;
            if (s.phase >= 6) gl.clock = s.clock;
        }
        break;
    }
    const double t0 = Now();
    g.levels.SetBoards(bs);
    g.boards_ms = (Now() - t0) * 1000.0;
}

void BuildWires() {
    if (g.shot_frames == 89 && !g.avatars.empty())   // --shot: how long the skinning took this frame
        if (FILE* f = nullptr; _wfopen_s(&f, (AppDir() / "avatar.log").c_str(), L"a") == 0 && f)
            fprintf(f, "frame: %.2f ms skinning, %zu verts, %zu tris, %.2f ms screens\n", g.skin_ms, g.actor_v.size(), g.actor_i.size() / 3, g.boards_ms), fclose(f);
    g.skin_ms = 0;
    g.wires.clear();
    g.actor_v.clear();
    g.actor_i.clear();
    g.actor_draws.clear();
    for (auto& part_indices : g.actor_parts) part_indices.clear();
    EnsureAvatars();
    const bool models = g.show_models && g.show_bodies && !g.avatars.empty();
    const Part& part = CurPart();
    const double lt = LocalT();
    UpdateArenaBoards(part, lt);
    if (g.hidden.size() != part.players.size()) g.hidden.assign(part.players.size(), false);
    for (size_t i = 0; i < part.players.size(); ++i) {
        const PlayerTrack& p = part.players[i];
        if (g.hidden[i] || !g.show_players) continue;
        if (g.mode == CamMode::Pov && int(i) == g.focus) continue;   // not your own helmet in your face
        PlayerPose pose;
        if (!SamplePlayer(p, lt, pose)) continue;
        FigureStyle st;
        const ImVec4 c = TeamCol(p.team);
        st.colour = ToWire(c);
        st.accent = ToWire({std::min(1.f, c.x * 0.6f + 0.4f), std::min(1.f, c.y * 0.6f + 0.4f), std::min(1.f, c.z * 0.6f + 0.4f), 1.f});
        st.xray = g.xray;
        st.body = g.show_bodies;
        st.thickness = g.thickness;
        st.forward_axis = g.forward_axis;
        if (models) {
            AvatarStyle as;
            as.primary = st.colour;
            as.secondary = st.accent;
            as.glow = st.colour;
            as.forward_axis = g.forward_axis;
            const double t0 = Now();
            const DemoLean lean = DemoLeanAt(p, lt, g.forward_axis);   // IKController's eased lean, replayed up to now
            for (size_t a = 0; a < g.avatars.size(); ++a) g.avatars[a].Add(pose, as, &lean, g.actor_v, g.actor_parts, g.avatar_material_base[a]);
            g.skin_ms += (Now() - t0) * 1000.0;
        } else {
            AddFigure(g.wires, pose, st);
        }
        if (g.show_trails) {
            std::vector<V3> pts;
            for (double k = std::max(p.first, lt - g.trail_s); k <= lt; k += 0.05) {
                PlayerPose q;
                if (SamplePlayer(p, k, q)) pts.push_back(q.head - V3{0, 0.5f, 0});
            }
            AddTrail(g.wires, pts, ToWire(c), g.xray);
        }
    }
    for (uint32_t m = 0; m < g.actor_parts.size(); ++m) {   // one draw per material
        if (g.actor_parts[m].empty()) continue;
        g.actor_draws.push_back({uint32_t(g.actor_i.size()), uint32_t(g.actor_parts[m].size()), m});
        g.actor_i.insert(g.actor_i.end(), g.actor_parts[m].begin(), g.actor_parts[m].end());
    }
    V3 pos, vel;
    Q4 ori;
    const bool have_disc = SampleDisc(part, lt, pos, ori, vel);
    double when = 0;
    const int last = have_disc ? LastToucher(part, lt, when, 8.0) : -1;
    const bool held = last >= 0 && lt - when < 8.0;
    if (g.levels.HasDisc()) {   // the level's own disc model and bubble follow the recording
        const float p3[3] = {pos.x, pos.y, pos.z}, q4[4] = {ori.x, ori.y, ori.z, ori.w};
        g.levels.SetDisc(have_disc, p3, q4);
    }
    if (have_disc) {
        const uint32_t dc = held ? ToWire(TeamCol(part.players[last].team)) : 0xFFFFFFFFu;
        if (!g.levels.HasDisc()) AddDisc(g.wires, pos, ori, dc, true);
        if (g.disc_trail) {
            std::vector<V3> pts;
            for (double k = std::max(0.0, lt - g.trail_s * 1.5); k <= lt; k += 0.03) {
                V3 p2, v2;
                Q4 o2;
                if (SampleDisc(part, k, p2, o2, v2)) pts.push_back(p2);
            }
            AddTrail(g.wires, pts, 0xFFFFF0E0u, true);
        }
    }
}

std::string PlayerName(int idx) {
    const Part& part = CurPart();
    return idx >= 0 && idx < int(part.players.size()) ? part.players[idx].name : std::string("?");
}

// Who last had the disc before `t`: the nearest hand within reach of it, looking back up to 30 s.
int LastToucher(const Part& part, double t, double& when, double back) {
    for (double k = t; k > t - back && k >= 0; k -= 1 / 30.0) {
        V3 dp, dv;
        Q4 dq;
        if (!SampleDisc(part, k, dp, dq, dv)) continue;
        int best = -1;
        float bestD = 0.35f;
        for (size_t i = 0; i < part.players.size(); ++i) {
            PlayerPose q;
            if (!SamplePlayer(part.players[i], k, q)) continue;
            const float d = std::min(Len(q.lhand - dp), Len(q.rhand - dp));
            if (d < bestD) bestD = d, best = int(i);
        }
        if (best >= 0) return when = k, best;
    }
    return -1;
}

void HoverInfo() {
    ImGuiIO& io = ImGui::GetIO();
    if (g.mouse_lock || g.rmb || io.WantCaptureMouse) return;
    const Part& part = CurPart();
    const double lt = LocalT();
    const ImVec2 m = io.MousePos;
    const float reach = 46 * g.dpi;
    int who = -1;
    float bestD = reach;
    bool disc = false;
    V3 dp, dv;
    Q4 dq;
    if (SampleDisc(part, lt, dp, dq, dv)) {
        ImVec2 sp;
        if (Project(dp, sp)) {
            const float d = sqrtf((sp.x - m.x) * (sp.x - m.x) + (sp.y - m.y) * (sp.y - m.y));
            if (d < reach * 0.7f) disc = true, bestD = d;
        }
    }
    if (g.show_players) {
        for (size_t i = 0; i < part.players.size(); ++i) {
            if (g.hidden[i] || (g.mode == CamMode::Pov && int(i) == g.focus)) continue;
            PlayerPose pose;
            if (!SamplePlayer(part.players[i], lt, pose)) continue;
            for (V3 at : {pose.head, pose.head - V3{0, 0.4f, 0}, pose.head - V3{0, 0.8f, 0}}) {
                ImVec2 sp;
                if (!Project(at, sp)) continue;
                const float d = sqrtf((sp.x - m.x) * (sp.x - m.x) + (sp.y - m.y) * (sp.y - m.y));
                if (d < bestD) bestD = d, who = int(i), disc = false;
            }
        }
    }
    if (!disc && who < 0) return;
    ImGui::BeginTooltip();
    if (disc) {
        ImGui::PushFont(g.font_big);
        ImGui::TextUnformatted("Disc");
        ImGui::PopFont();
        ImGui::Text("Speed  %.1f m/s", Len(dv));
        const Event* goal = nullptr;   // the latest goal so far: the disc's speed when it went in
        for (const Event& e : part.events) {
            if (e.t > lt) break;
            if (e.kind == ERV_EVENT_GOAL) goal = &e;
        }
        if (goal) {
            ImGui::Text("Last goal  %.1f m/s", goal->f0);
            if (goal->a >= 0 && goal->a < int(part.players.size()))
                ImGui::TextDisabled("by %s, %.0f s ago", part.players[goal->a].name.c_str(), lt - goal->t);
        }
        double when = 0;
        const int last = LastToucher(part, lt, when, 30.0);
        if (last >= 0) {
            const bool holding = lt - when < 0.05;
            ImGui::TextColored(TeamCol(part.players[last].team), "%s %s", holding ? "Held by" : "Last touched by", part.players[last].name.c_str());
            if (!holding) ImGui::TextDisabled("%.1f s ago", lt - when);
        } else {
            ImGui::TextDisabled("No one has touched it recently");
        }
    } else {
        const PlayerTrack& p = part.players[who];
        PlayerPose pose;
        SamplePlayer(p, lt, pose);
        int goals = 0, assists = 0, stunsGiven = 0, stunned = 0, points = 0;
        for (const Event& e : part.events) {
            if (e.t > lt) break;
            if (e.kind == ERV_EVENT_GOAL) {
                if (e.a == who) ++goals, points += int(e.points);
                if (e.b == who) ++assists;
            } else if (e.kind == ERV_EVENT_STUN) {
                if (e.a == who) ++stunned;
                if (e.b == who) ++stunsGiven;
            }
        }
        ImGui::PushFont(g.font_big);
        ImGui::TextColored(TeamCol(p.team), "%s", p.name.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("%s", p.team == 0 ? "Blue team" : p.team == 1 ? "Orange team" : p.team == 2 ? "Spectator" : "No team");
        if (p.recorder) ImGui::TextDisabled("Recorded this replay");
        if (p.guessed && p.recorder) ImGui::TextColored({1, 0.8f, 0.3f, 1}, "Team worked out from who they played near");
        else if (p.guessed) ImGui::TextColored({1, 0.8f, 0.3f, 1}, "Name matched by order: the recording did not say whose body this is");
        if (pose.body_only) ImGui::TextDisabled("Position only: no head or hand tracking recorded");
        ImGui::Text("Speed  %.1f m/s", Len(pose.vel));
        if (part.level_name == "mpl_arena_a" || goals || assists) ImGui::Text("Goals %d  (%d pts)   Assists %d", goals, points, assists);
        ImGui::Text("Stuns %d   Stunned %d", stunsGiven, stunned);
        if (pose.stunned) ImGui::TextColored({1, 0.9f, 0.3f, 1}, "Stunned");
        if (pose.speaking) ImGui::TextColored({0.4f, 1.f, 0.45f, 1}, "Talking");
        double when = 0;
        if (LastToucher(part, lt, when, 30.0) == who) ImGui::TextColored({1, 1, 1, 1}, lt - when < 0.05 ? "Has the disc" : "Last touched the disc %.1f s ago", lt - when);
        ImGui::TextDisabled("Click the view to look around · P first person");
    }
    ImGui::EndTooltip();
}

void Overlays() {
    HoverInfo();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const Part& part = CurPart();
    const double lt = LocalT();
    const float s = g.dpi;
    if (g.show_names && g.show_players) {
        for (size_t i = 0; i < part.players.size(); ++i) {
            if (g.hidden[i] || (g.mode == CamMode::Pov && int(i) == g.focus)) continue;
            PlayerPose pose;
            if (!SamplePlayer(part.players[i], lt, pose)) continue;
            ImVec2 sp;
            if (!Project(pose.head + V3{0, 0.32f, 0}, sp)) continue;
            const float dist = Len(pose.head - V3{g.cam.eye.x, g.cam.eye.y, g.cam.eye.z});
            const float k = std::clamp(1.4f - dist / 40.f, 0.55f, 1.f);
            ImFont* f = g.font;
            const float fs = f->FontSize * k;
            const char* name = part.players[i].name.c_str();
            const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, name);
            const ImVec4 c = TeamCol(part.players[i].team);
            const ImVec2 a{sp.x - ts.x / 2 - 8 * s, sp.y - ts.y - 6 * s}, b{sp.x + ts.x / 2 + 8 * s, sp.y};
            dl->AddRectFilled(a, b, Rgba(0.02f, 0.03f, 0.05f, 0.62f), 6 * s);
            dl->AddRectFilled({a.x, b.y - 2.5f * s}, b, ImGui::ColorConvertFloat4ToU32(c), 6 * s, ImDrawFlags_RoundCornersBottom);
            dl->AddText(f, fs, {a.x + 8 * s, a.y + 2 * s}, Rgba(1, 1, 1), name);
            if (pose.speaking) dl->AddCircleFilled({b.x + 8 * s, (a.y + b.y) / 2}, 4.5f * s, Rgba(0.4f, 1.f, 0.45f));
            if (pose.stunned) dl->AddText(g.font_small, g.font_small->FontSize, {a.x, a.y - g.font_small->FontSize}, Rgba(1, 0.9f, 0.3f), "STUNNED");
        }
    }
    // scoreboard
    if (const State* st = StateAt(part, lt); st && part.level_name == "mpl_arena_a") {
        const float cx = g.gpu.width / 2.f, y = 16 * s;
        char b[16], o[16];
        snprintf(b, sizeof b, "%u", st->blue);
        snprintf(o, sizeof o, "%u", st->orange);
        const std::string clk = Clock(st->clock);
        const float w = 120 * s, h = 46 * s;
        dl->AddRectFilled({cx - w - 60 * s, y}, {cx - 60 * s, y + h}, Rgba(0.10f, 0.38f, 0.75f, 0.85f), 8 * s, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled({cx - 60 * s, y}, {cx + 60 * s, y + h}, Rgba(0.03f, 0.04f, 0.06f, 0.85f));
        dl->AddRectFilled({cx + 60 * s, y}, {cx + w + 60 * s, y + h}, Rgba(0.85f, 0.42f, 0.08f, 0.85f), 8 * s, ImDrawFlags_RoundCornersRight);
        auto centred = [&](ImFont* f, float size, float x, const char* txt) {
            const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, txt);
            dl->AddText(f, size, {x - ts.x / 2, y + (h - ts.y) / 2}, Rgba(1, 1, 1), txt);
        };
        centred(g.font_big, g.font_big->FontSize * 1.25f, cx - 60 * s - w / 2, b);
        centred(g.font_big, g.font_big->FontSize * 1.25f, cx + 60 * s + w / 2, o);
        centred(g.font_mono, g.font_mono->FontSize * 1.2f, cx, clk.c_str());
    }
    // event feed
    if (g.show_events) {
        float y = 80 * s;
        int shown = 0;
        for (auto it = part.events.rbegin(); it != part.events.rend() && shown < 6; ++it) {
            if (it->t > lt || lt - it->t > 12) continue;
            ++shown;
            std::string line;
            ImVec4 col = kGrey;
            if (it->kind == ERV_EVENT_GOAL) {
                char sp[64];
                snprintf(sp, sizeof sp, "  %u pts  %.1f m/s", it->points, it->f0);
                line = "GOAL  " + PlayerName(it->a) + (it->b >= 0 ? "  (assist " + PlayerName(it->b) + ")" : "") + sp;
                col = it->team == 0 ? kBlue : kOrange;
            } else {
                line = "STUN  " + PlayerName(it->b) + "  >  " + PlayerName(it->a);
                col = {1, 0.9f, 0.35f, 1};
            }
            const float alpha = float(std::clamp(1.0 - (lt - it->t - 9) / 3.0, 0.0, 1.0));
            const ImVec2 ts = g.font->CalcTextSizeA(g.font->FontSize, FLT_MAX, 0, line.c_str());
            const float x = g.gpu.width - ts.x - 40 * s;
            dl->AddRectFilled({x - 10 * s, y - 4 * s}, {x + ts.x + 10 * s, y + ts.y + 4 * s}, Rgba(0.02f, 0.03f, 0.05f, 0.6f * alpha), 6 * s);
            dl->AddRectFilled({x - 10 * s, y - 4 * s}, {x - 6 * s, y + ts.y + 4 * s}, ImGui::ColorConvertFloat4ToU32({col.x, col.y, col.z, alpha}));
            dl->AddText(g.font, g.font->FontSize, {x, y}, Rgba(1, 1, 1, alpha), line.c_str());
            y += ts.y + 14 * s;
        }
    }
}

// A timeline: drag to scrub; goals, stuns, part joins and bookmarks marked.
void Timeline(float width) {
    const float s = g.dpi;
    const float h = 30 * s;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", {width, h});
    const bool active = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bar0 = p0.y + h * 0.5f - 3 * s, bar1 = p0.y + h * 0.5f + 3 * s;
    const double dur = std::max(0.001, g.replay.duration);
    auto X = [&](double t) { return p0.x + float(t / dur) * width; };
    dl->AddRectFilled({p0.x, bar0}, {p0.x + width, bar1}, Rgba(1, 1, 1, 0.12f), 3 * s);
    dl->AddRectFilled({p0.x, bar0}, {X(g.t), bar1}, ImGui::ColorConvertFloat4ToU32(kAccent), 3 * s);
    for (const Part& part : g.replay.parts) {
        if (part.offset > 0) dl->AddLine({X(part.offset), p0.y}, {X(part.offset), p0.y + h}, Rgba(1, 1, 1, 0.5f), 2 * s);
        for (const Event& e : part.events) {
            const float x = X(part.offset + e.t);
            if (e.kind == ERV_EVENT_GOAL) {
                const ImU32 c = ImGui::ColorConvertFloat4ToU32(e.team == 0 ? kBlue : kOrange);
                dl->AddTriangleFilled({x - 5 * s, p0.y}, {x + 5 * s, p0.y}, {x, p0.y + 9 * s}, c);
                dl->AddLine({x, p0.y + 6 * s}, {x, bar1}, c, 1.5f * s);
            } else {
                dl->AddLine({x, bar1}, {x, bar1 + 6 * s}, Rgba(1, 0.9f, 0.35f, 0.7f), 1.f * s);
            }
        }
    }
    for (double b : g.bookmarks) dl->AddCircleFilled({X(b), p0.y + h - 4 * s}, 4 * s, Rgba(0.95f, 0.35f, 0.75f));
    dl->AddCircleFilled({X(g.t), (bar0 + bar1) / 2}, (active ? 8.f : 6.5f) * s, Rgba(1, 1, 1));
    if (hov || active) {
        const float mx = std::clamp(ImGui::GetIO().MousePos.x, p0.x, p0.x + width);
        const double at = double(mx - p0.x) / width * dur;
        ImGui::SetTooltip("%s", Clock(at, true).c_str());
        if (active) Seek(at);
        // jump to an event clicked near
        if (ImGui::IsMouseClicked(0)) {}
    }
}

void PlayerScreen(double now, float dt) {
    if (g.replay.parts.empty()) {
        g.screen = Screen::Menu;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    const float s = g.dpi;
    // ---- keys
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) g.playing = !g.playing;
        const double jump = io.KeyShift ? 1 : 5;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) Seek(g.t - jump);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) Seek(g.t + jump);
        if (ImGui::IsKeyPressed(ImGuiKey_Comma)) g.playing = false, Seek(g.t - 1 / 45.0);
        if (ImGui::IsKeyPressed(ImGuiKey_Period)) g.playing = false, Seek(g.t + 1 / 45.0);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) Seek(0);
        if (ImGui::IsKeyPressed(ImGuiKey_End)) Seek(g.replay.duration);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) g.speed = std::max(-4.0, g.speed - (std::fabs(g.speed) <= 1 ? 0.25 : 0.5));
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) g.speed = std::min(4.0, g.speed + (std::fabs(g.speed) < 1 ? 0.25 : 0.5));
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) g.speed = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) g.speed = -g.speed;
        if (ImGui::IsKeyPressed(ImGuiKey_M)) g.bookmarks.push_back(g.t), std::sort(g.bookmarks.begin(), g.bookmarks.end());
        if (ImGui::IsKeyPressed(ImGuiKey_N)) g.show_names = !g.show_names;
        if (ImGui::IsKeyPressed(ImGuiKey_X)) g.xray = !g.xray;
        if (ImGui::IsKeyPressed(ImGuiKey_H)) g.show_ui = !g.show_ui;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) g.mode = CamMode::Free;
        if (ImGui::IsKeyPressed(ImGuiKey_C)) g.mode = CamMode((int(g.mode) + 1) % 5);
        if (ImGui::IsKeyPressed(ImGuiKey_V) && g.focus >= 0) g.mode = g.mode == CamMode::Pov ? CamMode::Follow : CamMode::Pov;
        if (ImGui::IsKeyPressed(ImGuiKey_P) && !CurPart().players.empty()) {   // first person; 1-8 then switch whose
            if (g.focus < 0) g.focus = 0;
            g.mode = CamMode::Pov;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_G)) {   // follow angle: behind / overhead / side
            if (g.focus < 0 && !CurPart().players.empty()) g.focus = 0;
            if (g.mode == CamMode::Follow) g.follow_view = (g.follow_view + 1) % 3;
            g.mode = CamMode::Follow;
            g.orbit_yaw = 0, g.orbit_pitch = 0.25f;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) g.panel_players = !g.panel_players;
        if (ImGui::IsKeyPressed(ImGuiKey_F3)) g.panel_view = !g.panel_view;
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            const int n = int(CurPart().players.size());
            if (n) g.focus = (g.focus + (io.KeyShift ? n - 1 : 1)) % n, g.mode = g.mode == CamMode::Pov ? CamMode::Pov : CamMode::Follow;
        }
        for (int k = 0; k < 9; ++k)
            if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_1 + k)) && k < int(CurPart().players.size()))
                g.focus = k, g.mode = g.mode == CamMode::Pov ? CamMode::Pov : CamMode::Follow;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g.screen = Screen::Menu;
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
            const fs::path dir = AppDir() / "screenshots";
            std::error_code ec;
            fs::create_directories(dir, ec);
            auto frame = g.renderer->capture(g.cam, g.gpu.width, g.gpu.height, {.selection_outline = false});
            if (frame.ok()) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                wchar_t n[64];
                swprintf_s(n, L"shot_%04u%02u%02u_%02u%02u%02u.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                echo::ui::write_png_file(dir / n, frame.value());
                g.status = "Saved " + U8((dir / n).wstring());
            }
        }
    }
    if (!io.WantCaptureMouse && io.MouseWheel != 0) {
        if (g.mode == CamMode::Free) g.cam_speed = std::clamp(g.cam_speed * (io.MouseWheel > 0 ? 1.2f : 1 / 1.2f), 0.5f, 80.f);
        else g.orbit_dist = std::clamp(g.orbit_dist * (io.MouseWheel > 0 ? 1 / 1.15f : 1.15f), 0.8f, 40.f);
    }

    // ---- time
    if (g.playing) {
        double next = g.t + dt * g.speed;
        if (next >= g.replay.duration) next = g.loop ? 0 : g.replay.duration, g.playing = g.loop;
        if (next <= 0 && g.speed < 0) next = 0, g.playing = false;
        Seek(next);
    }
    UpdateCamera(dt);
    {
        const echo::Vec3 r = g.cam.right();
        voice::Update(CurPart(), LocalT(), g.speed, g.playing, V3{g.cam.eye.x, g.cam.eye.y, g.cam.eye.z}, V3{r.x, r.y, r.z});
    }
    BuildWires();

    // ---- the level
    if (g.gpu.EnsureViewport(g.gpu.width, g.gpu.height)) {
        g.renderer->set_game_view(true);   // the game's look; our wires still draw (kViewNoGrid)
        g.renderer->set_view_layers(echo::render::kViewMarkers | echo::render::kViewNoGrid);   // our wires, no grid, no marker cubes
        g.renderer->set_shader_time(float(g.t));
        static_assert(sizeof(WireVert) == sizeof(echo::render::GizmoWireVertex), "wire layout");
        g.renderer->set_gizmo_wires(echo::render::GizmoWireList::Highlight,
                                    std::span<const echo::render::GizmoWireVertex>(
                                        reinterpret_cast<const echo::render::GizmoWireVertex*>(g.wires.data()), g.wires.size()));
        g.renderer->set_gizmo_wire_fade(1000.f, 2000.f);
        if (g.actor_materials_pending) {
        g.renderer->set_actor_materials(std::move(g.actor_textures), g.actor_materials);
        g.actor_textures.clear();
        g.actor_materials_pending = false;
    }
    g.renderer->set_actor_meshes(g.actor_v, g.actor_i, g.actor_draws);
        {
            const float eye[3] = {g.cam.eye.x, g.cam.eye.y, g.cam.eye.z};
            g.levels.FaceViewer(eye);
        }
        g.renderer->render_to(g.gpu.ViewportTarget(), g.cam);
        ImGui::GetBackgroundDrawList()->AddImage(g.gpu.ViewportId(), {0, 0}, io.DisplaySize);
        // --shot <png>: follow the first player a moment, save what the view shows, quit (testing)
        if (!g.shot_path.empty()) {
            ++g.shot_frames;
            if (g.shot_frames == 2 && !CurPart().players.empty()) g.focus = 0, g.mode = CamMode::Follow, g.playing = true;
            if (g.shot_frames == 90) {
                auto frame = g.renderer->capture(g.cam, g.gpu.width, g.gpu.height, {.selection_outline = false});
                if (frame.ok()) echo::ui::write_png_file(g.shot_path, frame.value());
                // --closeup N: also the player N (track index) from the front, side, back and above, close (testing)
                const Part& cp = CurPart();
                PlayerPose pose;
                if (g.closeup >= 0 && g.closeup < int(cp.players.size()) && SamplePlayer(cp.players[g.closeup], LocalT(), pose)) {
                    V3 hf, hu, hr;
                    HeadAxes(pose.head_q, g.forward_axis, hf, hu, hr);
                    V3 bf = Norm(V3{hf.x, 0, hf.z});
                    const V3 bl = Cross(V3{0, 1, 0}, bf);
                    const V3 chest = pose.head - V3{0, 0.45f, 0};
                    struct View { const wchar_t* name; V3 dir; float dist; };
                    const View views[] = {{L"_front", bf, 2.0f}, {L"_side", bl, 2.0f}, {L"_back", bf * -1.f, 2.0f},
                                          {L"_above", Norm(bf + V3{0, 1.2f, 0}), 2.2f}, {L"_hands", Norm(bf + bl * 0.6f + V3{0, 0.3f, 0}), 1.1f}};
                    const fs::path base = fs::path(g.shot_path).replace_extension();
                    for (const View& v : views) {
                        echo::render::FlyCamera c = g.cam;
                        const V3 target = v.name == std::wstring(L"_hands") ? (pose.lhand + pose.rhand) * 0.5f : chest;
                        const V3 eye = target + v.dir * v.dist;
                        c.eye = {eye.x, eye.y, eye.z};
                        const V3 d = Norm(target - eye);
                        c.yaw = atan2f(d.x, d.z);
                        c.pitch = asinf(std::clamp(d.y, -0.99f, 0.99f));
                        auto f2 = g.renderer->capture(c, g.gpu.width, g.gpu.height, {.selection_outline = false});
                        if (f2.ok()) echo::ui::write_png_file(base.wstring() + v.name + L".png", f2.value());
                    }
                }
                if (!g.shot_views.empty()) {   // --views: fixed cameras (testing the level's screens)
                    std::ifstream vf{fs::path(g.shot_views)};
                    std::string name;
                    float e[3], a[3];
                    const fs::path base = fs::path(g.shot_path).replace_extension();
                    std::ofstream picks{fs::path(base.wstring() + L"_picks.txt")};
                    while (vf >> name >> e[0] >> e[1] >> e[2] >> a[0] >> a[1] >> a[2]) {
                        if (name.rfind("disc", 0) == 0) {   // eye and target relative to the recorded disc
                            V3 dp, dv;
                            Q4 dq;
                            if (SampleDisc(CurPart(), LocalT(), dp, dq, dv))
                                for (int k = 0; k < 3; ++k) e[k] += (&dp.x)[k], a[k] += (&dp.x)[k];
                        }
                        if (name == "phases") {   // the recording's game phases, where each starts
                            uint32_t last = ~0u;
                            for (const State& st : CurPart().states)
                                if (st.phase != last) picks << "phase " << st.phase << " at " << st.t << " round " << st.round << " score " << st.blue << "-" << st.orange << '\n', last = st.phase;
                            picks << "duration " << g.replay.duration << '\n';
                            for (const PlayerTrack& p : CurPart().players)
                                picks << "player " << p.name << " slot " << p.slot << " team " << p.team << " from " << p.first << " to " << p.last
                                      << (p.recorder ? " recorder" : "") << '\n';
                            continue;
                        }
                        if (name.rfind("pick", 0) == 0) {   // which placement a ray meets first
                            const auto hit = g.renderer->pick({{e[0], e[1], e[2]}, {a[0] - e[0], a[1] - e[1], a[2] - e[2]}});
                            picks << name;
                            if (hit) picks << " placement " << hit->placement << " layer " << hit->layer << " row " << hit->mesh_row
                                           << " at " << hit->point.x << "," << hit->point.y << "," << hit->point.z;
                            if (hit && hit->layer < g.levels.sections().size()) picks << " layername " << g.levels.sections()[hit->layer];
                            if (hit && hit->layer == 0xFFFFFFFFu) {
                                const auto& sc = g.renderer->live_scene().scene();
                                if (hit->placement < sc.instances.size()) picks << " flags " << sc.instances[hit->placement].flags;
                                if (hit->placement < sc.placement_actor.size()) picks << " actor " << sc.placement_actor[hit->placement];
                                const auto& mesh = g.renderer->live_scene().mesh();
                                const auto& mats = g.renderer->material_scene();
                                for (size_t bi = 0; bi < mesh.batches.size(); ++bi) {
                                    const auto& bt = mesh.batches[bi];
                                    bool has = false;
                                    for (uint32_t k = 0; k < bt.instance_count; ++k)
                                        if (bt.first_instance + k < mesh.instances.size() && mesh.instances[bt.first_instance + k].pad == hit->placement) has = true;
                                    if (!has) continue;
                                    picks << " [batch " << bi << " flags " << (bt.canvas_receiver ? "R" : "") << (bt.unscripted_instance_model ? "U" : "") << ":";
                                    for (uint32_t t : mats.canvas_slots(bi))
                                        if (t < mats.textures.size()) picks << " " << std::hex << mats.textures[t].asset << std::dec;
                                    picks << "]";
                                }
                            }
                            else picks << " nothing";
                            picks << '\n';
                            continue;
                        }
                        echo::render::FlyCamera c = g.cam;
                        c.eye = {e[0], e[1], e[2]};
                        const V3 d = Norm(V3{a[0] - e[0], a[1] - e[1], a[2] - e[2]});
                        c.yaw = atan2f(d.x, d.z);
                        c.pitch = asinf(std::clamp(d.y, -0.99f, 0.99f));
                        g.levels.FaceViewer(e);
                        g.renderer->render_to(g.gpu.ViewportTarget(), c);   // so the boards' new pose is uploaded
                        auto f3 = g.renderer->capture(c, g.gpu.width, g.gpu.height, {.selection_outline = false});
                        if (f3.ok()) echo::ui::write_png_file(base.wstring() + L"_" + std::wstring(name.begin(), name.end()) + L".png", f3.value());
                    }
                }
                PostQuitMessage(0);
            }
        }
    }
    Overlays();

    // HUD fades when the mouse is still in playback
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsAnyItemActive()) g.last_input = now;
    if (!g.show_ui) return;
    {
        const char* hint = g.mouse_lock ? "Mouse look on  ·  Esc to release the mouse" : "Click the view to look around  ·  P first person  ·  1-8 players";
        ImDrawList* fl = ImGui::GetForegroundDrawList();
        const ImVec2 ts = g.font_small->CalcTextSizeA(g.font_small->FontSize, FLT_MAX, 0, hint);
        const ImVec2 at{(io.DisplaySize.x - ts.x) / 2, io.DisplaySize.y - 128 * s};
        fl->AddRectFilled({at.x - 10 * s, at.y - 4 * s}, {at.x + ts.x + 10 * s, at.y + ts.y + 4 * s}, Rgba(0.02f, 0.03f, 0.05f, 0.55f), 6 * s);
        fl->AddText(g.font_small, g.font_small->FontSize, at, Rgba(1, 1, 1, 0.8f), hint);
    }

    const Part& part = CurPart();
    // ---- top bar
    ImGui::SetNextWindowPos({12 * s, 12 * s});
    ImGui::Begin("##top", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("<  Menu")) g.screen = Screen::Menu;
    ImGui::SameLine();
    auto toggle = [&](const char* label, bool& on, const char* tip) {
        const bool was = on;
        if (was) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.30f, 0.86f, 1.f, 0.40f});
        if (ImGui::Button(label)) on = !on;
        if (was) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
    };
    toggle("Players", g.panel_players, "Show / hide the players panel (F2)");
    toggle("View", g.panel_view, "Show / hide the view panel (F3)");
    ImGui::PushFont(g.font_big);
    ImGui::TextUnformatted(PrettyLevel(part.level_name).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", g.replay.title.c_str(), g.replay.parts.size() > 1 ? ("   part " + std::to_string(g.part + 1) + "/" + std::to_string(g.replay.parts.size())).c_str() : "");
    if (part.level_name.empty()) ImGui::TextColored({1, 0.8f, 0.3f, 1}, "Unknown map (level %016llX): players only", (unsigned long long)part.level_id);
    else if (g.levels.current() != part.level_name) ImGui::TextColored({1, 0.8f, 0.3f, 1}, "The map did not load: players only. %s", g.levels.why().c_str());
    ImGui::End();

    // ---- players panel
    if (g.panel_players) {
    ImGui::SetNextWindowPos({12 * s, 70 * s}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({240 * s, 0}, {300 * s, g.gpu.height - 220 * s});
    ImGui::Begin("Players", &g.panel_players, ImGuiWindowFlags_AlwaysAutoResize);
    // Who is in the match now, by the roster (TeamAt): one row per name, so a player who left and rejoined (a new
    // entrant number, a second track) is one row, on the track they are playing on now.
    std::vector<int> team_now(part.players.size(), -1);
    for (size_t i = 0; i < part.players.size(); ++i) team_now[i] = TeamAt(part, part.players[i], LocalT());
    for (size_t i = 0; i < part.players.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (team_now[i] >= 0 && team_now[j] >= 0 && part.players[i].name == part.players[j].name) {
                PlayerPose pp;
                if (SamplePlayer(part.players[i], LocalT(), pp)) team_now[j] = -1;
                else team_now[i] = -1;
            }
    for (int team : {0, 1, 2}) {
        ImGui::TextColored(team == 2 ? kGrey : TeamCol(uint32_t(team)), team == 0 ? "BLUE" : team == 1 ? "ORANGE" : "SPECTATORS");
        bool any = false;
        for (size_t i = 0; i < part.players.size(); ++i) {
            const PlayerTrack& p = part.players[i];
            if (team_now[i] != team) continue;
            any = true;
            PlayerPose pose;
            const bool present = SamplePlayer(p, LocalT(), pose);
            ImGui::PushID(int(i));
            bool vis = !g.hidden[i];
            if (ImGui::Checkbox("##v", &vis)) g.hidden[i] = !vis;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show / hide");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, present && team < 2 ? TeamCol(uint32_t(team)) : ImVec4{0.5f, 0.5f, 0.55f, 1});
            const std::string label = (i < 9 ? std::to_string(i + 1) + "  " : "   ") + p.name + (p.recorder && p.guessed ? "  (team guessed)" : "");
            if (ImGui::Selectable(label.c_str(), g.focus == int(i), ImGuiSelectableFlags_AllowDoubleClick)) {
                g.focus = int(i);
                g.mode = ImGui::IsMouseDoubleClicked(0) ? CamMode::Pov : CamMode::Follow;
            }
            ImGui::PopStyleColor();
            if (!p.voice.empty()) {
                ImGui::SameLine();
                const bool m = voice::PlayerMuted(p.slot);
                ImGui::PushStyleColor(ImGuiCol_Text, m ? ImVec4{1, 0.4f, 0.4f, 1} : (present && pose.speaking ? ImVec4{0.4f, 1.f, 0.45f, 1} : kGrey));
                if (ImGui::SmallButton(m ? ICON_LC_MIC_OFF : ICON_LC_MIC)) voice::SetPlayerMuted(p.slot, !m);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(m ? "Unmute their voice" : "Mute their voice");
                const float lv = voice::Level(p.slot);
                if (lv > 0.01f) {
                    ImGui::SameLine();
                    const ImVec2 c = ImGui::GetCursorScreenPos();
                    const float h = ImGui::GetTextLineHeight();
                    ImGui::GetWindowDrawList()->AddRectFilled({c.x, c.y + h * 0.3f}, {c.x + 40 * s * lv, c.y + h * 0.7f}, Rgba(0.4f, 1.f, 0.45f), 2);
                    ImGui::Dummy({40 * s, h});
                }
            }
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled(team == 2 ? "  none in the recording now" : "  nobody");
    }
    if (part.players.empty()) ImGui::TextDisabled("No players in this part.");
    ImGui::TextDisabled("Click: follow   Double-click / P: first person");
    ImGui::End();
    }

    // ---- view panel
    const float vw = 270 * s;
    if (g.panel_view) {
    ImGui::SetNextWindowPos({g.gpu.width - vw - 12 * s, 70 * s}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({vw, 0}, ImGuiCond_FirstUseEver);
    ImGui::Begin("View", &g.panel_view, ImGuiWindowFlags_AlwaysAutoResize);
    int m = int(g.mode);
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(0, 4 * s);
        if (i == 3) ImGui::NewLine();
        const bool on = m == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.30f, 0.86f, 1.f, 0.40f});
        if (ImGui::Button(kCamNames[i])) {
            if ((i == 1 || i == 2) && g.focus < 0 && !part.players.empty()) g.focus = 0;
            g.mode = CamMode(i);
        }
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SliderFloat("FOV", &g.fov_deg, 40, 110, "%.0f deg");
    switch (g.mode) {
        case CamMode::Free:
            ImGui::SliderFloat("Fly speed", &g.cam_speed, 0.5f, 80, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Smoothing", &g.free_smooth, 1, 30, "%.0f");
            break;
        case CamMode::Follow: {
            const char* views[] = {"Behind##fv", "Overhead##fv", "Side##fv"};
            for (int v = 0; v < 3; ++v) {
                if (v) ImGui::SameLine(0, 4 * s);
                const bool on = g.follow_view == v;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.30f, 0.86f, 1.f, 0.40f});
                if (ImGui::Button(views[v])) g.follow_view = v, g.orbit_yaw = 0, g.orbit_pitch = 0.25f;
                if (on) ImGui::PopStyleColor();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(G)");
            ImGui::SliderFloat("Distance", &g.orbit_dist, 0.8f, 20, "%.1f m", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Height", &g.follow_height, -0.5f, 2.f, "%.2f m");
            ImGui::SliderFloat("Follow lag", &g.follow_smooth, 1, 30, "%.0f");
            ImGui::SliderFloat("Turn lag", &g.follow_turn, 0.5f, 20, "%.1f");
            break;
        }
        case CamMode::Pov: ImGui::SliderFloat("POV smoothing", &g.pov_smooth, 0, 20, "%.0f"); break;
        case CamMode::Disc: ImGui::SliderFloat("Distance", &g.orbit_dist, 0.8f, 20, "%.1f m", ImGuiSliderFlags_Logarithmic); break;
        case CamMode::Top: ImGui::SliderFloat("Height", &g.orbit_dist, 0.8f, 40, "%.1f", ImGuiSliderFlags_Logarithmic); break;
    }
    ImGui::SliderFloat("Mouse", &g.mouse_sens, 0.2f, 3.f, "%.1fx");
    ImGui::SameLine();
    ImGui::Checkbox("Invert Y", &g.invert_y);
    ImGui::Separator();
    ImGui::Checkbox("Players", &g.show_players);
    ImGui::SameLine(130 * s);
    ImGui::Checkbox("Names", &g.show_names);
    ImGui::Checkbox("Bodies", &g.show_bodies);
    ImGui::SameLine(130 * s);
    ImGui::BeginDisabled(g.avatars.empty());
    ImGui::Checkbox("Models", &g.show_models);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", g.avatars.empty() ? "The Echo chassis could not be read from the game files (see avatar.log)" : "Real Echo chassis instead of wire figures");
    ImGui::Checkbox("X-ray", &g.xray);
    ImGui::SameLine(130 * s);
    ImGui::Checkbox("Trails", &g.show_trails);
    ImGui::Checkbox("Disc trail", &g.disc_trail);
    ImGui::SameLine(130 * s);
    ImGui::Checkbox("Event feed", &g.show_events);
    if (voice::Ready()) {
        ImGui::Checkbox("Mute voice", &voice::muted);
        ImGui::SameLine();
        ImGui::Checkbox("Positional", &voice::positional);
        int pct = int(voice::volume * 100 + 0.5f);
        if (ImGui::SliderInt("Voice volume", &pct, 0, 200, "%d%%")) voice::volume = pct / 100.f;
    } else {
        ImGui::TextDisabled("Voice off: %s", voice::Why().c_str());
    }
    ImGui::SliderFloat("Trail", &g.trail_s, 0.2f, 6, "%.1f s");
    ImGui::SliderFloat("Line weight", &g.thickness, 0, 2, "%.1f");
    if (ImGui::TreeNode("Advanced")) {
        const char* axes[] = {"+X", "-X", "+Z", "-Z"};
        ImGui::Combo("Head forward", &g.forward_axis, axes, 4);
        ImGui::TextDisabled("Picked from the recording; change it if heads face backwards.");
        ImGui::TreePop();
    }
    if (!g.bookmarks.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Bookmarks (M)");
        for (size_t i = 0; i < g.bookmarks.size(); ++i) {
            ImGui::PushID(int(i));
            if (ImGui::SmallButton(Clock(g.bookmarks[i], true).c_str())) Seek(g.bookmarks[i]);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                g.bookmarks.erase(g.bookmarks.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
    }

    // ---- transport
    const float bw = std::min(g.gpu.width - 24 * s, 1500 * s);
    ImGui::SetNextWindowPos({(g.gpu.width - bw) / 2, g.gpu.height - 104 * s});
    ImGui::SetNextWindowSize({bw, 92 * s});
    ImGui::Begin("##transport", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    Timeline(ImGui::GetContentRegionAvail().x);
    ImGui::PushFont(g.font_big);
    auto icon = [&](const char* t2, const char* tip) {
        const bool r = ImGui::Button(t2, {42 * s, 34 * s});
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0, 4 * s);
        return r;
    };
    if (icon("|<", "Start (Home)")) Seek(0);
    if (icon("<<", "Back 5 s (Left, Shift: 1 s)")) Seek(g.t - 5);
    if (icon("<|", "Step back one frame (,)")) g.playing = false, Seek(g.t - 1 / 45.0);
    if (icon(g.playing ? "||" : ">", "Play / pause (Space)")) g.playing = !g.playing;
    if (icon("|>", "Step forward one frame (.)")) g.playing = false, Seek(g.t + 1 / 45.0);
    if (icon(">>", "Forward 5 s (Right)")) Seek(g.t + 5);
    if (icon(">|", "End (End)")) Seek(g.replay.duration);
    ImGui::PopFont();
    ImGui::SameLine(0, 18 * s);
    ImGui::PushFont(g.font_mono);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s / %s", Clock(g.t, true).c_str(), Clock(g.replay.duration).c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, 18 * s);
    const double speeds[] = {-4, -2, -1, -0.5, -0.25, 0.1, 0.25, 0.5, 1, 1.5, 2, 4};
    for (double sp : speeds) {
        char b[16];
        snprintf(b, sizeof b, sp == int(sp) ? "%.0fx" : "%.2gx", sp);
        const bool on = std::fabs(g.speed - sp) < 1e-6;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.30f, 0.86f, 1.f, 0.45f});
        ImGui::PushID(b);
        if (ImGui::Button(b)) g.speed = sp, g.playing = true;
        ImGui::PopID();
        if (on) ImGui::PopStyleColor();
        ImGui::SameLine(0, 3 * s);
    }
    ImGui::SameLine(0, 14 * s);
    ImGui::Checkbox("Loop", &g.loop);
    ImGui::SameLine();
    if (ImGui::Button("Bookmark")) g.bookmarks.push_back(g.t), std::sort(g.bookmarks.begin(), g.bookmarks.end());
    ImGui::SameLine();
    if (ImGui::Button("?")) ImGui::OpenPopup("keys");
    if (ImGui::BeginPopup("keys")) {
        ImGui::TextUnformatted(
            "Space  play / pause          Left / Right  5 s (Shift 1 s)\n"
            ", .    one frame             [ ]   slower / faster   R  reverse   Backspace  1x\n"
            "Home / End                   M     bookmark\n"
            "Right mouse  look / orbit    W A S D Q E  fly (Shift fast, Ctrl slow)   wheel  speed / distance\n"
            "1-9 / Tab  follow a player   V  first person   C  next camera   F  free camera\n"
            "N  names   X  x-ray   H  hide the interface   F12  screenshot   Esc  back to the menu");
        ImGui::EndPopup();
    }
    ImGui::End();

    if (!g.status.empty()) {
        ImGui::GetForegroundDrawList()->AddText(g.font_small, g.font_small->FontSize, {16 * s, g.gpu.height - 128 * s}, Rgba(0.8f, 0.85f, 0.9f, 0.9f), g.status.c_str());
    }
}

void LoadingScreen(double now) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    DrawBackground(ImGui::GetBackgroundDrawList(), size, now);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ts = g.font_big->CalcTextSizeA(g.font_big->FontSize, FLT_MAX, 0, g.loading_text.c_str());
    dl->AddText(g.font_big, g.font_big->FontSize, {(size.x - ts.x) / 2, size.y / 2 - ts.y}, Rgba(1, 1, 1), g.loading_text.c_str());
    const float r = 14 * g.dpi;
    const float a = float(now * 5);
    dl->PathArcTo({size.x / 2, size.y / 2 + 30 * g.dpi}, r, a, a + 4.2f, 24);
    dl->PathStroke(ImGui::ColorConvertFloat4ToU32(kAccent), 0, 3 * g.dpi);
    if (g_open) {   // what the worker is on now
        const char* st = g_open->stage.load();
        const ImVec2 ss = g.font->CalcTextSizeA(g.font->FontSize, FLT_MAX, 0, st);
        dl->AddText(g.font, g.font->FontSize, {(size.x - ss.x) / 2, size.y / 2 + 56 * g.dpi}, Rgba(1, 1, 1, 0.6f), st);
    }
}

void SetMouseLock(bool on) {
    if (on == g.mouse_lock) return;
    g.mouse_lock = on;
    g.raw_dx = g.raw_dy = 0;
    ImGuiIO& io = ImGui::GetIO();
    if (on) {
        GetCursorPos(&g.last_mouse);
        g.abs_have = false;
        RECT r;
        GetClientRect(g.hwnd, &r);
        MapWindowPoints(g.hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);   // kept inside the window (hidden) so a click can never land on another app
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        SetCursor(nullptr);
    } else {
        ClipCursor(nullptr);
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // while looking with the right mouse the cursor is hidden; at any other time Windows / ImGui own it
    if (msg == WM_INPUT) {
        if (g.rmb || g.mouse_lock) {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData(HRAWINPUT(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) && ri.header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& rm = ri.data.mouse;
                if ((rm.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    if (rm.lLastX || rm.lLastY) g.raw_dx += float(rm.lLastX), g.raw_dy += float(rm.lLastY), g.raw_rel = true;
                } else {
                    const bool virt = (rm.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                    const float w = float(GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN));
                    const float hh = float(GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN));
                    const float x = rm.lLastX / 65535.f * w, y = rm.lLastY / 65535.f * hh;
                    if (g.abs_have) g.abs_dx += x - g.abs_x, g.abs_dy += y - g.abs_y, g.abs_seen = true;
                    g.abs_x = x, g.abs_y = y, g.abs_have = true;
                }
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    if (g.mouse_lock && msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        SetMouseLock(false);
        return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && (g.rmb || g.mouse_lock)) {
        SetCursor(nullptr);
        return TRUE;
    }
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && g.renderer) g.gpu.Resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_RBUTTONDOWN:
            if (g.screen == Screen::Player && (g.mouse_lock || !ImGui::GetIO().WantCaptureMouse)) {
                g.rmb = true;
                g.raw_dx = g.raw_dy = 0;
                g.abs_have = false;
                if (!g.mouse_lock) GetCursorPos(&g.last_mouse);
                SetCapture(h);
            }
            return 0;
        case WM_LBUTTONDOWN:
            // click the view: the mouse looks around (Echo's spectator free look) until Esc
            if (g.screen == Screen::Player && !g.mouse_lock && !ImGui::GetIO().WantCaptureMouse) SetMouseLock(true);
            return 0;
        case WM_KILLFOCUS:
            SetMouseLock(false);
            [[fallthrough]];
        case WM_RBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g.rmb) {
                g.rmb = false;
                if (msg != WM_CAPTURECHANGED) ReleaseCapture();
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            }
            return 0;
        case WM_DROPFILES: {
            wchar_t p[MAX_PATH];
            if (DragQueryFileW(HDROP(wp), 0, p, MAX_PATH)) RequestOpen(p);
            DragFinish(HDROP(wp));
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {1024, 640};
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Echo's data, read in place from the install (the one chosen in the menu, else every one found).
void MountGame() {
    std::vector<fs::path> installs;
    const std::wstring chosen = IniGet(L"echo");
    if (!chosen.empty() && IsEchoInstall(chosen)) installs.push_back(chosen);
    for (const fs::path& p : FindEchoInstalls()) {
        bool dup = false;
        for (auto& q : installs) dup |= _wcsicmp(q.c_str(), p.c_str()) == 0;
        if (!dup) installs.push_back(p);
    }
    gamefiles::Mount(installs, AppDir() / "echo");
    g.levels_ok = g.levels.Init(g.renderer.get(), gamefiles::Corpus());
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(nullptr, IDI_APPLICATION), LoadCursorW(nullptr, IDC_ARROW),
                   nullptr, nullptr, L"EchoReplayViewer", nullptr};
    RegisterClassExW(&wc);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Echo Replay Viewer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1600, 940,
                             nullptr, nullptr, inst, nullptr);
    g.dpi = float(GetDpiForWindow(g.hwnd)) / 96.f;
    DragAcceptFiles(g.hwnd, TRUE);
    {   // raw mouse motion for free look (unaffected by the cursor being clipped / at the screen edge)
        RAWINPUTDEVICE rid{0x01, 0x02, 0, g.hwnd};
        RegisterRawInputDevices(&rid, 1, sizeof rid);
    }

    if (erv_version() != ERV_API_VERSION) {
        MessageBoxW(g.hwnd, L"echoreplay_reader.dll is a different version. Rebuild EchoReplay and this viewer.", L"Echo Replay Viewer", MB_ICONERROR);
        return 1;
    }
    auto created = echo::render::PreviewRenderer::create({});
    if (!created.ok()) {
        MessageBoxW(g.hwnd, L"The renderer could not start (needs a D3D12 GPU with ray tracing, as EchoConstruct does).", L"Echo Replay Viewer", MB_ICONERROR);
        return 1;
    }
    g.renderer = std::move(created.value());
    MountGame();
    voice::Init();

    // --test-voice <replay> <out.wav>: decode the busiest speaker's voice to a wav, log, exit.
    {
        int argc2 = 0;
        LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &argc2);
        for (int i = 1; i + 2 < argc2; ++i) {
            if (wcscmp(av[i], L"--test-voice") != 0) continue;
            FILE* log = nullptr;
            _wfopen_s(&log, (AppDir() / "test.log").c_str(), L"w");
            Replay r;
            std::string why;
            if (!r.Load(av[i + 1])) why = "load failed: " + r.error;
            const VoiceTrack* best = nullptr;
            size_t players = 0;
            for (const Part& part : r.parts) {
                players += part.players.size();
                for (const VoiceTrack& vt : part.voices)
                    if (!best || vt.t.size() > best->t.size()) best = &vt;
            }
            int n = -1;
            if (best && !best->t.empty()) n = voice::DecodeToWav(*best, av[i + 2], why);
            if (log) {
                fprintf(log, "voice init: %s\n", voice::Ready() ? "ok" : voice::Why().c_str());
                fprintf(log, "players drawn %zu\n", players);
                for (const Part& part : r.parts)
                    for (const PlayerTrack& pt : part.players)
                        fprintf(log, "  %08X %-14s team %u  %.1f..%.1f s  pose %zu body %zu voice %zu%s%s\n", pt.slot, pt.name.c_str(), pt.team, pt.first,
                                pt.last, pt.pose.size(), pt.body.size(), pt.voice.size(), pt.recorder ? "  RECORDER" : "", pt.guessed ? " (guessed)" : "");
                fprintf(log, "speaker %s: %zu frames, %d decoded. %s\n", best ? best->name.c_str() : "-", best ? best->t.size() : 0, n, why.c_str());
                fclose(log);
            }
            LocalFree(av);
            return 0;
        }
        LocalFree(av);
    }

    // --test-level <level> <out.png>: load one map from the game's packages, capture it, log, exit.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--make-previews") == 0) {   // the hidden helper the menu starts: art for each map
                const auto maps = KnownMaps();
                int n = 0;
                for (const auto& [id, name] : maps) {
                    ++n;
                    if (Levels::HasPreview(name)) continue;
                    {
                        std::ofstream pr(AppDir() / "previews.progress");
                        pr << PrettyLevel(name) << "  (" << n << "/" << maps.size() << ")\n";
                    }
                    if (g.levels_ok) g.levels.MakePreview(name);
                }
                std::error_code ec;
                fs::remove(AppDir() / "previews.progress", ec);
                LocalFree(argv);
                return 0;
            }
            if (wcscmp(argv[i], L"--test-level") != 0 || i + 2 >= argc) continue;
            const std::string level = U8(argv[i + 1]);
            const fs::path out = argv[i + 2];
            FILE* log = nullptr;
            _wfopen_s(&log, (AppDir() / "test.log").c_str(), L"w");
            auto say = [&](const std::string& s) { if (log) fprintf(log, "%s\n", s.c_str()), fflush(log); };
            for (const fs::path& p : gamefiles::Installs()) say("install " + U8(p.wstring()));
            for (const auto& r : gamefiles::Reports()) say(std::string(r.ok ? "  ok   " : "  skip ") + r.line);
            say("corpus " + U8(gamefiles::Corpus().wstring()) + "  why: " + gamefiles::Why());
            const double t0 = Now();
            const bool ok = g.levels_ok && g.levels.Load(level);
            say(std::string("load ") + level + ": " + (ok ? "ok" : "FAILED " + g.levels.why()) + "  variant '" + g.levels.variant() + "'  " +
                std::to_string(Now() - t0) + " s, files served " + std::to_string(gamefiles::FilesServed()) + ", read failures " +
                std::to_string(gamefiles::ReadFailures()));
            for (const std::string& sec : g.levels.sections()) say("  section " + sec);
            if (ok) {
                g.renderer->set_game_view(true);
                g.renderer->set_view_layers(0);
                g.renderer->wait_for_shading();
                auto frame = g.renderer->capture(g.levels.Overview(16.f / 9.f), 1920, 1080, {.selection_outline = false});
                say(frame.ok() && echo::ui::write_png_file(out, frame.value()).ok() ? "captured " + U8(out.wstring()) : "capture failed");
            }
            if (log) fclose(log);
            LocalFree(argv);
            return ok ? 0 : 2;
        }
        LocalFree(argv);
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(g.hwnd);
    SetupStyle();
    if (!g.gpu.Init(g.hwnd, g.renderer->device(), g.renderer->queue())) {
        MessageBoxW(g.hwnd, L"The window's swap chain could not be created.", L"Echo Replay Viewer", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g.hwnd);

    LoadBackgrounds();

    // the folder: last used, else Documents\EchoReplay
    std::wstring recent = IniGet(L"recent");
    for (size_t a = 0; a < recent.size();) {
        const size_t b = recent.find(L'|', a);
        if (b == std::wstring::npos) break;
        if (b > a) g.recent.push_back(recent.substr(a, b - a));
        a = b + 1;
    }
    SetRecorderName(U8(IniGet(L"me", L"You")));   // what to call the person who recorded (ini me=)
    std::wstring folder = IniGet(L"folder");
    if (folder.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) folder = std::wstring(p) + L"\\EchoReplay";
        CoTaskMemFree(p);
    }
    SetFolder(folder);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--folder") == 0 && i + 1 < argc) SetFolder(argv[++i]);
        else if (wcscmp(argv[i], L"--seek") == 0 && i + 1 < argc) g.t = _wtof(argv[++i]);
        else if (wcscmp(argv[i], L"--shot") == 0 && i + 1 < argc) g.shot_path = argv[++i];
        else if (wcscmp(argv[i], L"--views") == 0 && i + 1 < argc) g.shot_views = argv[++i];
        else if (wcscmp(argv[i], L"--closeup") == 0 && i + 1 < argc) g.closeup = _wtoi(argv[++i]);
        else RequestOpen(argv[i]);
    }
    const double startSeek = g.t;
    LocalFree(argv);

    double last = Now();
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(g.hwnd)) {
            Sleep(30);
            continue;
        }
        const double now = Now();
        const float dt = float(std::min(0.1, now - last));
        last = now;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(g.font);
        bool openNow = false;
        switch (g.screen) {
            case Screen::Menu: ++g.menu_frames, MenuScreen(now); break;
            case Screen::Loading: LoadingScreen(now), openNow = true; break;
            case Screen::Player: PlayerScreen(now, dt); break;
        }
        if (g.screen != Screen::Player && voice::Ready()) voice::Stop();
        if (now < g.error_until) {
            const ImVec2 size = io.DisplaySize;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 ts = g.font->CalcTextSizeA(g.font->FontSize, size.x * 0.6f, size.x * 0.6f, g.error_text.c_str());
            const ImVec2 a{(size.x - ts.x) / 2 - 16, size.y - 150 * g.dpi}, b{a.x + ts.x + 32, a.y + ts.y + 20};
            dl->AddRectFilled(a, b, Rgba(0.45f, 0.08f, 0.08f, 0.92f), 8);
            dl->AddText(g.font, g.font->FontSize, {a.x + 16, a.y + 10}, Rgba(1, 1, 1), g.error_text.c_str(), nullptr, size.x * 0.6f);
        }
        ImGui::PopFont();
        ImGui::Render();
        g.gpu.Present(ImGui::GetDrawData());
        if (openNow && !g.pending_open.empty() && !g_open) {   // after the loading frame is on screen
            const fs::path p = g.pending_open;
            g.pending_open.clear();
            StartOpen(p);
        }
        if (g_open && g_open->done) {
            static bool first = true;   // --seek: the replay given on the command line only
            FinishOpen(first ? startSeek : 0);
            first = false;
            last = Now();
        }
    }
    if (g_open) g_open->worker.join();   // (closed while a replay was loading)
    g.lib.Stop();
    g.gpu.Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g.renderer.reset();
    return 0;
}
