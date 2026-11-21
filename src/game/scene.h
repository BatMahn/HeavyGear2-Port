// World scene: terrain, sky and placed mission objects, built from a loaded mission.
#pragma once
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/assets.h"
#include "../assets/collision.h"
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
    bool hasTerrain() const { return hasTerrain_; }
    Vec3 groundNormal(float x, float z) const;  // the terrain triangle's normal (up when there's no terrain)
    // Radius of the sphere round an object's model (rigged units at their current pose); 0 if it has none.
    float objectRadius(int object) const;
    bool objectCenter(int object, Vec3& out) const;  // centre of that sphere (the posed model's box centre)
    // The mission's surface entry under a point (lava etc.), or nullptr when the terrain has none there.
    const Surface* surfaceAt(float x, float z) const;
    Vec3 startPosition() const;
    void setTime(float seconds) {
        time_ = seconds;
        for (auto& i : instances_)
            if (i.rig && i.frame < 0) i.poseDirty = true;  // looping on scene time
    }
    // Pose a placed object (player / simulated units): world position, yaw (radians), animation.
    void setObjectPose(int object, const Vec3& pos, float yaw, int animSet, float frame);
    // The same with the body tilted: pitch about its own X axis (+ raises the nose, models face -Z) and roll about its
    // Z axis, applied in the body's frame before the yaw (vehicles that sit on the ground: tanks, hovers, mounts).
    void setObjectPose(int object, const Vec3& pos, float yaw, int animSet, float frame, float pitch, float roll);
    // A rigged unit's frame at rest (no animation) in model space: its origin, and its mesh's box (lo / hi; zero when
    // it has no mesh). False when the model has no such frame. (FUN_10019b90 + the frame's vtable +0x24 / FUN_1001f3c0.)
    bool frameRest(int object, const std::string& frame, Vec3& origin, Vec3& lo, Vec3& hi) const;
    // The proxy's +0x58 (FUN_100a79b0): the product of the model's box extents at rest, at least 0.1.
    float boxVolume(int object) const;
    bool isUnit(int object) const;  // a unit class (collides with units only in pairs that hold the player)
    // WalkableGraph (mission token 0x14f, FUN_10084f90 / FUN_1001a1a0 / FUN_1001a550): an object's hull graph in world
    // space - one node per frame of the graph's geometry file, in depth-first order, linked as the .grx lists them.
    struct WalkGraph {
        int object = -1;
        std::vector<Vec3> nodes;
        std::vector<std::vector<int>> links;
        Vec3 centre;    // the object's origin and bounding radius (FUN_100c1da0 only looks at graphs within it)
        float radius = 0;
    };
    const std::vector<WalkGraph>& walkGraphs() const { return graphs_; }
    // FUN_100c1da0: the nearest node of the first graph whose object holds p (see scene.cpp).
    bool nearestGraphNode(const Vec3& p, int& graph, int& node) const;
    // A second animation set played on one branch of the model (ECY kind-3 transitions: hit reactions on the
    // body, "BDB"); set < 0 removes it.
    void setObjectOverlay(int object, int animSet, float frame, const std::string& rootFrame);
    // Fade the pose from another set (weight 1 .. 0) into the current one; animSet < 0 ends it.
    void setObjectFade(int object, int animSet, float frame, float weight);
    int animLength(int object, int animSet) const;
    int animCycle(int object, int animSet) const;  // a looping set's length without its trailing padding keys
    // The root motion walkPose strips from a locomotion set at key time t: the top frames' horizontal travel since
    // the first key, in model space (models face -Z). Player moves the body by its change from frame to frame.
    Vec3 animTravel(int object, int animSet, float t) const;
    // Collision (the original's world pass, FUN_100a0b00 -> FUN_100a63d0 -> FUN_1000d690; see collision.h): the
    // nearest front-face crossing of the segment a -> b with an object's polygons. Static objects always take part;
    // units only as `units` says: -1 none, -2 all of them, >= 0 that one unit (the pass tests unit against unit only
    // in pairs that hold the player, DAT_101fcf48). endInBox: only objects whose box holds a or b (the part segments
    // of FUN_100a63d0's second loop, FUN_100a6330).
    struct SegmentHit { int object = -1; float t = 2.f; Vec3 n; };
    bool segment(const Vec3& a, const Vec3& b, int ignoreObject, int units, SegmentHit& hit, bool endInBox = false) const;
    // The pathfinder's object test at a node (FUN_100c2210 -> FUN_100c20e0): a static object's box blocks p when it
    // reaches within r of p on x and z and spans p.y .. p.y + 2r in height. Only the objects listed on p's 32 m
    // terrain tile count; units are never obstacles (their [4] bit 0x10000 is clear).
    // Returns 0 when no object stands there, 1 when one blocks, 2 when a bridge (class 0x13f) declares the point
    // walkable (its vtable +0xb4, FUN_10040270) - then *end is the bridge end whose square holds it (FUN_10040590,
    // bridge * 2 + 0 / 1) or -1. The tile's objects are asked in the order they were placed; the first answer counts.
    int objectTest(const Vec3& p, float r, int self, int* by = nullptr, int* end = nullptr) const;
    // Bridges (class 0x13f, FUN_1003f370 / FUN_1003f950): the two ends (frames HP1 / HP2, on the ground under them,
    // both at the lower one's height) with their outward axis (the frame's Z), and the half-width numbers.
    struct Bridge {
        int object = -1;
        Vec3 end[2], axis[2];
        Vec3 xaxis[2];          // the HP frame's X row (FUN_1003f6a0's sideways step)
        Vec3 lo, hi;            // the object's world box (+0xe90 / +0xea8)
        float halfW = 0;        // +0xe88: half the smaller of the box's widths
        float lineTol = 0;      // +0xe84: an eighth of it
        float square = 8.f;     // an end's square: 2 x the pathfinder's 4 m cell (FUN_1003f6a0)
    };
    const std::vector<Bridge>& bridges() const { return bridges_; }
    // FUN_10040270 for one bridge: whether p counts as walkable; end: the end square holding p, or -1
    bool bridgeWalkable(const Bridge& b, const Vec3& p, int& end) const;
    // A unit's bounding-sphere radius (object +0x98, FUN_10017ff0: the model at rest), 0 if none.
    float boundRadius(int object) const;
    // Segments from the centre of a unit's body (its middle mesh part's sphere centre, proxy +0x8c) to each mesh
    // part's origin, posed: what FUN_100a63d0's second loop pushes through the polygons of objects near the player.
    bool bodySegments(int object, Vec3& centre, std::vector<Vec3>& ends) const;
    void setPlayerObject(int object) { playerObject_ = object; }
    // HG2_PROF: time spent in collision queries (ns) and their count since the last take
    mutable uint64_t profNs = 0, profCalls = 0;
    int playerObject() const { return playerObject_; }
    void setObjectDrawn(int object, bool drawn);
    int addObject(const MissionObject& o);  // spawned at run time (fountains); returns its object index
    void setObjectTwist(int object, float yaw);  // rotates the torso-twist frame (frm-tst)
    void setObjectPitch(int object, float up);   // raises a turret's gun on its pitch-joint frame (frm-xa)
    // Rigged units: world position of a frame (e.g. "frm-hp1", "frm-POV"), the nearest of several frames to a
    // point, whether a frame hangs under another, and hiding a frame with everything attached to it.
    bool frameWorld(int object, const std::string& frame, Vec3& out) const;
    bool frameWorldMatrix(int object, const std::string& frame, Mat4& out) const;  // the frame's posed world matrix
    // An object's WalkableGraph (object token 0x14f, a text .grx file, FUN_10084f90): "Geometry_File: <model>" (keyword
    // 0x6e) gives the nodes - every frame of that model in depth-first order (FUN_1001a1a0: a frame, its children
    // (+0x13c), then its siblings (+0x138)), placed by the frame chain under the object's matrix (FUN_1001a550 puts
    // the object's matrix on the root) - and each "Vertex n {a b ...}" line (keyword 0xcd) node n's links. Node
    // records are 0x28 bytes: position, +0x18 link count, +0x1a index, +0x1c links, +0x20 taken by a generator.
    struct GenGraph {
        std::vector<Vec3> nodes;
        std::vector<std::vector<int>> links;
        std::vector<bool> taken;
    };
    const GenGraph* walkGraph(int object);
    // FUN_100c1da0: the first object (in the generator's cell, +0x40 clear, with a graph) whose position is within its
    // bounding radius of p (largest axis difference), and its node nearest p by the same measure - the first one
    // within 2 m (DAT_10148418), else the nearest. False when no object qualifies.
    bool graphNodeAt(const Vec3& p, int& object, int& node);
    bool frameBottom(int object, const std::string& frame, Vec3& out) const;  // bottom centre (the lower Y face) of a part's mesh, posed
    bool objectPosition(int object, Vec3& out) const;  // current placement of any instance
    bool objectWorld(int object, Mat4& out) const;     // its whole placement (position and orientation)
    bool objectAxis(int object, int row, Vec3& out) const;  // a row of its world matrix (0 local x, 2 local z)
    std::string nearestFrame(int object, const Vec3& p, const std::vector<std::string>& frames) const;
    bool frameUnder(int object, const std::string& frame, const std::string& ancestor) const;
    void hideFrame(int object, const std::string& frame);
    void clearDamage(int object);  // blown-off parts and scorch back (respawn)
    // Battle damage: the frame is overdrawn with the definition's damaged skin at this strength (0..1).
    void setFrameDamage(int object, const std::string& frame, const std::string& texture, float amount);
    void setWaterLevel(float y) { waterOffset_ = y - mission_.waterLevel; }  // SetWaterLevel
    // FUN_100a5790: the surface bobs 0.09 m on a sine whose phase runs at Water_Tidal_Rate radians a second
    float waterLevel() const { return mission_.waterLevel + waterOffset_ + std::sin(mission_.waterTidal * time_) * 0.09f; }
    // First object hit by the segment a->b, units included (returns object index or -1; t = fraction along it).
    int raycast(const Vec3& a, const Vec3& b, int ignoreObject, float& t) const;
    void hideObject(int object);
    void showObject(int object);  // a recycled object back in the world (fountain pools)
    bool objectHidden(int object) const;
    float terrainRay(const Vec3& a, const Vec3& b) const;  // fraction where the segment enters the ground (>1: none)
    Renderer& renderer() { return r_; }
    void drawExtra(const GpuMesh* m, const Mat4& w, bool lit, float alpha = 1.f) { r_.draw(m, w, lit, alpha); }
    GpuMesh* extraModel(const std::string& name) { return model(name); }  // a .m model for drawExtra (cached)

