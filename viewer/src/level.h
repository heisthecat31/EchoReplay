#pragma once
// Stock levels from your Echo install, through EchoConstruct: the same load its editor viewport and its
// --capture CLI do, into the viewer's one renderer.

#include <stdint.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <echo/renderer/camera.hpp>
#include <echo/renderer/preview.hpp>

namespace echo::model { struct Document; }

namespace erv {

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

class Levels {
public:
    Levels();
    ~Levels();
    // `corpus`: the folder EchoConstruct's loaders read (gamefiles::Corpus(), Echo's data read in place).
    bool Init(echo::render::PreviewRenderer* renderer, const std::filesystem::path& corpus);
    const std::string& why() const { return why_; }
    const std::filesystem::path& corpus() const { return corpus_; }

    // Loads a level by name (e.g. "mpl_arena_a"). Keeps the current one if it is that already.
    bool Load(const std::string& level);
    const std::string& current() const { return current_; }
    // A camera that frames the level (for the menu previews and a starting view).
    echo::render::FlyCamera Overview(float aspect) const;

    // Map art: a 960x540 frame of each map, cached under %LOCALAPPDATA%\EchoReplayViewer\maps\<level>.erva.
    static std::filesystem::path CacheDir();
    static std::filesystem::path ArtFile(const std::string& level);
    static bool HasPreview(const std::string& level);
    bool MakePreview(const std::string& level);   // loads the level and captures it (slow: once per map)
    static bool ReadImage(const std::filesystem::path&, Image&);
    static Image Blurred(const Image&);           // half size, soft: the menu's background

    // Which manifest variant the last load used ("" = the live one).
    const std::string& variant() const { return variant_; }

    const std::vector<std::string>& sections() const { return layer_names_; }

    // The arena's screens as the game fills them at one moment of the replay: the live scoreboards and clock,
    // the launch tubes' countdown, and the end-of-match board (with its player cards and rematch console),
    // which the game only brings out after the match. Cheap when nothing shown changed.
    struct BoardPlayer {
        std::string name;
        uint32_t team = 0;      // 0 blue, 1 orange
        int points = 0, assists = 0, stuns = 0;
        int saves = -1, ping = -1;   // -1: the recording does not hold it
    };
    struct BoardState {
        bool valid = false;
        uint32_t phase = 0, round = 0, blue = 0, orange = 0;   // phase: demotypes.h (8 RoundOver, 9 PostMatch); round from 0
        float clock = 0, countdown = 0;
        std::vector<BoardPlayer> players;
        struct Goal {                 // the latest goal at this moment (the goal ends' info boards)
            bool have = false;
            float since = 0;          // seconds since it was scored
            std::string scorer, assist;   // "" when the recording does not name them
            uint32_t points = 0, type = 0;
            float speed = 0, distance = 0;
            float clock = -1;         // the game clock when it went in (-1 unknown)
        } goal;
    };
    void SetBoards(const BoardState&);
    // The level's own disc (its model and bubble) follows the recorded one: world position and rotation
    // (x, y, z, w), local +y the disc's axis. have = false hides it.
    void SetDisc(bool have, const float pos[3], const float rot[4]);
    // The goal ends' info boards are one-sided panels the game shows from both sides: each is turned to read
    // the right way round from `eye` (the camera about to draw).
    void FaceViewer(const float eye[3]);
    bool HasDisc() const { return !disc_parts_.empty(); }
    // What the screen scripts did on load: which ran, which were not run and why (shown in the log).
    const std::string& ScriptsLog() const;
private:
    struct Boards;
    std::unique_ptr<Boards> boards_;
    struct Loaded;                     // the open level's session and mesh / material sources, kept while it is shown
    std::unique_ptr<Loaded> live_;
    std::vector<std::string> layer_names_;
    struct DiscPart { echo::ActorId id; echo::render::Affine local; };   // pose in the disc root's frame
    std::vector<DiscPart> disc_parts_;
    bool play_ok_ = false;
    struct P3 { float x, y, z; } play_lo_{}, play_hi_{};
    bool TryLoad(const std::string& level);
    void StartScripts(const echo::model::Document& doc);
    void TurnGoalText(echo::render::CanvasSet& set) const;   // Echo's screen scripts for the open level (scriptvm.h)
    std::string variant_;
    echo::render::PreviewRenderer* r_ = nullptr;
    std::filesystem::path corpus_;
    std::string current_, why_;
};

}  // namespace erv
