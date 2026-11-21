// FountainSystem (mission block 0x12a): unit generators that bring reinforcements in from pools of units, the
// original's FUN_1005dfc0 state machine. See docs/FORMATS.md "Unit generators".
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../assets/mission.h"
#include "../core/math.h"

namespace hg2 {

class FountainSystem {
public:
    // What the system needs from the session.
    struct Host {
        // Bring template `tpl`'s pool unit into the field for generator `f`: `reuse` is the object that unit had
        // before (-1 the first time); returns the object, or -1 when it can't be made.
        std::function<int(int tpl, const Fountain& f, int reuse)> spawn;
        std::function<void(int object)> recycle;            // out of the field and back in the pool (vtable +8)
        std::function<bool(int object)> alive;
        std::function<bool(int object, Vec3& out)> position;
        std::function<bool(int object)> inCombat;           // brain mode 1 (FUN_100b7c60)
        std::function<bool(const Vec3& p, float radius)> inView;  // the camera's view volume (FUN_100056f0)
        // FUN_100c1da0: the walkable-graph node a generator sits on (object with the graph, node index)
        std::function<bool(const Vec3& p, int& graph, int& node)> findNode;
        std::function<void(int graph, int node)> takeNode;  // the node's +0x20 flag
        // FUN_1005c4e0 / FUN_1005c110: a unit spawned at a generator on a graph node remembers the graph and the node
        // (brain +0x1b4 / +0x1b8) and is ordered toward that node's first link
        std::function<void(int object, int graph, int node)> onGraph;
    };

    void init(const Mission& m);
    // user: the user's position and its matrix's third row (the back of the gear); squad: the user's living mates
    void update(float dt, const Vec3& user, const Vec3& userBack, const std::vector<Vec3>& squad, const Host& host);
    void activate(const std::string& name, bool on);
    int spawned() const { return total_; }
    bool fromFountain(int object) const;
    // c4e0's random offset from the generator: a direction from FUN_100b6db0 (angle in -pi..pi) at the unit's
    // bounding radius, with that routine's own z term
    static Vec3 spawnOffset(float radius);

private:
    struct Gen {
        Fountain f;
        int alive = 0;       // +0x1c
        int lost = 0;        // +0x20
        float timer = -1.f;  // +0x24
        int cursor = 0;      // +0x30 next template
        bool cooling = false;
        int graph = -1, node = -1;  // +0x34 the walkable graph (its object), +0x38 the node it sits on
    };
    struct Slot { int object = -1; bool free = true; };
    struct Live { int tpl, slot, gen; };

    void cull(const Vec3& user, const std::vector<Vec3>& squad, const Host& host);       // FUN_1005d610
    void score(const Vec3& user, const Vec3& back, const std::vector<Vec3>& squad, const Host& host);  // FUN_1005dac0
    void spawn(const Host& host);                                                        // FUN_1005def0
    int fromPool(int tpl, int gen, const Host& host);                                    // FUN_1005c720
    static float nearest(const Vec3& p, const Vec3& user, const std::vector<Vec3>& squad);

    std::vector<Gen> gens_;
    std::vector<FountainTemplate> templates_;
    std::vector<std::vector<Slot>> pools_;  // per template, built for the templates a generator names
    std::vector<Live> live_;                // +0x58
    std::vector<std::pair<float, int>> heap_;  // +0x68: 1-based max-heap of (score, generator)
    bool on_ = false;                       // +0x15
    int pool_ = 4, cap_ = 0;                // +0x1c, +0x18
    int alive_ = 0, lost_ = 0;              // +0x20, +0x24
    int state_ = 5;                         // +0x10
    bool bound_ = false;                    // +0x14: the generators have looked up their graph nodes
    float timer_ = 0;                       // +0xc
    int total_ = 0;
};

}  // namespace hg2
