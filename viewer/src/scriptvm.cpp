#include "scriptvm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>

namespace erv::script {

namespace {

constexpr uint64_t kNetVarChanged = 0x6b26f01883349f28ull;   // OnEvent: a net variable changed
constexpr uint64_t kGameState = 0xb2996ad399fdc0ffull;
constexpr uint64_t kUnset = ~0ull;                           // a symbol pin left at "none"

uint64_t Hex(const std::string& s) { return strtoull(s.c_str(), nullptr, 16); }

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
        const size_t b = line.find('\t', a);
        f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return f;
}

}  // namespace

int64_t Value::AsInt() const {
    switch (kind) {
        case Int: case Bool: return i;
        case Real: return int64_t(f);
        case Sym: return int64_t(s);
        case Str: return strtoll(str.c_str(), nullptr, 10);
        default: return 0;
    }
}
double Value::AsReal() const { return kind == Real ? f : double(AsInt()); }
bool Value::AsBool() const { return kind == Real ? f != 0 : kind == Str ? !str.empty() : AsInt() != 0; }
uint64_t Value::AsSym() const { return kind == Sym ? s : kind == None ? 0 : uint64_t(AsInt()); }
std::string Value::AsStr() const {
    char b[32];
    switch (kind) {
        case Str: return str;
        case Int: return std::to_string(i);
        case Bool: return i ? "1" : "0";
        case Real: snprintf(b, sizeof b, "%g", f); return b;
        case Sym: snprintf(b, sizeof b, "%016llx", (unsigned long long)s); return b;
        default: return {};
    }
}

struct Graph {
    struct Node {
        std::string type;
        std::map<std::string, Value> lits;
        std::string nvar;                   // SetVariable's variable, CallFunction's function
        std::map<int, uint64_t> exits;      // exit index -> pin name hash (Switch)
        // A part the lifter carries as Raw: its stock record's class and port values, and the operator
        // chains it computes its inputs with (rexpr), by port.
        std::string raw_class;
        std::map<std::string, uint64_t> raw_ports;
        std::map<std::string, std::string> raw_expr;
    };
    struct Var {
        std::string kind;
        uint64_t hash = 0;
        Value def;
    };
    std::map<uint32_t, Node> nodes;
    std::map<std::pair<uint32_t, int>, std::vector<uint32_t>> links;
    std::map<std::pair<uint32_t, std::string>, std::pair<uint32_t, std::string>> data;
    std::map<std::pair<uint32_t, std::string>, std::string> reads;
    std::map<std::string, Var> vars;
    std::map<std::string, uint32_t> funcs;
    std::vector<std::string> raw_classes;   // every Raw part's class, once
};

