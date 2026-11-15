#include "avatar.h"

#include <algorithm>
#include <cstring>
#include <execution>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>

#include <windows.h>

#include <echo/codecs/material_resource.hpp>
#include <echo/codecs/meshlist_resource.hpp>
#include <echo/codecs/scene_resource.hpp>
#include <echo/codecs/texture_resource.hpp>
#include <meshoptimizer.h>
#include <echo/importers/resource_closure.hpp>
#include <ui/mesh_source.hpp>

#include "figure.h"

namespace erv {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kBoneStride = 32, kHierStride = 24;
constexpr float kDeg = 3.14159265f / 180.f;

// the material binding slots (lone_echo_blender docs/COSMETICS.md, data/material_property_names.json)
constexpr uint64_t kSlotBaseColour = 0x01bc8bf07fbfc184ull;   // shading_basecolormap
constexpr uint64_t kSlotEmissive = 0xf55db9261547732aull;     // emissive_map
constexpr uint64_t kSlotSecondMask = 0x7abbb0b3b865219bull;   // customization_secondaryblendmask
constexpr uint64_t kSlotNormal = 0x760ba3c3f12eed10ull;       // shading_normalmap
constexpr uint64_t kSlotSpec = 0x2c714411035f192eull;         // shading_specmap (red metal, green sqrt-roughness)
constexpr uint64_t kSlotHeightAo = 0x1ea5d65d02ed2414ull;     // shading_heightaomap
constexpr uint64_t kSlotTransmit = 0x0af5f316d1716b6aull;     // shading_transmittancemap (the visor)
constexpr float kVisorOpacity = 0.3f;   // the visor read as near-opaque white; drawn at 30% (70% more see-through)
constexpr uint64_t kSlotStun = 0xc2507357d3f1ca6eull;         // emissive_hitfx_stun_map
// material properties (lone_echo_blender data/material_property_names.json)
constexpr uint64_t kPropEmissiveIntensity = 0xc53650165d02c64bull;   // emissive_intensity
constexpr uint64_t kPropTransmitScale = 0x9b76fe894fa0d867ull;       // shading.transmittancescale
constexpr uint64_t kPropMetallicOffset = 0x7414ce071e5c5cb2ull;      // shading.metallicoffset
constexpr uint64_t kPropStunUScale = 0x681878e6112f9ad9ull;          // emissive.hitfx.stun.uvxform.uscale
constexpr uint64_t kPropStunVScale = 0x68187be6112f9ad9ull;          // emissive.hitfx.stun.uvxform.vscale

const std::unordered_map<uint64_t, const char*>& BoneNames() {
    static const std::unordered_map<uint64_t, const char*> table = [] {
        struct Row {
            uint64_t h;
            const char* n;
        };
        static const Row rows[] = {
#include "bone_names.inc"
        };
        std::unordered_map<uint64_t, const char*> m;
        for (const Row& r : rows) m.emplace(r.h, r.n);
        return m;
    }();
    return table;
}

template <class T>
T At(const std::vector<uint8_t>& b, size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

bool ReadFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

std::string Hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

// ---- CSkeletonResourceWin10 (lone_echo_blender scripts/evr_apply_skeleton.py) ----------------------------------

struct Table {
    size_t header, size, count, stride;
};

std::vector<Table> Tables(const std::vector<uint8_t>& b) {
    std::vector<Table> out;
    for (size_t c = 0; c < 0x600 && c + 0x38 <= b.size(); c += 8) {
        const uint64_t size = At<uint64_t>(b, c + 8), mark = At<uint64_t>(b, c + 0x20), total = At<uint64_t>(b, c + 0x28),
                       used = At<uint64_t>(b, c + 0x30);
        if (mark != 32 || !used || total != used || used > 100000 || !size || size % used) continue;
        const uint64_t stride = size / used;
        static const uint64_t ok[] = {4, 8, 12, 16, 24, 32, 48, 64, 96, 120, 128};
        if (std::find(std::begin(ok), std::end(ok), stride) != std::end(ok)) out.push_back({c, size_t(size), size_t(used), size_t(stride)});
    }
    return out;
}

// Walking first_child / next_sibling must rebuild the parent column exactly.
bool HierarchyAt(const std::vector<uint8_t>& b, size_t base, size_t n, std::vector<uint32_t>& parent) {
    if (base + n * kHierStride > b.size()) return false;
    std::vector<uint32_t> par(n), child(n), sib(n);
    bool root = false;
    for (size_t i = 0; i < n; ++i) {
        const size_t o = base + i * kHierStride;
        par[i] = At<uint32_t>(b, o + 0x0c), child[i] = At<uint32_t>(b, o + 0x10), sib[i] = At<uint32_t>(b, o + 0x14);
        for (uint32_t v : {par[i], child[i], sib[i]})
            if (v != kNone && v >= n) return false;
        root |= par[i] == kNone;
    }
    if (!root) return false;
    std::vector<uint32_t> derived(n, kNone);
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = child[i];
        size_t guard = 0;
        while (c != kNone) {
            if (derived[c] != kNone || ++guard > n) return false;
            derived[c] = uint32_t(i);
            c = sib[c];
        }
    }
    if (derived != par) return false;
    parent = par;
    return true;
}

bool CleanBind(const std::vector<uint8_t>& b, size_t off, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const size_t o = off + i * kBoneStride;
        if (o + kBoneStride > b.size()) return false;
        const float x = At<float>(b, o), y = At<float>(b, o + 4), z = At<float>(b, o + 8), w = At<float>(b, o + 12);
        const float l = sqrtf(x * x + y * y + z * z + w * w);
        if (!(fabsf(l - 1.f) <= 1e-3f)) return false;
        if (!(fabsf(At<float>(b, o + 28) - 1.f) <= 1e-3f)) return false;
    }
    return true;
}

// ---- textures: the whole chain from `max_dim` down, streamed levels read from their packfiles ---------------------

