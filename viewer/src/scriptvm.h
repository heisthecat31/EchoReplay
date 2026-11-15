#pragma once
// Echo's own screen scripts, run on the replay. A stock script (a compiled CScriptCR DLL) is lifted to a graph by
// EchoConstruct's lifter (script::lift_stock_script); this runs that graph: its OnInitialize / OnStart entries, an
// OnEvent for every net variable the replay changes, and the screen nodes it reaches (SetText2Node, ShowUINode,
// SetAlphaUINode, CreateUIBindingsNode). The net variables come from the recording (game_state, game_timer,
// countdown_timer, blue_score, red_score); one the recording does not hold reads as unset, as on a fresh client.
//
// What the scripts write is kept per (screen actor, canvas component, item): the arena's screens apply it to the
// stock canvases (Levels::SetBoards).

#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace erv::script {

struct Value {
    enum Kind : uint8_t { None, Int, Real, Bool, Sym, Str } kind = None;
    int64_t i = 0;
    double f = 0;
    uint64_t s = 0;
    std::string str;
    static Value I(int64_t v) { Value x; x.kind = Int; x.i = v; return x; }
    static Value R(double v) { Value x; x.kind = Real; x.f = v; return x; }
    static Value B(bool v) { Value x; x.kind = Bool; x.i = v; return x; }
    static Value S(uint64_t v) { Value x; x.kind = Sym; x.s = v; return x; }
    static Value T(std::string v) { Value x; x.kind = Str; x.str = std::move(v); return x; }
    int64_t AsInt() const;
    double AsReal() const;
    bool AsBool() const;
    uint64_t AsSym() const;
    std::string AsStr() const;
    bool operator==(const Value& o) const { return kind == o.kind && i == o.i && f == o.f && s == o.s && str == o.str; }
};

// One lifted graph (the emk2-graph text script::StockLift::graph holds).
struct Graph;
std::shared_ptr<const Graph> ParseGraph(const std::string& text, std::string* why = nullptr);
// True when the graph writes to a screen (the scripts worth running here).
bool WritesScreens(const Graph&);
size_t NodeCount(const Graph&);
// True when every node the graph holds is one the lifter could read (no Raw parts carried along).
bool Whole(const Graph&);
// True when every Raw part the graph carries is one this runs from its record (a function output set to a
// constant, a thread start, a branch on an operator chain, an actor test); `why` names the first that is not.
bool Runnable(const Graph&, std::string* why = nullptr);

// What the scripts wrote, per screen item.
struct ItemKey {
    uint64_t actor = 0;       // the screen actor's nodeid
    uint64_t component = 0;   // its CCanvasUICR instance name
    uint64_t item = 0;        // the canvas item's name
    bool operator<(const ItemKey& o) const {
        return actor != o.actor ? actor < o.actor : component != o.component ? component < o.component : item < o.item;
    }
};
struct ItemState {
    std::optional<std::string> text;
    std::optional<bool> shown;
    std::optional<float> alpha;
};

// The script instance's parameters (its CScriptCR row's variable tables), by variable name hash.
using Params = std::map<uint64_t, Value>;

// One lobby member, from the recording: what R15NetTeamSlotToUserIdExpression, R15NetUserExpression and
// R15NetPlayerMatchStatExpression read. A user id is the member's index in the list.
struct Player {
    int64_t team = 0;   // 0 blue, 1 orange
    int64_t slot = 0;   // in the team
    std::string name;
    int64_t ping = 0;
    std::map<uint64_t, int64_t> stats;   // by stat name hash
};

class Machine {
public:
    Machine();
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    void Clear();
    // One running instance of `graph` on the actor `self` (nodeid).
    void Add(std::shared_ptr<const Graph> graph, uint64_t self, const Params& params);
    size_t Instances() const { return inst_.size(); }

    // Feeds the net variables: the first call runs OnInitialize and OnStart, every call then sends an OnEvent per
    // variable whose value changed since the last (game_state first). Returns true when any screen item changed.
    bool Tick(const std::map<uint64_t, Value>& net);
    // The lobby the scripts read; then a game event (a delegate or nevt_*) sent to every script.
    void SetPlayers(std::vector<Player> p) { players_ = std::move(p); }
    void SendEvent(uint64_t event);

    const std::map<ItemKey, ItemState>& Items() const { return items_; }
    uint64_t Generation() const { return generation_; }
    // Diagnostics: node kinds the scripts reached that this does not run (each once).
    const std::vector<std::string>& Unknown() const { return unknown_; }

private:
    struct Inst;
    std::vector<std::unique_ptr<Inst>> inst_;
    std::map<uint64_t, Value> net_;
    bool started_ = false;
    std::map<ItemKey, ItemState> items_;
    uint64_t generation_ = 0;
    bool dirty_ = false;
    std::vector<std::string> unknown_;
    std::vector<Player> players_;
    // Runs the OnEvent entries listening for `event`: on the scripts of `actor` (its nodeid), or on every script
    // when `actor` is 0.
    void Send(uint64_t event, uint64_t actor, int depth);
    friend struct Run;
};

}  // namespace erv::script