std::shared_ptr<const Graph> ParseGraph(const std::string& text, std::string* why) {
    auto g = std::make_shared<Graph>();
    std::istringstream in(text);
    std::string line;
    auto id = [](const std::string& s) { return uint32_t(strtoul(s.c_str(), nullptr, 10)); };
    // Raw records name nodes by their stock id: a Raw part's is in its rnode line, an editor node's is its
    // id (the low 32 bits). An exit whose target has bit 32 set is the thread-ending stub.
    std::map<uint64_t, uint32_t> raw_ids;
    struct RawLink { uint32_t from; int pin; uint64_t to; };
    struct RawData { uint32_t to; std::string port; uint64_t from; std::string from_port; };
    std::vector<RawLink> raw_links;
    std::vector<RawData> raw_data;
    auto lit_value = [](int kind, const std::string& num, const std::string& str) {
        switch (kind) {
            case 0: {   // hex bits: a 32-bit int (ffffffff is -1), or the 64-bit pattern
                const uint64_t bits = Hex(num);
                return Value::I(bits >> 32 ? int64_t(bits) : int64_t(int32_t(uint32_t(bits))));
            }
            case 1: { const uint32_t bits = uint32_t(Hex(num)); float x; std::memcpy(&x, &bits, 4); return Value::R(x); }
            case 4: return Value::B(num != "0");
            case 6: return Value::T(str);
            default: return Value::S(Hex(num));   // 2 symbol, 7 actor
        }
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = Split(line);
        const std::string& k = f[0];
        if (k == "node" && f.size() >= 3) {
            g->nodes[id(f[1])].type = f[2];
        } else if (k == "lit" && f.size() >= 5) {
            const std::string str = f.size() >= 6 && f[5] != "\\0" ? f[5] : "";
            g->nodes[id(f[1])].lits[f[2]] = lit_value(atoi(f[3].c_str()), f[4], str);
        } else if (k == "rnode" && f.size() >= 8) {
            // rnode id rawid class has_hash hash has_body count (name table bits)* props
            Graph::Node& n = g->nodes[id(f[1])];
            n.raw_class = f[3];
            raw_ids[Hex(f[2])] = id(f[1]);
            const size_t count = strtoull(f[7].c_str(), nullptr, 10);
            for (size_t i = 0; i < count && 8 + i * 3 + 2 < f.size(); ++i) n.raw_ports[f[8 + i * 3]] = Hex(f[8 + i * 3 + 2]);
            if (std::find(g->raw_classes.begin(), g->raw_classes.end(), f[3]) == g->raw_classes.end()) g->raw_classes.push_back(f[3]);
        } else if (k == "rexit" && f.size() >= 8) {
            // rexit id rawid 1 pin 1 pin_hash target 0
            raw_links.push_back({id(f[1]), atoi(f[4].c_str()), Hex(f[7])});
        } else if (k == "rdata" && f.size() >= 6) {
            // rdata id src_rawid src_port dst_rawid dst_port
            raw_data.push_back({id(f[1]), f[5], Hex(f[2]), f[3]});
        } else if (k == "rexpr" && f.size() >= 5) {
            g->nodes[id(f[1])].raw_expr[f[3]] = f[4];
        } else if (k == "rlit" && f.size() >= 6) {
            g->nodes[id(f[1])].lits[f[3]] = lit_value(atoi(f[4].c_str()), f[5], "");
        } else if (k == "nvar" && f.size() >= 3) {
            g->nodes[id(f[1])].nvar = f[2];
        } else if (k == "exit" && f.size() >= 4) {
            g->nodes[id(f[1])].exits[atoi(f[2].c_str())] = Hex(f[3]);
        } else if (k == "link" && f.size() >= 4) {
            g->links[{id(f[1]), atoi(f[2].c_str())}].push_back(id(f[3]));
        } else if (k == "data" && f.size() >= 5) {
            g->data[{id(f[3]), f[4]}] = {id(f[1]), f[2]};
        } else if (k == "read" && f.size() >= 4) {
            g->reads[{id(f[2]), f[3]}] = f[1];
        } else if (k == "func" && f.size() >= 3) {
            g->funcs[f[1]] = id(f[2]);
        } else if (k == "var" && f.size() >= 5) {
            Graph::Var v;
            v.kind = f[2];
            v.hash = Hex(f[3]);
            const std::string& d = f[4];
            if (d != "-") {
                if (v.kind == "flag") v.def = Value::B(d == "1");
                else if (v.kind == "int") v.def = Value::I(strtoll(d.c_str(), nullptr, 10));
                else if (v.kind == "real") v.def = Value::R(strtod(d.c_str(), nullptr));
                else if (v.kind == "string") v.def = Value::T(d);
                else v.def = Value::S(Hex(d));
            } else if (v.kind == "flag") v.def = Value::B(false);
            else if (v.kind == "int") v.def = Value::I(0);
            else if (v.kind == "real") v.def = Value::R(0);
            else if (v.kind == "string") v.def = Value::T("");
            g->vars[f[1]] = v;
        }
    }
    if (g->nodes.empty()) {
        if (why) *why = "no nodes";
        return nullptr;
    }
    auto node_of = [&](uint64_t raw) {
        const auto r = raw_ids.find(raw);
        return r != raw_ids.end() ? r->second : uint32_t(raw);
    };
    for (const RawLink& l : raw_links)
        if (!(l.to >> 32)) g->links[{l.from, l.pin}].push_back(node_of(l.to));
    for (const RawData& d : raw_data) g->data[{d.to, d.port}] = {node_of(d.from), d.from_port};
    return g;
}