bool LoadTexture(echo::ui::CorpusTextureProvider& tp, uint64_t id, int max_dim, echo::render::ActorTexture& out, std::string& note) {
    echo::render::TextureBlobs blobs;
    if (!tp.fetch(echo::Symbol64{id}, blobs)) {
        note = "not in the game files";
        return false;
    }
    echo::codecs::TextureReader reader;
    std::string err;
    if (!reader.decode(blobs.resource, blobs.sidecar, err)) {
        note = "does not decode: " + err;
        return false;
    }
    const echo::codecs::TextureResource& res = reader.resource();
    if (res.is_volume || res.array_size > 1 || res.total_mips == 0 || res.res_mips == 0 || res.res_mips > res.total_mips) {
        note = "not a single 2-D texture";
        return false;
    }
    const uint32_t total = res.total_mips, first_resident = total - res.res_mips;
    auto level_bytes = [&](uint32_t l, std::vector<uint8_t>& texels) {
        const uint32_t w = std::max(1u, res.full_width >> l), h = std::max(1u, res.full_height >> l);
        const uint64_t want = echo::codecs::dxgi_image_bytes(res.dxgi_format, w, h);
        if (want == 0) return false;
        if (res.level_is_streamed(l)) {
            uint64_t asset = 0;
            uint32_t bytes = 0;
            std::vector<uint8_t> blocks;
            if (!res.streamed_asset_for_level(l, asset, bytes) || !tp.fetch_packfile(echo::Symbol64{asset}, blocks) || blocks.size() != want)
                return false;
            texels.insert(texels.end(), blocks.begin(), blocks.end());
            return true;
        }
        const uint32_t i = l - first_resident;
        if (l < first_resident || i >= res.images.size() || res.images[i].width != w || res.images[i].height != h || res.images[i].bytes != want)
            return false;
        const uint8_t* d = res.image_data(i);
        if (!d) return false;
        texels.insert(texels.end(), d, d + want);
        return true;
    };
    uint32_t first = 0;
    while (first + 1 < total && int(std::max(res.full_width, res.full_height) >> first) > max_dim) ++first;
    for (uint32_t start : {first, std::max(first, first_resident)}) {   // the streamed chain, else what ships resident
        std::vector<uint8_t> texels;
        bool ok = true;
        for (uint32_t l = start; l < total && ok; ++l) ok = level_bytes(l, texels);
        if (!ok) continue;
        out.dxgi_format = res.dxgi_format;
        out.width = std::max(1u, res.full_width >> start);
        out.height = std::max(1u, res.full_height >> start);
        out.mips = total - start;
        out.texels = std::move(texels);
        note = std::to_string(out.width) + "x" + std::to_string(out.height) + " fmt " + std::to_string(out.dxgi_format) + ", " +
               std::to_string(out.mips) + " mips" + (start < first_resident ? " (streamed)" : "");
        return true;
    }
    note = "its levels could not be read";
    return false;
}


// ---- demo-viewer's rig (IKController's numbers, in Unity's space) -------------------------------------------------

struct RigNode {
    const char* name;
    int parent;
    V3 p;
    Q4 q;
    float s;
};
const RigNode kRig[] = {
#include "demo_rig.inc"
};
constexpr int kRigN = int(sizeof kRig / sizeof kRig[0]);

int RigIndex(const char* name) {
    for (int i = 0; i < kRigN; ++i)
        if (strcmp(kRig[i].name, name) == 0) return i;
    return -1;
}

V3 U(V3 v) { return {v.z, v.y, v.x}; }              // API <-> Unity: demo-viewer's ToVector3 swaps x and z
Q4 UQ(Q4 q) { return {-q.z, -q.y, -q.x, q.w}; }    // the same rotation seen across that mirror

Q4 LookRotation(V3 f, V3 u) {   // Unity's: z along f, y toward u
    f = Norm(f);
    const V3 x = Norm(Cross(u, f));
    return FromBasis(x, Cross(f, x), f);
}
Q4 Euler(float x, float y, float z) {   // Unity's Quaternion.Euler: z, then x, then y (degrees)
    return QNorm(Mul(Mul(AxisAngle({0, 1, 0}, y * kDeg), AxisAngle({1, 0, 0}, x * kDeg)), AxisAngle({0, 0, 1}, z * kDeg)));
}

struct RigPose {
    V3 p[kRigN];
    Q4 q[kRigN];
};

struct RigIds {
    int neck = RigIndex("Neck"), spine2 = RigIndex("Spine2"), spine1 = RigIndex("Spine1"), spine = RigIndex("Spine"),
        hips = RigIndex("Hips");
    int up[2]{RigIndex("LeftUpLeg"), RigIndex("RightUpLeg")}, leg[2]{RigIndex("LeftLeg"), RigIndex("RightLeg")},
        foot[2]{RigIndex("LeftFoot"), RigIndex("RightFoot")}, hint[2]{RigIndex("LeftHint"), RigIndex("RightHint")};
};
const RigIds& Ids() {
    static const RigIds ids;
    return ids;
}

// IKController.Update (the branch every frame takes): the player at the head, the neck and the body target turned to
// the body's frame and pitched by X0, the spine joints below by their X / Y and the rest curl, the thighs and calves.
// `neutral` is the same with no lean and straight legs. head_u, bf, bu are in Unity's space.
void SolveRig(V3 head_u, V3 bf, V3 bu, const DemoLean& L, bool neutral, RigPose& o) {
    const RigIds& id = Ids();
    float x[4], y[4];
    for (int k = 0; k < 4; ++k) x[k] = neutral ? 0 : L.x[k], y[k] = neutral ? 0 : L.y[k];
    const float r = neutral ? 0 : L.rest, lg = neutral ? 0 : L.legs;
    float scale[kRigN];
    for (int i = 0; i < kRigN; ++i) {
        const RigNode& n = kRig[i];
        Q4 lq = n.q;
        if (i == id.spine1) lq = Euler(x[1] - 10 * r, 0, -y[1]);
        else if (i == id.spine) lq = Euler(x[2] - 10 * r, 0, -y[2]);
        else if (i == id.hips) lq = Euler(-x[3] + 10 * r, 180, -y[3]);
        else if (i == id.up[0]) lq = Euler(32 * lg, -5 * lg, -10 * lg);
        else if (i == id.up[1]) lq = Euler(32 * lg, 5 * lg, 10 * lg);
        else if (i == id.leg[0] || i == id.leg[1]) lq = Euler(-110 * lg, 0, 0);
        const V3 pp = n.parent < 0 ? head_u : o.p[n.parent];
        const Q4 pq = n.parent < 0 ? Q4{} : o.q[n.parent];
        const float ps = n.parent < 0 ? 1.f : scale[n.parent];
        o.p[i] = pp + Rotate(pq, n.p * ps);
        scale[i] = ps * n.s;
        if (i == id.neck) o.q[i] = Mul(LookRotation(bf, bu), Euler(x[0], 0, 0));
        else if (i == id.spine2) o.q[i] = Mul(LookRotation(bf * -1.f, bu * -1.f), Euler(x[0], 0, 0));
        else o.q[i] = QNorm(Mul(pq, lq));
    }
}

