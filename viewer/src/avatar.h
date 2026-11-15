#pragma once
// Real Echo players: the chassis mesh and its skeleton read from the game's packages (CGMeshListResourceWin10 and
// CSkeletonResourceWin10 at the model's name), textured with the chassis' own maps (its CGSceneResource palette ->
// CGMaterialResource bindings -> cgtextureresource + streamed RawTexturePackfile mips), posed per player the way
// demo-viewer's IKController does (torso from the body, a velocity lean down the spine, legs that fold at rest and
// trail at speed, two-bone arms to the controllers) and skinned on the CPU into the renderer's actor meshes.
//
// The skeleton layout is lone_echo_blender's decode (scripts/evr_apply_skeleton.py, docs/EVR_SKELETONS.md): a 32-byte
// bind pose per bone (quaternion, parent-relative translation, scale) and a 24-byte first-child / next-sibling
// hierarchy with each bone's CSymbol64 name.
//
// The mesh list carries every LOD of the chassis at once (rows in groups: body, visor, trim); only LOD 0 is kept,
// found by the palette naming the lower LODs' materials "<model>_rift_lod_N".

#include <stdint.h>

#include <filesystem>
#include <string>
#include <vector>

#include <echo/renderer/preview.hpp>

#include "replay.h"

namespace erv {

// demo-viewer IKController's per-player state: the spine's X (pitch) / Y (roll) lean per joint and the legs' rest
// multiplier, each eased toward its target by 5% a frame. DemoLeanAt replays those frames (60 a second) over the last
// two seconds of the track, so a paused or scrubbed replay poses the same as one that played up to it.
struct DemoLean {
    float x[4]{}, y[4]{};   // BodyXRotation0..3, BodyYRotation0..3 (degrees)
    float rest = 1;         // TargetlegsRestingMultiplier (not eased): 1 at rest, 0 at 5 m/s
    float legs = 1;         // LegsRestingMultiplier (eased)
};
DemoLean DemoLeanAt(const PlayerTrack&, double t, int forward_axis);

struct AvatarStyle {
    uint32_t primary = 0xFFFFFFFF;     // the main tint, 0xAABBGGRR
    uint32_t secondary = 0xFF303030;   // the second tint (where the chassis' secondary blend mask is set)
    uint32_t glow = 0xFFFFFFFF;        // visor / light strips
    int forward_axis = 2;              // which head axis looks forward (figure.h HeadAxes)
};

class Avatar {
public:
    // Loads `model` (e.g. "cst_body_s11_a") from the corpus EchoConstruct reads. Keeps the last good one.
    bool Load(const std::filesystem::path& corpus, const std::string& model);
    bool ok() const { return !bones_.empty() && !verts_.empty(); }
    const std::string& why() const { return why_; }
    const std::string& model() const { return model_; }
    std::string Describe() const;   // bones, rows, what was found (the test log)

    // The renderer materials this chassis draws with, appended (texture indices offset by what `textures` held).
    // Moves the texels out: call once.
    void TakeMaterials(std::vector<echo::render::ActorTexture>& textures, std::vector<echo::render::ActorMaterial>& materials);
    size_t material_count() const { return materials_.size(); }

    // Poses and skins one player. Indices go to `by_material[material_base + m]` (sized by the caller).
    // `lean` is the player's IKController state (DemoLeanAt); without it the targets are used as they stand.
    void Add(const PlayerPose&, const AvatarStyle&, const DemoLean* lean, std::vector<echo::render::ActorMeshVertex>& out,
             std::vector<std::vector<uint32_t>>& by_material, size_t material_base) const;

    // Tuning, from the ini (avatar_*).
    float eye_up = 0.07f, eye_fwd = 0.09f;
    float neck_drop = 0.216f;   // the neck joint below the recorded head position (demo-viewer's prefab)
    int tri_budget = 0;      // triangles kept per chassis (ini avatar_tris), 0 = all
    int hand_frame = 0;      // the controller frame -> the TouchController bone: 0 as-is, 1..3 turned variants (testing)
    int max_texture = 2048;  // the largest texture level uploaded (ini avatar_texture)

private:
    struct Bone {
        uint64_t name = 0;
        std::string label;
        int parent = -1;
        Q4 lq;          // bind, parent-relative
        V3 lt;
        Q4 wq;          // bind, model space
        V3 wt;
    };
    struct Vert {
        V3 p, n;
        V3 t;              // tangent (the game's own), tw its bitangent sign: 0 = none
        float tw = 0;
        float u, v;
        uint8_t bi[4];
        float bw[4];
        uint32_t material;   // index into materials_
    };
    struct Material {
        uint64_t symbol = 0;
        int kind = 1;              // ActorMaterial::kind
        int base = -1, emissive = -1, mask = -1;   // into textures_
        int normal = -1, spec = -1, ao = -1, stun = -1;   // ao: the visor's transmittance map for kind 2
        float emissive_scale = 1, transmit = 0, metallic_offset = 0, stun_u = 5, stun_v = 3;
    };
    std::vector<Bone> bones_;
    std::vector<std::vector<int>> subtree_;   // each bone and everything under it
    std::vector<Vert> verts_;
    std::vector<uint32_t> tris_;
    std::vector<Material> materials_;
    std::vector<echo::render::ActorTexture> textures_;
    int head_ = -1, neck_ = -1, spine_top_ = -1, pelvis_ = -1;
    int spine_[4]{-1, -1, -1, -1};            // Spine1..3, Neck1
    std::vector<int> fingers_[2];
    std::vector<int> order_;                  // bones, parents first
    std::vector<std::pair<int, int>> retarget_;   // chassis bone <- demo rig node whose turn it copies
    int mid_[2]{-1, -1}, thumb_[2]{-1, -1}, touch_[2]{-1, -1};
    int arm1_[2]{-1, -1}, arm2_[2]{-1, -1}, hand_[2]{-1, -1}, leg1_[2]{-1, -1}, leg2_[2]{-1, -1}, foot_[2]{-1, -1};
    V3 fwd_{0, 0, 1}, up_{0, 1, 0}, left_{1, 0, 0};   // the model's own axes in bind
    std::string model_, why_, notes_;
    int Find(const char* name) const;
};

}  // namespace erv