bool WritesScreens(const Graph& g) {
    for (const auto& [i, n] : g.nodes)
        if (n.type == "SetText2Node" || n.type == "ShowUINode" || n.type == "SetAlphaUINode" || n.type == "CreateUIBindingsNode") return true;
    return false;
}
size_t NodeCount(const Graph& g) { return g.nodes.size(); }
bool Whole(const Graph& g) {
    for (const auto& [i, n] : g.nodes)
        if (n.type == "Raw") return false;
    return true;
}
bool Runnable(const Graph& g, std::string* why) {
    // the Raw parts this runs from their records (Run::ExecRaw, Run::Eval)
    static const char* const kRun[] = {"FLOW_SETPARAM", "UNEMITTED_SPAWN", "FLOW_BRANCH", "ACTORVALID"};
    for (const std::string& c : g.raw_classes) {
        if (std::find_if(std::begin(kRun), std::end(kRun), [&](const char* k) { return c == k; }) != std::end(kRun)) continue;
        if (why) *why = c;
        return false;
    }
    return true;
}

struct Machine::Inst {
    std::shared_ptr<const Graph> g;
    uint64_t self = 0;
    std::map<std::string, Value> vars;
    // A binding's type (bindings.N.type, as the stock scripts use it): 0 a string shown as the item's text,
    // 2 a real3 (its colour, not shown here), 3 a flag that shows or hides the item.
    struct Bound { uint64_t actor, component, item; int64_t type; };
    std::map<std::string, std::vector<Bound>> bindings;   // variable -> the items showing it
    uint64_t event_var = 0;
    std::vector<uint32_t> loops;   // While nodes whose body thread runs again each tick until the condition clears
};

namespace {
std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> w;
    std::istringstream in(s);
    for (std::string x; in >> x;) w.push_back(x);
    return w;
}
}  // namespace

// One run through a graph: the exec walk and the data pulls.
struct Run {
    Machine& m;
    Machine::Inst& in;
    const Graph& g;
    int steps = 0;
    // The parameter blocks of the script functions running, innermost last: each output or counter by its
    // byte offset (`.fparam` literals, Set Function Output, FLOW_SETPARAM records).
    std::vector<std::map<int64_t, Value>> frames = std::vector<std::map<int64_t, Value>>(1);
    std::map<uint32_t, int64_t> loop_index;   // ForLoop -> the iteration running