// Which of the rig's sides is the API's left (+X of the body frame): the rig was imported mirrored, then mirrored again.
bool RigLeftIsLeft() {
    static const bool v = [] {
        RigPose o;
        SolveRig({0, 0, 0}, U({0, 0, 1}), {0, 1, 0}, DemoLean{}, true, o);
        const V3 d = U(o.p[Ids().up[0]] - o.p[Ids().hips]);
        return d.x > 0;
    }();
    return v;
}

V3 BodyForward(const Q4& head_q, int forward_axis) {   // demo-viewer's bodyForward: the head's heading, level
    V3 f, u, r;
    HeadAxes(head_q, forward_axis, f, u, r);
    V3 bf{f.x, 0, f.z};
    if (Len(bf) < 0.2f) bf = V3{u.x, 0, u.z} * (f.y > 0 ? -1.f : 1.f);   // looking straight up or down
    return Len(bf) > 1e-4f ? Norm(bf) : V3{0, 0, 1};
}

// IKController's targets for one frame (in Unity's space, as it computes them).
void LeanTargets(V3 bf_api, V3 vel_api, float x[4], float y[4], float& rest) {
    const V3 bf = U(bf_api), bu{0, 1, 0}, vel = U(vel_api);
    const float tf = Dot(bf, vel) * 20.f;
    const float ts = Dot(Cross(Norm(bf), bu), vel) * 20.f;
    if (tf > 0) {
        const float off[4] = {0, 20, 40, 40};
        for (int k = 0; k < 4; ++k) x[k] = std::clamp(tf - off[k], 0.f, 20.f);
    } else {
        const float off[4] = {0, 15, 30, 45};
        for (int k = 0; k < 4; ++k) x[k] = std::clamp(tf + off[k], -15.f, 0.f);
    }
    const float off[4] = {0, 0, 20, 40};
    for (int k = 0; k < 4; ++k) {
        const float lim = k == 3 ? 30.f : 20.f;
        y[k] = ts > 0 ? std::clamp(ts - off[k], 0.f, lim) : std::clamp(ts + off[k], -lim, 0.f);
    }
    rest = (5.f - std::min(Len(vel), 5.f)) / 5.f;
}

}  // namespace

int Avatar::Find(const char* name) const {
    for (size_t i = 0; i < bones_.size(); ++i)
        if (bones_[i].label == name) return int(i);
    return -1;
}

