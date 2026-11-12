// World scene: terrain, sky and placed mission objects, built from a loaded mission.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/assets.h"
#include "../assets/mission.h"
#include "../core/fs.h"
#include "../render/renderer.h"

namespace hg2 {

class Scene {
public:
    Scene(const FileSystem& fs, Renderer& r) : fs_(fs), r_(r) {}
    // `prepare` runs on the parsed mission before anything is built (gear swaps, squad mates).
    bool load(const std::string& missionName, std::string* err, const std::function<void(Mission&)>& prepare = nullptr);
    void render(const FrameParams& base);
    FrameParams frameDefaults() const;
    const Mission& mission() const { return mission_; }
    Mission& missionRW() { return mission_; }  // definitions added at run time (instant-action waves)
    float groundHeight(float x, float z) const;
    // The mission's surface entry under a point (lava etc.), or nullptr when the terrain has none there.
    const Surface* surfaceAt(float x, float z) const;
    Vec3 startPosition() const;
    void setTime(float seconds) { time_ = seconds; }
    // Pose a placed object (player / simulated units): world position, yaw (radians), animation.
    void setObjectPose(int object, const Vec3& pos, float yaw, int animSet, float frame);
    // A second animation set played on one branch of the model (ECY kind-3 transitions: hit reactions on the
    // body, "BDB"); set < 0 removes it.
    void setObjectOverlay(int object, int animSet, float frame, const std::string& rootFrame);
    int animLength(int object, int animSet) const;
    float animStride(int object, int animSet) const;  // the top frame's horizontal travel over the set (root motion)
    // Pushes a vertical cylinder (feet at pos.y, given radius/height) out of static object boxes.
    // Returns true if it hit something.
    bool collide(Vec3& pos, float radius, float height, int ignoreObject) const;
    // Highest walkable surface under pos: terrain or the top of an object box no more than `step` above the feet.
    float supportHeight(const Vec3& pos, float step, int ignoreObject) const;
    void setObjectDrawn(int object, bool drawn);
    int addObject(const MissionObject& o);  // spawned at run time (fountains); returns its object index
    void setObjectTwist(int object, float yaw);  // rotates the torso-twist frame (frm-tst)
    // Rigged units: world position of a frame (e.g. "frm-hp1", "frm-POV"), the nearest of several frames to a
    // point, whether a frame hangs under another, and hiding a frame with everything attached to it.
    bool frameWorld(int object, const std::string& frame, Vec3& out) const;
    bool frameBottom(int object, const std::string& frame, Vec3& out) const;  // bottom centre of a part's mesh, posed
    bool objectPosition(int object, Vec3& out) const;  // current placement of any instance
    std::string nearestFrame(int object, const Vec3& p, const std::vector<std::string>& frames) const;
    bool frameUnder(int object, const std::string& frame, const std::string& ancestor) const;
    void hideFrame(int object, const std::string& frame);
    void clearDamage(int object);  // blown-off parts and scorch back (respawn)
    // Battle damage: the frame is overdrawn with the definition's damaged skin at this strength (0..1).
    void setFrameDamage(int object, const std::string& frame, const std::string& texture, float amount);
    void setWaterLevel(float y) { waterOffset_ = y - mission_.waterLevel; }  // SetWaterLevel
    float waterLevel() const { return mission_.waterLevel + waterOffset_; }
    // First object box hit by the segment a->b (returns object index or -1; t = fraction along the segment).
    int raycast(const Vec3& a, const Vec3& b, int ignoreObject, float& t) const;
    void hideObject(int object);
    bool objectHidden(int object) const;
    float terrainRay(const Vec3& a, const Vec3& b) const;  // fraction where the segment enters the ground (>1: none)
    Renderer& renderer() { return r_; }
    void drawExtra(const GpuMesh* m, const Mat4& w, bool lit, float alpha = 1.f) { r_.draw(m, w, lit, alpha); }

private:
    int texture(const std::string& name);
    GpuMesh* model(const std::string& name);
    struct Rigged {
        Model model;
        std::vector<GpuMesh*> parts;  // one per frame (local space), nullptr for empty frames
    };
    Rigged* rigged(const std::string& name);
    const std::vector<AnimSet>* animSets(const std::string& ecyName);
    struct Instance;
    // Walks a rigged instance's frames at its current pose: f(frame index, frame, world matrix, hidden branch).
    void walkPose(const Instance& i, const std::function<void(int, const Frame&, const Mat4&, bool)>& f) const;
    void buildTerrain();
    void addInstance(size_t objectIndex);
    void appendModel(const Model& m, std::vector<Vertex>& out, std::vector<DrawBatch>& batches, int onlyFrame = -1);
    float time_ = 0;

    const FileSystem& fs_;
    Renderer& r_;
    Mission mission_;
    Terrain terrain_;
    bool hasTerrain_ = false;
    GpuMesh* terrainMesh_ = nullptr;
    GpuMesh* waterMesh_ = nullptr;
    float waterOffset_ = 0;
    GpuMesh* sky_ = nullptr;
    std::unordered_map<std::string, int> textures_;
    std::unordered_map<std::string, GpuMesh*> models_;
    struct Box { Vec3 lo, hi; };  // model-space bounds of one frame mesh
    std::unordered_map<std::string, std::vector<Box>> boxes_;
    std::unordered_map<std::string, std::unique_ptr<Rigged>> rigged_;
    std::unordered_map<std::string, std::unique_ptr<std::vector<AnimSet>>> anims_;
    struct Instance {
        GpuMesh* mesh = nullptr;
        Mat4 world;
        Rigged* rig = nullptr;
        const std::vector<AnimSet>* sets = nullptr;
        int set = 0;
        int object = -1;       // mission object index
        float frame = -1;      // animation key time override (< 0: loop on scene time)
        const std::vector<Box>* boxes = nullptr;
        bool hidden = false;
        bool noDraw = false;   // still collides / takes hits (player in cockpit view)
        float twist = 0;
        int overlaySet = -1;
        float overlayFrame = 0;
        std::string overlayRoot;  // lower case
        std::vector<std::string> hiddenFrames;
        std::vector<std::pair<std::string, float>> damagedFrames;  // lower-case frame name, strength
        int damageTex = -1;  // blown-off parts (lower-case frame names; children go too)
        Mat4 inv;              // world -> model (rigid inverse of world)
    };
    std::vector<Instance> instances_;
};

}  // namespace hg2