    void Unknown(const std::string& type) {
        if (std::find(m.unknown_.begin(), m.unknown_.end(), type) == m.unknown_.end()) m.unknown_.push_back(type);
    }
    const Graph::Node* NodeOf(uint32_t id) const {
        const auto it = g.nodes.find(id);
        return it == g.nodes.end() ? nullptr : &it->second;
    }
    Value Param(int64_t offset) const {
        const auto v = frames.back().find(offset);
        return v == frames.back().end() ? Value::I(0) : v->second;
    }
    // A Raw part's operator chain (the lifter's `op:` text, `op:i;fparam i 0;i 00000001;iadd #0 #1`): one
    // step per `;`, `#k` an earlier step, the last step the result.
    Value Chain(const std::string& text) {
        std::vector<Value> st;
        size_t a = text.find(';');
        auto arg = [&](const std::string& w) -> Value {
            if (w.size() > 1 && w[0] == '#') {
                const size_t k = strtoull(w.c_str() + 1, nullptr, 10);
                return k < st.size() ? st[k] : Value{};
            }
            return {};
        };
        while (a != std::string::npos) {
            const size_t b = text.find(';', a + 1);
            const std::vector<std::string> w = Words(text.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
            a = b;
            if (w.empty()) return {};
            const std::string& op = w[0];
            Value v;
            if (op == "fparam" && w.size() >= 3) v = Param(strtoll(w[2].c_str(), nullptr, 10));
            else if (op == "i" && w.size() >= 2) v = Value::I(int32_t(uint32_t(Hex(w[1]))));
            else if (op == "f" && w.size() >= 2) { const uint32_t bits = uint32_t(Hex(w[1])); float x; std::memcpy(&x, &bits, 4); v = Value::R(x); }
            else if (op == "b" && w.size() >= 2) v = Value::B(w[1] != "0");
            else if (op == "not" && w.size() >= 2) v = Value::B(!arg(w[1]).AsBool());
            else if (w.size() >= 3) {
                const Value x = arg(w[1]), y = arg(w[2]);
                if (op == "iadd") v = Value::I(x.AsInt() + y.AsInt());
                else if (op == "isub") v = Value::I(x.AsInt() - y.AsInt());
                else if (op == "imul") v = Value::I(x.AsInt() * y.AsInt());
                else if (op == "fadd") v = Value::R(x.AsReal() + y.AsReal());
                else if (op == "fsub") v = Value::R(x.AsReal() - y.AsReal());
                else if (op == "fmul") v = Value::R(x.AsReal() * y.AsReal());
                else if (op == "ieq") v = Value::B(x.AsInt() == y.AsInt());
                else if (op == "ine") v = Value::B(x.AsInt() != y.AsInt());
                else if (op == "ilt") v = Value::B(x.AsInt() < y.AsInt());
                else if (op == "igt") v = Value::B(x.AsInt() > y.AsInt());
                else if (op == "and") v = Value::B(x.AsBool() && y.AsBool());
                else if (op == "or") v = Value::B(x.AsBool() || y.AsBool());
                else { Unknown("op " + op); return {}; }
            } else {
                Unknown("op " + op);
                return {};
            }
            st.push_back(v);
        }
        return st.empty() ? Value{} : st.back();
    }
    Value Input(uint32_t id, const std::string& port, int depth = 0) {
        if (depth > 64) return {};
        if (const Graph::Node* n = NodeOf(id)) {
            // read from the running function's parameter block
            if (const auto p = n->lits.find(port + ".fparam"); p != n->lits.end()) return Param(p->second.AsInt());
            if (const auto e = n->raw_expr.find(port); e != n->raw_expr.end()) return Chain(e->second);
        }
        if (const auto d = g.data.find({id, port}); d != g.data.end()) return Eval(d->second.first, d->second.second, depth + 1);
        if (const auto r = g.reads.find({id, port}); r != g.reads.end()) {
            const auto v = in.vars.find(r->second);
            return v == in.vars.end() ? Value{} : v->second;
        }
        if (const Graph::Node* n = NodeOf(id))
            if (const auto l = n->lits.find(port); l != n->lits.end()) return l->second;
        return {};
    }
    Value Eval(uint32_t id, const std::string& port, int depth) {
        const Graph::Node* n = NodeOf(id);
        if (!n) return {};
        const std::string& t = n->type;
        auto in_ = [&](const char* p) { return Input(id, p, depth); };
        if (t == "R15NetSymbol" || t == "R15NetReal" || t == "R15NetInt" || t == "R15NetFlag") {
            const auto v = m.net_.find(in_("name").AsSym());
            if (v != m.net_.end()) return v->second;
            if (t == "R15NetSymbol") return Value::S(0);
            if (t == "R15NetReal") return Value::R(0);
            if (t == "R15NetInt") return Value::I(0);
            return Value::B(false);
        }
        if (t == "R15NetVarNameExpression") return Value::S(in.event_var);
        if (t == "EqualSymbol") return Value::B(in_("a").AsSym() == in_("b").AsSym());
        if (t == "NotEqualSymbol") return Value::B(in_("a").AsSym() != in_("b").AsSym());
        if (t == "EqualInteger") return Value::B(in_("a").AsInt() == in_("b").AsInt());
        if (t == "NotEqualInteger") return Value::B(in_("a").AsInt() != in_("b").AsInt());
        if (t == "LessInteger") return Value::B(in_("a").AsInt() < in_("b").AsInt());
        if (t == "GreaterInteger") return Value::B(in_("a").AsInt() > in_("b").AsInt());
        if (t == "AddIntegers") return Value::I(in_("a").AsInt() + in_("b").AsInt());
        if (t == "GreaterNumber") return Value::B(in_("a").AsReal() > in_("b").AsReal());
        if (t == "LessNumber") return Value::B(in_("a").AsReal() < in_("b").AsReal());
        if (t == "NumberToInteger") return Value::I(int64_t(in_("value").AsReal()));   // truncates, as the game's cast
        if (t == "IntegerToNumber") return Value::R(double(in_("value").AsInt()));
        if (t == "Not") return Value::B(!in_("value").AsBool());
        if (t == "And") return Value::B(in_("a").AsBool() && in_("b").AsBool());
        if (t == "Or") return Value::B(in_("a").AsBool() || in_("b").AsBool());
        if (t == "BoolToInteger") return Value::I(in_("value").AsBool() ? 1 : 0);
        if (t == "SelectBool" || t == "SelectNumber" || t == "SelectSymbol" || t == "SelectString")
            return in_("condition").AsBool() ? in_("if true") : in_("if false");
        if (t == "PrintSymbolExpression") return in_("input");
        if (t == "PassThrough") return in_(port.c_str());
        if (t == "inttostring") {
            const int64_t v = in_("value_in").AsInt();
            const int64_t z = std::clamp<int64_t>(in_("numleadingzeros").AsInt(), 0, 18);
            char b[48];
            snprintf(b, sizeof b, "%0*lld", int(z), (long long)v);
            return Value::T(b);
        }
        if (t == "secondstotime") {
            const double s = std::max(0.0, in_("value_in").AsReal());
            const bool mins = in_("showminutes").AsBool(), secs = in_("showseconds").AsBool();
            const bool tenths = in_("showtenths").AsBool(), hundredths = in_("showhundredths").AsBool();
            const int64_t cs = int64_t(std::floor(s * 100.0 + 1e-6));
            std::string out;
            char b[16];
            if (mins) snprintf(b, sizeof b, "%02lld", (long long)(cs / 6000)), out += b;
            if (secs) snprintf(b, sizeof b, "%s%02lld", out.empty() ? "" : ":", (long long)(mins ? (cs / 100) % 60 : cs / 100)), out += b;
            if (hundredths) snprintf(b, sizeof b, "%s%02lld", out.empty() ? "" : ":", (long long)(cs % 100)), out += b;
            else if (tenths) snprintf(b, sizeof b, "%s%lld", out.empty() ? "" : ".", (long long)((cs % 100) / 10)), out += b;
            return Value::T(out);
        }
        if (t == "R15NetGameExpression" || t == "R15NetGameTypeExpression" || t == "R15NetMatchExpression") return Value::B(false);
        if (t == "ForLoop") {
            const auto i = loop_index.find(id);
            return Value::I(i == loop_index.end() ? 0 : i->second);
        }
        // the lobby, from the recording (Machine::SetPlayers): a user id is its index there, -1 none
        if (t == "R15NetTeamSlotToUserIdExpression") {
            const int64_t team = in_("teamid").AsInt(), slot = in_("teamslot").AsInt();
            for (size_t k = 0; k < m.players_.size(); ++k)
                if (m.players_[k].team == team && m.players_[k].slot == slot) return Value::I(int64_t(k));
            return Value::I(-1);
        }
        if (t == "R15NetUserExpression" || t == "R15NetPlayerMatchStatExpression") {
            const int64_t user = in_("userid").AsInt();
            const Player* p = user >= 0 && size_t(user) < m.players_.size() ? &m.players_[size_t(user)] : nullptr;
            if (t == "R15NetPlayerMatchStatExpression") {
                if (!p) return Value::I(0);
                const auto v = p->stats.find(in_("statname").AsSym());
                return Value::I(v == p->stats.end() ? 0 : v->second);
            }
            if (port == "valid") return Value::B(p != nullptr);
            if (port == "username") return Value::T(p ? p->name : "");
            if (port == "ping") return Value::I(p ? p->ping : 0);
            return {};
        }
        if (t == "R15NetUserIdToActorExpression") return Value::S(0);   // the players' bodies are not script actors here
        if (t == "R15NetEarlyQuitPenaltyExpression") return Value::I(0);
        if (t == "R15NetEarlyQuitFeatureFlagExpression") return Value::B(false);
        if (t == "Raw") return {};   // ACTORVALID: no actors to test
        Unknown(t);
        return {};
    }

    uint64_t ActorOf(const Value& v) const {
        const uint64_t a = v.AsSym();
        return a == 0 || a == kUnset ? in.self : a;
    }
    void Write(uint64_t actor, uint64_t component, uint64_t item, const std::function<void(ItemState&)>& f) {
        ItemState& s = m.items_[{actor, component == kUnset ? 0 : component, item}];
        const ItemState before = s;
        f(s);
        if (before.text != s.text || before.shown != s.shown || before.alpha != s.alpha) m.dirty_ = true;
    }
    void Show(const Machine::Inst::Bound& x, const Value& v) {
        if (x.type == 0) Write(x.actor, x.component, x.item, [&](ItemState& s) { s.text = v.AsStr(); });
        else if (x.type == 3) Write(x.actor, x.component, x.item, [&](ItemState& s) { s.shown = v.AsBool(); });
    }
    void SetVar(const std::string& name, const Value& v) {
        in.vars[name] = v;
        if (const auto b = in.bindings.find(name); b != in.bindings.end())
            for (const auto& x : b->second) Show(x, v);
    }

    // A part the lifter carries as Raw, from its record: returns the exit taken, -1 to stop.
    int ExecRaw(uint32_t id, const Graph::Node& n, int depth) {
        const std::string& c = n.raw_class;
        if (c == "FLOW_SETPARAM") {
            const auto off = n.raw_ports.find("param.offset");
            if (off == n.raw_ports.end()) return -1;
            const auto size = n.raw_ports.find("param.size");
            const auto value = n.raw_ports.find("param.value");
            Value v;
            if (value != n.raw_ports.end()) {
                const uint64_t bits = value->second;
                const uint64_t bytes = size == n.raw_ports.end() ? 4 : size->second;
                v = bytes == 1 ? Value::I(int8_t(bits)) : bytes == 4 ? Value::I(int32_t(uint32_t(bits))) : Value::I(int64_t(bits));
            } else {
                v = Input(id, "value");
            }
            frames.back()[int64_t(off->second)] = v;
            return 0;
        }
        if (c == "UNEMITTED_SPAWN") {
            // starts pin 0 on its own thread (once while one runs; here it runs to its end at once), ends this one
            if (const auto body = g.links.find({id, 0}); body != g.links.end())
                for (const uint32_t b : body->second) Exec(b, depth + 1);
            return 1;
        }
        if (c == "FLOW_BRANCH") {
            // A condition the operator lift could not read is taken as false. The stock case (the match board,
            // 57a56db2) inlines an actor-valid test of its collision actor, which the viewer does not simulate;
            // both its exits go on to the board's bindings.
            const Value cond = Input(id, "condition");
            const auto neg = n.raw_ports.find("negate");
            return cond.AsBool() != (neg != n.raw_ports.end() && neg->second != 0) ? 0 : 1;
        }
        Unknown("Raw " + c);
        return -1;
    }

    void Exec(uint32_t id, int depth = 0) {
        while (id != 0) {
            if (++steps > 20000 || depth > 48) return;
            const Graph::Node* n = NodeOf(id);
            if (!n) return;
            const std::string& t = n->type;
            int pin = 0;
            if (t == "OnStart" || t == "OnInitialize" || t == "OnEvent" || t == "FunctionEntry" || t == "End" || t == "DelayNode" ||
                t == "PassThrough" || t == "DebugPrintStringNode") {
            } else if (t == "While") {
                // The body (pin 0) runs on its own thread, once per tick while the condition holds; then pin 1.
                const bool on = Input(id, "condition").AsBool();
                const auto at = std::find(in.loops.begin(), in.loops.end(), id);
                if (on) {
                    if (at == in.loops.end()) in.loops.push_back(id);
                    if (const auto body = g.links.find({id, 0}); body != g.links.end())
                        for (const uint32_t b : body->second) Exec(b, depth + 1);
                    return;
                }
                if (at != in.loops.end()) in.loops.erase(at);
                pin = 1;
            } else if (t == "SendEvent") {
                // to an actor's scripts (the sender's own when the pin is unset)
                Value a = Input(id, "actor");
                if (a.kind == Value::None) a = Input(id, "actorname");
                m.Send(Input(id, "event").AsSym(), ActorOf(a), depth + 1);
            } else if (t == "SendEventNode") {
                // a named event to every script listening for it
                m.Send(Input(id, "name").AsSym(), 0, depth + 1);
            } else if (t == "SetVariable") {
                SetVar(n->nvar, Input(id, "value"));
            } else if (t.rfind("SetFunctionOutput", 0) == 0) {
                if (const auto o = n->lits.find("output.fparam"); o != n->lits.end()) frames.back()[o->second.AsInt()] = Input(id, "value");
            } else if (t == "CallFunction") {
                // a fresh parameter block for the call (these functions take no arguments)
                if (const auto f = g.funcs.find(n->nvar); f != g.funcs.end()) {
                    frames.emplace_back();
                    Exec(f->second, depth + 1);
                    frames.pop_back();
                }
            } else if (t == "ForLoop") {
                // the body (pin 0) once per count, then pin 1
                const int64_t count = std::clamp<int64_t>(Input(id, "count").AsInt(), 0, 256);
                const auto body = g.links.find({id, 0});
                for (int64_t k = 0; k < count && body != g.links.end(); ++k) {
                    loop_index[id] = k;
                    for (const uint32_t b : body->second) Exec(b, depth + 1);
                }
                pin = 1;
            } else if (t == "Raw") {
                pin = ExecRaw(id, *n, depth);
                if (pin < 0) return;
            } else if (t == "R15NetGameTypeNode") {
                pin = 0;   // arena
            } else if (t == "R15OverrideToDecalNode" || t == "EnableCollisionNode") {
                // the players' bodies and the board's collision are not shown here
            } else if (t == "Branch") {
                pin = Input(id, "condition").AsBool() ? 0 : 1;
            } else if (t == "Switch") {
                const uint64_t v = Input(id, "value").AsSym();
                uint64_t want = Input(id, "default").AsSym();
                for (int k = 0; k < 64; ++k) {
                    const auto key = n->lits.find("case." + std::to_string(k) + ".key");
                    if (key == n->lits.end()) break;
                    if (key->second.AsSym() == v) {
                        want = Input(id, "case." + std::to_string(k) + ".pin").AsSym();
                        break;
                    }
                }
                pin = -1;
                for (const auto& [idx, h] : n->exits)
                    if (h == want) pin = idx;
                if (pin < 0) return;
            } else if (t == "R15NetIsDedicatedNode") {
                pin = g.links.count({id, 1}) ? 1 : 0;   // a viewer is a client
            } else if (t == "SetText2Node") {
                const uint64_t actor = ActorOf(Input(id, "actorname"));
                const uint64_t comp = Input(id, "componentname").AsSym(), item = Input(id, "uiitemname").AsSym();
                const std::string text = Input(id, "text").AsStr();
                Write(actor, comp, item, [&](ItemState& s) { s.text = text; });
            } else if (t == "ShowUINode") {
                Value a = Input(id, "actorname");
                if (a.kind == Value::None) a = Input(id, "actor");
                const bool show = Input(id, "show").AsBool();
                Write(ActorOf(a), Input(id, "componentname").AsSym(), Input(id, "itemname").AsSym(), [&](ItemState& s) { s.shown = show; });
            } else if (t == "SetAlphaUINode") {
                Value a = Input(id, "actorname");
                if (a.kind == Value::None) a = Input(id, "actor");
                const float alpha = float(Input(id, "alpha").AsReal());
                Write(ActorOf(a), Input(id, "componentname").AsSym(), Input(id, "itemname").AsSym(), [&](ItemState& s) { s.alpha = alpha; });
            } else if (t == "SetColorUINode") {
            } else if (t == "CreateUIBindingsNode") {
                const uint64_t actor = ActorOf(Input(id, "actor"));
                const uint64_t comp = Input(id, "componentname").AsSym();
                const int64_t used = Input(id, "bindings.used").AsInt();
                for (int64_t k = 0; k < used && k < 32; ++k) {
                    const std::string p = "bindings." + std::to_string(k) + ".";
                    const uint64_t item = Input(id, p + "uiitemname").AsSym(), var = Input(id, p + "variablename").AsSym();
                    for (const auto& [name, v] : g.vars) {
                        if (v.hash != var) continue;
                        const Machine::Inst::Bound x{actor, comp, item, Input(id, p + "type").AsInt()};
                        in.bindings[name].push_back(x);
                        if (const auto cur = in.vars.find(name); cur != in.vars.end()) Show(x, cur->second);
                    }
                }
            } else {
                Unknown(t);
                return;
            }
            const auto next = g.links.find({id, pin});
            if (next == g.links.end() || next->second.empty()) return;
            for (size_t k = 1; k < next->second.size(); ++k) Exec(next->second[k], depth + 1);
            id = next->second[0];
        }
    }
};

Machine::Machine() = default;
Machine::~Machine() = default;

void Machine::SendEvent(uint64_t event) {
    dirty_ = false;
    Send(event, 0, 0);
    if (dirty_) ++generation_;
}

void Machine::Clear() {
    inst_.clear();
    players_.clear();
    net_.clear();
    started_ = false;
    items_.clear();
    unknown_.clear();
    ++generation_;
}

void Machine::Add(std::shared_ptr<const Graph> graph, uint64_t self, const Params& params) {
    if (!graph) return;
    auto in = std::make_unique<Inst>();
    in->g = std::move(graph);
    in->self = self;
    for (const auto& [name, v] : in->g->vars) {
        const auto p = params.find(v.hash);
        in->vars[name] = p != params.end() ? p->second : v.def;
    }
    inst_.push_back(std::move(in));
}

void Machine::Send(uint64_t event, uint64_t actor, int depth) {
    if (event == 0 || event == kUnset || event == kNetVarChanged || depth > 32) return;
    for (auto& in : inst_) {
        if (actor != 0 && in->self != actor) continue;
        Run r{*this, *in, *in->g};
        for (const auto& [id, n] : in->g->nodes) {
            if (n.type != "OnEvent") continue;
            // one On Event listens for up to 8 events: `event`, `event.1` ..
            bool hit = false;
            for (int k = 0; k < 8 && !hit; ++k) {
                const auto e = n.lits.find(k == 0 ? std::string("event") : "event." + std::to_string(k));
                hit = e != n.lits.end() && e->second.AsSym() == event;
            }
            if (hit) r.Exec(id, depth);
        }
    }
}

bool Machine::Tick(const std::map<uint64_t, Value>& net) {
    dirty_ = false;
    std::vector<uint64_t> changed;
    for (const auto& [k, v] : net) {
        const auto old = net_.find(k);
        if (old == net_.end() || !(old->second == v)) changed.push_back(k);
    }
    for (const auto& [k, v] : net_)
        if (!net.count(k)) changed.push_back(k);
    net_ = net;
    // the While loops running since an earlier tick step their bodies again (or end, taking their exit)
    for (auto& in : inst_) {
        const std::vector<uint32_t> loops = in->loops;
        Run r{*this, *in, *in->g};
        for (const uint32_t id : loops) r.Exec(id);
    }
    auto entries = [&](const char* type) {
        for (auto& in : inst_) {
            Run r{*this, *in, *in->g};
            for (const auto& [id, n] : in->g->nodes)
                if (n.type == type) r.Exec(id);
        }
    };
    if (!started_) {
        started_ = true;
        entries("OnInitialize");
        entries("OnStart");
    }
    // game_state first: the scripts arm their timers on it
    std::stable_sort(changed.begin(), changed.end(), [](uint64_t a, uint64_t b) { return (a == kGameState) > (b == kGameState); });
    for (const uint64_t var : changed) {
        for (auto& in : inst_) {
            in->event_var = var;
            Run r{*this, *in, *in->g};
            for (const auto& [id, n] : in->g->nodes) {
                if (n.type != "OnEvent") continue;
                const auto e = n.lits.find("event");
                if (e == n.lits.end() || e->second.AsSym() != kNetVarChanged) continue;
                const Value ok = r.Input(id, "shouldexecute");
                if (ok.kind != Value::None && !ok.AsBool()) continue;
                r.Exec(id);
            }
        }
    }
    if (dirty_) ++generation_;
    return dirty_;
}

}  // namespace erv::script