bool Avatar::Load(const std::filesystem::path& corpus, const std::string& model) {
    why_.clear();
    notes_.clear();
    std::ostringstream log;
    auto res = echo::importers::CorpusResources::shared(corpus);
    if (!res.has_value()) {
        why_ = "the game files cannot be read";
        return false;
    }
    const auto& cr = *res.value();
    const uint64_t name = static_cast<uint64_t>(echo::rad_hash(model));
    auto read = [&](const char* type, std::vector<uint8_t>& out) {
        const std::filesystem::path* p = cr.find(static_cast<uint64_t>(echo::rad_hash(type)), name);
        return p && ReadFile(*p, out);
    };
    std::vector<uint8_t> skel, header, gpu;
    if (!read("CSkeletonResourceWin10", skel)) {
        why_ = model + ": no skeleton in the game files";
        return false;
    }
    // the mesh: the skeleton's own name, else its rift / LOD variants
    {
        std::string found;
        for (const std::string suffix : {"", "_rift", "_rift_lod_1", "_lod_1", "_rift_lod_2", "_lod_0", "_rift_lod_0"}) {
            const std::string mesh = model + suffix;
            const uint64_t mh = static_cast<uint64_t>(echo::rad_hash(mesh));
            const std::filesystem::path* hp = cr.find(static_cast<uint64_t>(echo::rad_hash("CGMeshListResourceWin10")), mh);
            const std::filesystem::path* gp = cr.find(0xE642BFB1ABCF76DFull, mh);
            if (found.empty() && hp && gp && ReadFile(*hp, header) && ReadFile(*gp, gpu)) found = mesh;
        }
        if (found.empty()) {
            why_ = model + ": no mesh in the game files";
            return false;
        }
        log << "using mesh " << found << "\n";
    }

    // ---- skeleton
    const std::vector<Table> tables = Tables(skel);
    size_t count = 0, bind_off = 0, hier_off = 0;
    std::vector<uint32_t> parent;
    {
        size_t bi = SIZE_MAX, hi = SIZE_MAX;
        for (size_t i = 0; i < tables.size(); ++i)
            if (tables[i].stride == kBoneStride) {
                bi = i;
                break;
            }
        if (bi != SIZE_MAX) {
            count = tables[bi].count;
            for (size_t i = 0; i < tables.size(); ++i)
                if (tables[i].stride == kHierStride && tables[i].count == count) {
                    hi = i;
                    break;
                }
        }
        if (hi == SIZE_MAX) {
            why_ = model + ": skeleton tables not found (" + std::to_string(tables.size()) + " tables)";
            return false;
        }
        bool found = false;
        for (size_t base = 0; base + count * kHierStride <= skel.size(); base += 4)
            if (HierarchyAt(skel, base, count, parent)) {
                hier_off = base, found = true;
                break;
            }
        if (!found) {
            why_ = model + ": skeleton hierarchy not found";
            return false;
        }
        size_t sizes = 0;
        for (size_t k = bi; k < hi; ++k) sizes += tables[k].size;
        if (hier_off < sizes) {
            why_ = model + ": skeleton bind pose out of range";
            return false;
        }
        bind_off = hier_off - sizes;
        bool clean = false;
        for (long long d : {0LL, 4LL, -4LL, 8LL, -8LL}) {
            const long long o = (long long)bind_off + d;
            if (o >= 0 && CleanBind(skel, size_t(o), count)) {
                bind_off = size_t(o), clean = true;
                break;
            }
        }
        log << "skeleton: " << count << " bones, bind @0x" << std::hex << bind_off << " hierarchy @0x" << hier_off << std::dec
            << (clean ? "" : " (bind window not clean)") << "\n";
    }
    std::vector<Bone> bones(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t o = bind_off + i * kBoneStride;
        Bone& b = bones[i];
        b.lq = QNorm({At<float>(skel, o), At<float>(skel, o + 4), At<float>(skel, o + 8), At<float>(skel, o + 12)});
        b.lt = {At<float>(skel, o + 16), At<float>(skel, o + 20), At<float>(skel, o + 24)};
        b.parent = parent[i] == kNone ? -1 : int(parent[i]);
        b.name = At<uint64_t>(skel, hier_off + i * kHierStride + 4);
        auto it = BoneNames().find(b.name);
        b.label = it != BoneNames().end() ? it->second : "bone_" + std::to_string(i);
    }
    // model space, parents first (resolve recursively with a guard)
    std::vector<int> done(count, 0);
    for (size_t i = 0; i < count; ++i) {
        std::vector<int> chain;
        for (int c = int(i); c >= 0 && !done[c] && chain.size() <= count; c = bones[c].parent) chain.push_back(c);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            Bone& b = bones[*it];
            if (b.parent >= 0) {
                const Bone& p = bones[b.parent];
                b.wq = QNorm(Mul(p.wq, b.lq));
                b.wt = p.wt + Rotate(p.wq, b.lt);
            } else {
                b.wq = b.lq, b.wt = b.lt;
            }
            done[*it] = 1;
        }
    }

    // ---- the palette: the same-named scene resource's materials (draw.material_index indexes it). The lower LODs'
    //      materials are named "<mesh>_lod_N"; a mesh row drawing one of them starts the next LOD.
    std::vector<uint64_t> palette;
    {
        std::vector<uint8_t> scene;
        if (read("CGSceneResourceWin10", scene)) {
            auto sr = echo::codecs::decode_scene_resource(scene);
            if (sr.has_value()) palette = sr.value().symbols_2832;
        }
        log << "palette: " << palette.size() << " materials\n";
    }
    std::set<uint64_t> lod_symbols;
    for (const std::string& stem : {model, model + "_rift"})
        for (int k = 1; k <= 8; ++k) lod_symbols.insert(static_cast<uint64_t>(echo::rad_hash(stem + "_lod_" + std::to_string(k))));

    // ---- mesh: LOD 0's rows
    echo::codecs::MeshListReader reader;
    std::string err;
    if (!reader.decode(header, gpu, err)) {
        why_ = model + ": mesh does not decode: " + err;
        return false;
    }
    const auto& rs = reader.resource();
    std::vector<Vert> verts;
    std::vector<uint32_t> tris;
    std::map<uint32_t, uint32_t> local_of;   // palette index -> materials_ index
    std::vector<Material> materials;
    std::unordered_map<uint64_t, uint32_t> split;
    size_t rows_kept = 0;
    for (uint32_t row = 0; row < rs.meshes.size(); ++row) {
        bool lower_lod = false;
        for (uint32_t d : rs.distinct_draws(row)) {
            const uint32_t mi = rs.draws[d].material_index;
            if (mi < palette.size() && lod_symbols.count(palette[mi])) lower_lod = true;
        }
        if (lower_lod) {
            log << "row " << row << " draws a lower LOD's material: rows " << row << ".." << rs.meshes.size() - 1 << " left out\n";
            break;
        }
        const std::vector<float> pos = reader.positions(row), nrm = reader.normals(row), uv = reader.texcoords(row, 0),
                                 tan = reader.tangents(row);
        const std::vector<uint8_t> bi = reader.blend_indices(row);
        const std::vector<float> bw = reader.blend_weights(row);
        const std::vector<uint32_t> idx = reader.indices(row);
        const size_t n = pos.size() / 3;
        if (n == 0 || bi.size() < n * 4 || bw.size() < n * 4) continue;
        ++rows_kept;
        const uint32_t base = uint32_t(verts.size());
        for (size_t v = 0; v < n; ++v) {
            Vert x{};
            x.material = kNone;
            x.p = {pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]};
            x.n = nrm.size() >= (v + 1) * 3 ? V3{nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]} : V3{0, 1, 0};
            if (tan.size() >= (v + 1) * 4) {
                x.t = {tan[v * 4], tan[v * 4 + 1], tan[v * 4 + 2]};
                x.tw = tan[v * 4 + 3] < 0 ? -1.f : 1.f;
                if (Len(x.t) < 1e-4f) x.tw = 0;
            }
            if (uv.size() >= (v + 1) * 2) x.u = uv[v * 2], x.v = uv[v * 2 + 1];
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                x.bi[k] = bi[v * 4 + k] < count ? bi[v * 4 + k] : 0;
                x.bw[k] = bw[v * 4 + k];
                sum += x.bw[k];
            }
            if (sum > 1e-4f)
                for (float& w : x.bw) w /= sum;
            else
                x.bw[0] = 1;
            verts.push_back(x);
        }
        for (uint32_t d : rs.distinct_draws(row)) {
            const auto& dr = rs.draws[d];
            if (dr.prim_type != 4) continue;
            auto [lit, fresh] = local_of.emplace(dr.material_index, uint32_t(materials.size()));
            if (fresh) {
                Material m;
                m.symbol = dr.material_index < palette.size() ? palette[dr.material_index] : 0;
                materials.push_back(m);
            }
            const uint32_t local = lit->second;
            for (uint32_t i = dr.first_index; i + 2 < dr.first_index + dr.index_count && i + 2 < idx.size(); i += 3) {
                const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
                if (a >= n || b >= n || c >= n) continue;
                // a vertex shared by two materials is split, so each triangle keeps its own material
                auto take = [&](uint32_t at_row) {
                    const uint64_t key = (uint64_t(base + at_row) << 8) | (local & 0xFF);
                    auto it = split.find(key);
                    if (it != split.end()) return it->second;
                    uint32_t at = base + at_row;
                    if (verts[at].material != kNone) {
                        at = uint32_t(verts.size());
                        verts.push_back(verts[base + at_row]);
                    }
                    verts[at].material = local;
                    split.emplace(key, at);
                    return at;
                };
                tris.insert(tris.end(), {take(a), take(b), take(c)});
            }
        }
    }
    if (verts.empty() || tris.empty()) {
        why_ = model + ": the mesh has no skinned triangles";
        return false;
    }
    {   // vertices no triangle uses (the rows' unused corners) are dropped
        std::vector<uint32_t> remap(verts.size(), kNone);
        std::vector<Vert> kept;
        kept.reserve(verts.size());
        for (uint32_t& i : tris) {
            if (remap[i] == kNone) remap[i] = uint32_t(kept.size()), kept.push_back(verts[i]);
            i = remap[i];
        }
        verts = std::move(kept);
    }
    if (tri_budget > 0 && tris.size() > size_t(tri_budget) * 3) {   // optional, for slow machines
        std::vector<uint32_t> simple(tris.size());
        float serr = 0;
        const size_t before = tris.size() / 3;
        simple.resize(meshopt_simplify(simple.data(), tris.data(), tris.size(), &verts[0].p.x, verts.size(), sizeof(Vert),
                                       size_t(tri_budget) * 3, 0.05f, 0, &serr));
        std::vector<uint32_t> remap(verts.size(), kNone);
        std::vector<Vert> kept;
        for (uint32_t& i : simple) {
            if (remap[i] == kNone) remap[i] = uint32_t(kept.size()), kept.push_back(verts[i]);
            i = remap[i];
        }
        log << "simplified " << before << " -> " << simple.size() / 3 << " tris\n";
        tris = std::move(simple);
        verts = std::move(kept);
    }
    log << "mesh: " << rows_kept << " rows, " << verts.size() << " verts, " << tris.size() / 3 << " tris\n";

    // ---- each material's textures
    {
        echo::ui::CorpusTextureProvider tp(corpus);
        std::map<uint64_t, int> loaded;   // texture id -> textures_ index (-1 failed)
        textures_.clear();
        auto texture = [&](uint64_t id) {
            auto it = loaded.find(id);
            if (it != loaded.end()) return it->second;
            echo::render::ActorTexture t;
            std::string note;
            const bool ok = LoadTexture(tp, id, max_texture, t, note);
            log << "    texture " << Hex(id) << ": " << note << "\n";
            const int at = ok ? int(textures_.size()) : -1;
            if (ok) textures_.push_back(std::move(t));
            loaded.emplace(id, at);
            return at;
        };
        for (Material& m : materials) {
            log << "  material " << Hex(m.symbol);
            const std::filesystem::path* mp = cr.find(static_cast<uint64_t>(echo::rad_hash("CGMaterialResourceWin10")), m.symbol);
            std::vector<uint8_t> mb;
            echo::codecs::MaterialReader mr;
            std::string merr;
            if (!m.symbol || !mp || !ReadFile(*mp, mb) || !mr.decode(mb, merr)) {
                log << " (not readable: drawn untextured)\n";
                m.kind = 0;
                continue;
            }
            const auto& r = mr.resource();
            m.kind = r.material_type == 2 ? 2 : 1;   // the visor glass is type 2 (blend 12)
            log << " type " << r.material_type << " blend " << r.blend_mode << " flags " << r.flags << " -> kind " << m.kind << "\n";
            for (const auto& [slot, tex] : r.texture_bindings) {
                const uint64_t s = static_cast<uint64_t>(slot), t = static_cast<uint64_t>(tex);
                if (s == kSlotBaseColour) m.base = texture(t);
                else if (s == kSlotEmissive) m.emissive = texture(t);
                else if (s == kSlotSecondMask) m.mask = texture(t);
                else if (s == kSlotNormal) m.normal = texture(t);
                else if (s == kSlotSpec) m.spec = texture(t);
                else if (s == kSlotHeightAo && m.kind == 1) m.ao = texture(t);
                else if (s == kSlotTransmit && m.kind == 2) m.ao = texture(t);
                else if (s == kSlotStun && m.kind == 1) m.stun = texture(t);
            }
            auto prop = [&](uint64_t name, float fallback) {
                const auto* h = r.find_property(name);
                if (!h) return fallback;
                const std::vector<float> f = r.property_floats(*h);
                return f.empty() ? fallback : f[0];
            };
            m.emissive_scale = prop(kPropEmissiveIntensity, 2.f);
            m.transmit = prop(kPropTransmitScale, 0.f);
            m.metallic_offset = prop(kPropMetallicOffset, 0.f);
            m.stun_u = prop(kPropStunUScale, 5.f);
            m.stun_v = prop(kPropStunVScale, 3.f);
        }
    }

    bones_ = std::move(bones);
    verts_ = std::move(verts);
    tris_ = std::move(tris);
    materials_ = std::move(materials);
    model_ = model;
    subtree_.assign(bones_.size(), {});
    for (size_t i = 0; i < bones_.size(); ++i)
        for (int c = int(i), guard = 0; c >= 0 && guard <= int(bones_.size()); c = bones_[c].parent, ++guard) subtree_[c].push_back(int(i));

    head_ = Find("EXP_C1_Head1");
    neck_ = Find("EXP_C1_Neck1");
    spine_top_ = Find("EXP_C1_Spine3");
    pelvis_ = Find("EXP_C1_Pelvis1");
    spine_[0] = Find("EXP_C1_Spine1"), spine_[1] = Find("EXP_C1_Spine2"), spine_[2] = spine_top_, spine_[3] = neck_;
    const char* side[2] = {"EXP_L1_", "EXP_R1_"};
    for (int s = 0; s < 2; ++s) {
        auto f = [&](const char* part) { return Find((std::string(side[s]) + part).c_str()); };
        arm1_[s] = f("Arm1"), arm2_[s] = f("Arm2"), hand_[s] = f("Hand1");
        leg1_[s] = f("Leg1"), leg2_[s] = f("Leg2"), foot_[s] = f("Foot1");
    }
    // the model's own axes: up from the pelvis to the head, left from the right shoulder to the left one
    if (head_ >= 0 && pelvis_ >= 0) up_ = Norm(bones_[head_].wt - bones_[pelvis_].wt);
    if (arm1_[0] >= 0 && arm1_[1] >= 0) {
        V3 l = bones_[arm1_[0]].wt - bones_[arm1_[1]].wt;
        l = l - up_ * Dot(l, up_);
        left_ = Norm(l);
    }
    fwd_ = Norm(Cross(left_, up_));
    const int toe = Find("EXP_L1_Toe1");
    if (toe >= 0 && foot_[0] >= 0) {
        V3 d = bones_[toe].wt - bones_[foot_[0]].wt;
        d = d - up_ * Dot(d, up_);
        if (Len(d) > 0.01f && Dot(Norm(d), fwd_) < 0) fwd_ = fwd_ * -1.f;   // left_ keeps naming the L1 side
    }
    // parents first, for the forward pass
    order_.clear();
    {
        std::vector<char> placed(bones_.size(), 0);
        for (size_t pass = 0; pass <= bones_.size() && order_.size() < bones_.size(); ++pass)
            for (size_t i = 0; i < bones_.size(); ++i)
                if (!placed[i] && (bones_[i].parent < 0 || placed[bones_[i].parent])) placed[i] = 1, order_.push_back(int(i));
    }
    // the chassis bones that copy demo-viewer's rig: the neck and the chain hanging from it, the legs
    retarget_.clear();
    {
        const RigIds& id = Ids();
        const bool same = RigLeftIsLeft();
        auto map = [&](const std::string& bone, int node) {
            const int b = Find(bone.c_str());
            if (b >= 0 && node >= 0) retarget_.push_back({b, node});
        };
        map("EXP_C1_Neck1", id.neck);
        map("EXP_C1_Neck2", id.neck);
        map("EXP_C1_Spine3", id.spine2);
        map("EXP_C1_Spine2", id.spine1);
        map("EXP_C1_Spine1", id.spine);
        map("EXP_C1_Pelvis1", id.hips);
        for (int s = 0; s < 2; ++s) {
            const int r = same ? s : 1 - s;
            const std::string side = s == 0 ? "EXP_L1_" : "EXP_R1_";
            map(side + "Leg1", id.up[r]);
            map(side + "Leg2", id.leg[r]);
            map(side + "Foot1", id.foot[r]);
        }
        log << "demo rig: " << kRigN << " nodes, its left side is " << (same ? "the left" : "the right") << ", " << retarget_.size()
            << " chassis bones follow it\n";
    }
    for (int s = 0; s < 2; ++s) {
        touch_[s] = Find(s == 0 ? "EXP_L1_TouchController1" : "EXP_R1_TouchController1");
        thumb_[s] = Find(s == 0 ? "EXP_L1_Thumb2" : "EXP_R1_Thumb2");
        if (thumb_[s] < 0) thumb_[s] = Find(s == 0 ? "EXP_L1_Thumb1" : "EXP_R1_Thumb1");
        fingers_[s].clear();
        mid_[s] = Find(s == 0 ? "EXP_L1_Middle1" : "EXP_R1_Middle1");
        for (const char* f : {"Index", "Middle", "Ring", "Pinky"})
            for (int k = 1; k <= 3; ++k) {
                const int b = Find((std::string(s == 0 ? "EXP_L1_" : "EXP_R1_") + f + std::to_string(k)).c_str());
                if (b >= 0) fingers_[s].push_back(b);
            }
    }
    size_t named = 0;
    for (const Bone& b : bones_) named += b.label.rfind("bone_", 0) != 0;
    log << named << "/" << bones_.size() << " bones named\n";
    for (size_t i = 0; i < bones_.size(); ++i) {   // the bind skeleton, for the test log
        const Bone& b = bones_[i];
        const V3 x = Rotate(b.wq, {1, 0, 0}), y = Rotate(b.wq, {0, 1, 0}), z = Rotate(b.wq, {0, 0, 1});
        char line[320];
        snprintf(line, sizeof line, "  %3zu %-28s <- %-22s at %6.3f %6.3f %6.3f  X %5.2f %5.2f %5.2f  Y %5.2f %5.2f %5.2f  Z %5.2f %5.2f %5.2f\n", i,
                 b.label.c_str(), b.parent >= 0 ? bones_[b.parent].label.c_str() : "-", b.wt.x, b.wt.y, b.wt.z, x.x, x.y, x.z, y.x, y.y, y.z, z.x,
                 z.y, z.z);
        log << line;
    }
    notes_ = log.str();
    return true;
}