private:
    int texture(const std::string& name);
    GpuMesh* model(const std::string& name);
    struct Rigged {
        Model model;
        std::vector<GpuMesh*> parts;  // one per frame (local space), nullptr for empty frames
        std::vector<std::unique_ptr<CollMesh>> coll;  // collision polygons per frame (local space), nullptr if none
        Vec3 centre;        // bounding sphere at rest (model space), FUN_10017ff0
        float radius = 0;
        int middle = -1;    // the middle mesh part (proxy +0x8c when the owner names none, FUN_100a79b0)
    };
    Rigged* rigged(const std::string& name);
    const std::vector<AnimSet>* animSets(const std::string& ecyName);
    struct Instance;
    // Walks a rigged instance's frames at its current pose: f(frame index, frame, world matrix, hidden branch).
    bool objectBox(int object, Vec3& lo, Vec3& hi) const;  // world box of the posed model
    void walkPose(const Instance& i, const std::function<void(int, const Frame&, const Mat4&, bool)>& f) const;
    void buildTerrain();
    // the pose of a rigged instance's frames, cached until the instance changes (collision queries)
    void posed(const Instance& i) const;
    bool segmentInstance(const Instance& i, const Vec3& a, const Vec3& b, SegmentHit& hit, int object) const;
    void instanceBox(const Instance& i, Vec3& lo, Vec3& hi) const;
    void gridInsert(int instance);
    void addBridge(int instance, const std::string& model);
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
    std::unordered_map<std::string, std::unique_ptr<CollMesh>> coll_;  // model-space collision polygons per model
    // static colliders by 32 m terrain tile (the original's per-tile proxy lists, FUN_100a2490), 128 x 128
    std::vector<std::vector<int>> grid_;
    std::vector<int> movers_;   // colliders placed again at run time (setObjectPose on a static model) and rigged ones
    mutable std::vector<uint32_t> seen_;
    mutable uint32_t seenStamp_ = 0;
    int playerObject_ = -1;
    std::vector<Bridge> bridges_;
    std::vector<WalkGraph> graphs_;
    void addWalkGraph(int instance, const std::string& file);
    std::map<int, std::unique_ptr<GenGraph>> genGraphs_;  // by object; null: none / unreadable
    std::vector<int> bridgeOf_;  // per instance: index into bridges_, or -1
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
        const CollMesh* coll = nullptr;  // static collision polygons (nullptr: none - rigged, NoColl, markers)
        bool collides = false; // in the collision world (object [5] bit 0x40000; rigged models use their parts)
        bool unit = false;     // a unit class: collides with other units only in pairs with the player
        bool mover = false;    // a static model placed again at run time: kept out of the tile grid
        Vec3 wlo, whi;         // world box of a static collider (FUN_100a6240)
        mutable bool poseDirty = true;
        mutable std::vector<Mat4> pose;        // per frame, world
        mutable std::vector<char> poseHidden;
        bool hidden = false;
        bool noDraw = false;   // still collides / takes hits (player in cockpit view)
        float twist = 0;
        float pitch = 0;  // turret gun elevation (frm-xa), radians up
        int overlaySet = -1;
        float overlayFrame = 0;
        int fadeSet = -1;      // set being faded out (walk <-> run), its key time and remaining weight
        float fadeFrame = 0, fadeWeight = 0;
        std::string overlayRoot;  // lower case
        std::vector<std::string> hiddenFrames;
        std::vector<std::pair<std::string, float>> damagedFrames;  // lower-case frame name, strength
        int damageTex = -1;  // blown-off parts (lower-case frame names; children go too)
        Mat4 inv;              // world -> model (rigid inverse of world)
    };
    std::vector<Instance> instances_;
};

}  // namespace hg2