std::string Avatar::Describe() const { return model_ + "\n" + (why_.empty() ? notes_ : why_ + "\n"); }

void Avatar::TakeMaterials(std::vector<echo::render::ActorTexture>& textures, std::vector<echo::render::ActorMaterial>& materials) {
    const int offset = int(textures.size());
    for (auto& t : textures_) textures.push_back(std::move(t));
    textures_.clear();
    for (const Material& m : materials_) {
        echo::render::ActorMaterial a;
        a.kind = uint32_t(m.kind);
        a.base = m.base >= 0 ? m.base + offset : -1;
        a.emissive = m.emissive >= 0 ? m.emissive + offset : -1;
        a.mask = m.mask >= 0 ? m.mask + offset : -1;
        a.normal = m.normal >= 0 ? m.normal + offset : -1;
        a.spec = m.spec >= 0 ? m.spec + offset : -1;
        a.ao = m.ao >= 0 ? m.ao + offset : -1;
        a.stun = m.stun >= 0 ? m.stun + offset : -1;
        a.emissive_scale = m.emissive_scale;
        a.transmit = m.transmit;
        a.opacity = m.kind == 2 ? kVisorOpacity : 1.f;
        a.metallic_offset = m.metallic_offset;
        a.stun_uv_scale[0] = m.stun_u, a.stun_uv_scale[1] = m.stun_v;
        materials.push_back(a);
    }
}

// ---- demo-viewer's IKController, run on its own rig ------------------------------------------------------------------
//
// demo-viewer feeds the API's vectors to Unity with x and z swapped (EchoVRAPI ToVector3), a mirror; its IKController
// then sets world and local rotations on the prefab's mixamo rig (Unity's LookRotation and Euler). That is reproduced
// here number for number in Unity's space, and what each rig joint turned by -- against the same rig with no lean and
// straight legs -- is mirrored back and given to the matching chassis bone.
DemoLean DemoLeanAt(const PlayerTrack& track, double t, int forward_axis) {
    DemoLean L;
    constexpr int kFrames = 120;   // two seconds at 60 frames a second: 0.95^120 leaves nothing of the start
    for (int k = kFrames; k >= 0; --k) {
        PlayerPose s;
        if (!SamplePlayer(track, t - k / 60.0, s)) continue;
        float x[4], y[4], rest;
        LeanTargets(BodyForward(s.head_q, forward_axis), s.vel, x, y, rest);
        for (int i = 0; i < 4; ++i) L.x[i] += (x[i] - L.x[i]) * 0.05f, L.y[i] += (y[i] - L.y[i]) * 0.05f;
        L.rest = rest;
        L.legs += (rest - L.legs) * 0.05f;
    }
    return L;
}

void Avatar::Add(const PlayerPose& p, const AvatarStyle& st, const DemoLean* lean, std::vector<echo::render::ActorMeshVertex>& out,
                 std::vector<std::vector<uint32_t>>& by_material, size_t material_base) const {
    if (!ok()) return;
    const size_t nb = bones_.size();

    // frames: the model's (bind), the convention the recording's rotations use (forward axis, +Y up)
    const Q4 model_frame = FromBasis(Cross(up_, fwd_), up_, fwd_);
    V3 cf;
    switch (st.forward_axis) {
        case 0: cf = {1, 0, 0}; break;
        case 1: cf = {-1, 0, 0}; break;
        case 3: cf = {0, 0, -1}; break;
        default: cf = {0, 0, 1}; break;
    }
    const V3 cu{0, 1, 0};
    const Q4 conv = FromBasis(Cross(cu, cf), cu, cf);
    const Q4 head_delta = QNorm(Mul(Mul(p.head_q, conv), Conj(model_frame)));   // model -> where the head is

    // The body frame. demo-viewer keeps it upright, facing the head's heading; in Echo's zero-g a player who pitches
    // or rolls turns their whole body, and an upright torso under a tumbling head tore the neck and over-stretched
    // the arms. So the body takes most of the head's full turn (kBodyFollow), the rest staying upright: a player
    // looking down still bends at the neck, and one upside down hangs upside down.
    constexpr float kBodyFollow = 0.7f;
    V3 bf, bu;
    {
        const V3 lf = BodyForward(p.head_q, st.forward_axis), wu{0, 1, 0};
        V3 hf, hu, hr;
        HeadAxes(p.head_q, st.forward_axis, hf, hu, hr);
        const V3 hz = Norm(hf), hy = Norm(hu - hz * Dot(hu, hz));
        const Q4 upright = FromBasis(Cross(wu, lf), wu, lf), full = FromBasis(Cross(hy, hz), hy, hz);
        const Q4 b = QNorm(Slerp(upright, full, Len(hy) > 0.5f ? kBodyFollow : 0.f));
        bf = Norm(Rotate(b, V3{0, 0, 1}));
        bu = Norm(Rotate(b, V3{0, 1, 0}));
    }
    const Q4 body = QNorm(Mul(FromBasis(Cross(bu, bf), bu, bf), Conj(model_frame)));

    // IKController on demo-viewer's rig, now and with no lean / straight legs; what each joint turned by
    DemoLean L;
    if (lean) {
        L = *lean;
    } else {
        LeanTargets(bf, p.vel, L.x, L.y, L.rest);
        L.legs = L.rest;
    }
    RigPose now, base;
    SolveRig(U(p.head), U(bf), U(bu), L, false, now);
    SolveRig(U(p.head), U(bf), U(bu), L, true, base);

    std::vector<Q4> W(nb), ovr(nb);
    std::vector<V3> P(nb), pos(nb);
    std::vector<char> has(nb, 0), has_pos(nb, 0);
    for (const auto& [bone, node] : retarget_) {
        ovr[bone] = QNorm(Mul(Mul(UQ(QNorm(Mul(now.q[node], Conj(base.q[node])))), body), bones_[bone].wq));
        has[bone] = 1;
    }
    if (head_ >= 0) ovr[head_] = QNorm(Mul(head_delta, bones_[head_].wq)), has[head_] = 1;   // head.rotation = the head's

    V3 offset{};
    auto fk = [&] {
        for (int b : order_) {
            const Bone& B = bones_[b];
            if (B.parent < 0) {
                W[b] = has[b] ? ovr[b] : QNorm(Mul(body, B.wq));
                P[b] = Rotate(body, B.wt) + offset;
            } else {
                W[b] = has[b] ? ovr[b] : QNorm(Mul(W[B.parent], B.lq));
                P[b] = P[B.parent] + Rotate(W[B.parent], B.lt);
            }
            if (has_pos[b]) P[b] = pos[b];
        }
    };
    fk();
    // the prefab hangs the rig's neck 21.6 cm below the head position, along the body's up
    const int anchor = neck_ >= 0 ? neck_ : head_;
    if (anchor >= 0) offset = (p.head - bu * neck_drop) - P[anchor];
    fk();

    // arms: TwoBoneIK to the hand, the elbow toward the prefab's hint (which hangs from the neck), the hand where the
    // game's own TouchController bone puts it relative to the controller
    const bool same = RigLeftIsLeft();
    V3 target[2];
    Q4 hand_q[2];
    bool arm[2]{};
    for (int s = 0; s < 2; ++s) {
        const int a1 = arm1_[s], a2 = arm2_[s], h = hand_[s];
        if (a1 < 0 || a2 < 0 || h < 0) continue;
        arm[s] = true;
        Q4 cq = s == 0 ? p.l_q : p.r_q;
        switch (hand_frame) {   // other readings of the controller frame (testing, ini avatar_hand=)
            case 1: cq = Mul(cq, AxisAngle({0, 0, 1}, 3.14159265f)); break;
            case 2: cq = Mul(cq, AxisAngle({0, 1, 0}, 3.14159265f)); break;
            case 3: cq = Mul(cq, AxisAngle({1, 0, 0}, 3.14159265f)); break;
            default: break;
        }
        const V3 ctrl = s == 0 ? p.lhand : p.rhand;
        const Bone& BH = bones_[h];
        if (touch_[s] >= 0) {
            const Bone& BT = bones_[touch_[s]];
            const Q4 rel_q = QNorm(Mul(Conj(BH.wq), BT.wq));
            const V3 rel_t = Rotate(Conj(BH.wq), BT.wt - BH.wt);
            hand_q[s] = QNorm(Mul(cq, Conj(rel_q)));
            target[s] = ctrl - Rotate(hand_q[s], rel_t);
        } else {
            hand_q[s] = QNorm(Mul(cq, BH.wq));
            target[s] = ctrl;
        }
        const float l1 = Len(bones_[a2].wt - bones_[a1].wt), l2 = Len(BH.wt - bones_[a2].wt);
        const V3 hint = U(now.p[Ids().hint[same ? s : 1 - s]]);
        const V3 sh = P[a1];
        const V3 d = target[s] - sh;
        const float len = std::clamp(Len(d), 0.05f, (l1 + l2) * 0.999f);
        const V3 dir = Norm(d);
        const float a = (l1 * l1 - l2 * l2 + len * len) / (2 * len);
        const float hh = sqrtf(std::max(0.f, l1 * l1 - a * a));
        const V3 pole = hint - sh;
        V3 side = pole - dir * Dot(pole, dir);
        if (Len(side) < 1e-4f) side = Cross(dir, V3{0, 1, 0});
        const V3 elbow = sh + dir * a + Norm(side) * hh;
        ovr[a1] = QNorm(Mul(FromTo(P[a2] - P[a1], elbow - P[a1]), W[a1]));
        has[a1] = 1;
    }
    fk();
    for (int s = 0; s < 2; ++s) {
        if (!arm[s]) continue;
        const int a2 = arm2_[s], h = hand_[s];
        ovr[a2] = QNorm(Mul(FromTo(P[h] - P[a2], target[s] - P[a2]), W[a2]));
        has[a2] = 1;
    }
    fk();
    for (int s = 0; s < 2; ++s) {
        if (!arm[s]) continue;
        const int a2 = arm2_[s], h = hand_[s];
        // the forearm takes most of the hand's twist about its own length, so the wrist does not wring
        const Q4 rigid = QNorm(Mul(hand_q[s], Conj(bones_[h].lq)));
        const Q4 delta = QNorm(Mul(rigid, Conj(W[a2])));
        const V3 ax = Norm(P[h] - P[a2]);
        const V3 proj = ax * Dot(V3{delta.x, delta.y, delta.z}, ax);
        Q4 twist = QNorm({proj.x, proj.y, proj.z, delta.w});
        if (twist.w < 0) twist = {-twist.x, -twist.y, -twist.z, -twist.w};
        ovr[a2] = QNorm(Mul(Slerp(Q4{}, twist, 0.7f), W[a2]));
        ovr[h] = hand_q[s], has[h] = 1;
        pos[h] = target[s], has_pos[h] = 1;
    }
    fk();

    // fingers: grip with the trigger
    struct X {
        Q4 q;
        V3 t;
    };
    std::vector<X> g(nb);
    for (size_t i = 0; i < nb; ++i) g[i] = {W[i], P[i]};
    for (int s = 0; s < 2; ++s) {
        const int h = hand_[s];
        // The recording has no finger bones or grip value, only each hand's trigger: a relaxed hand rests half
        // closed (never flat), and the trigger closes it into a fist.
        constexpr float kRestCurl = 0.35f, kTriggerCurl = 0.8f;
        const float curl = kRestCurl + std::clamp(s == 0 ? p.trig_l : p.trig_r, 0.f, 1.f) * kTriggerCurl;
        if (h < 0 || mid_[s] < 0) continue;
        const V3 fingers = Norm(bones_[mid_[s]].wt - bones_[h].wt);
        V3 back = up_ - fingers * Dot(up_, fingers);
        back = Len(back) > 0.1f ? Norm(back) : fwd_;
        const V3 axis = Rotate(g[h].q, Rotate(Conj(bones_[h].wq), Cross(back, fingers))) * (s == 0 ? -1.f : 1.f);
        for (int b : fingers_[s]) {
            const Q4 d = AxisAngle(axis, curl * 0.5f);
            const V3 pivot = g[b].t;
            for (int k : subtree_[b]) {
                g[k].t = pivot + Rotate(d, g[k].t - pivot);
                g[k].q = QNorm(Mul(d, g[k].q));
            }
        }
    }

    // skin, in parallel chunks
    std::vector<X> m(nb);
    for (size_t i = 0; i < nb; ++i) {
        m[i].q = QNorm(Mul(g[i].q, Conj(bones_[i].wq)));
        m[i].t = g[i].t - Rotate(m[i].q, bones_[i].wt);
    }
    const uint32_t base_v = uint32_t(out.size());
    out.resize(out.size() + verts_.size());
    // the team's tints always; a stun is the game's hit-fx shimmer over them (rgba2's alpha), not a recolour
    const uint32_t tint = st.primary;
    const uint32_t tint2 = (st.secondary & 0x00FFFFFFu) | (p.stunned ? 0xFF000000u : 0u);
    echo::render::ActorMeshVertex* dst = out.data() + base_v;
    constexpr size_t kChunk = 4096;
    std::vector<size_t> chunks((verts_.size() + kChunk - 1) / kChunk);
    std::iota(chunks.begin(), chunks.end(), size_t(0));
    std::for_each(std::execution::par, chunks.begin(), chunks.end(), [&](size_t c) {
        const size_t end = std::min(verts_.size(), (c + 1) * kChunk);
        for (size_t i = c * kChunk; i < end; ++i) {
            const Vert& v = verts_[i];
            V3 pp{}, nn{}, tt{};
            for (int k = 0; k < 4; ++k) {
                const float w = v.bw[k];
                if (w <= 0) continue;
                const X& s = m[v.bi[k]];
                pp = pp + (Rotate(s.q, v.p) + s.t) * w;
                nn = nn + Rotate(s.q, v.n) * w;
                tt = tt + Rotate(s.q, v.t) * w;
            }
            nn = Norm(nn);
            dst[i] = {{pp.x, pp.y, pp.z}, {nn.x, nn.y, nn.z}, {v.u, v.v}, tint, tint2, {tt.x, tt.y, tt.z, v.tw}};
        }
    });
    for (size_t t = 0; t + 2 < tris_.size(); t += 3) {
        const size_t mat = material_base + verts_[tris_[t]].material;
        if (mat >= by_material.size()) by_material.resize(mat + 1);
        std::vector<uint32_t>& into = by_material[mat];
        into.insert(into.end(), {base_v + tris_[t], base_v + tris_[t + 1], base_v + tris_[t + 2]});
    }
}

}  // namespace erv
