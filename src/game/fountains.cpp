#include "fountains.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace hg2 {

namespace {
// the unit classes FUN_1005cdb0 can build a pool of: Biped, Turret, Jeep, Hover, Infantry, Mount, ChickenWalker,
// Frame, Plane; any other template type gets no pool and never spawns
bool poolable(int type) {
    switch (type) {
        case 0x48: case 0x4b: case 0x4e: case 0x4f: case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: return true;
        default: return false;
    }
}
// FUN_100b5ce0: the larger of |dx|, |dy|, |dz|
float cheb(const Vec3& a, const Vec3& b) { return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)}); }
}  // namespace

void FountainSystem::init(const Mission& m) {
    templates_ = m.fountainTemplates;
    on_ = m.fountainsOn, pool_ = std::max(0, m.fountainPool), cap_ = std::max(0, m.fountainCap);
    for (auto& f : m.fountains) gens_.push_back({f});
    // FUN_1005cdb0: every template a generator names gets a pool of +0x1c units
    pools_.assign(templates_.size(), {});
    for (auto& g : gens_)
        for (int t : g.f.templates)
            if (t >= 0 && size_t(t) < templates_.size() && poolable(templates_[size_t(t)].type) && pools_[size_t(t)].empty())
                pools_[size_t(t)].assign(size_t(pool_), Slot{});
    heap_.assign(1, {1e30f, -1});  // slot 0: the sentinel 0x7149f2ca
    if (getenv("HG2_FOUNTDBG"))
        for (size_t t = 0; t < templates_.size(); t++) SDL_Log("fountain: template %zu type 0x%x pool %zu", t, templates_[t].type, pools_[t].size());
    if (!gens_.empty())
        SDL_Log("fountains: %zu generators, %zu unit templates, system %s, pool %d, cap %d", gens_.size(), templates_.size(),
                on_ ? "on" : "off", pool_, cap_);
}

void FountainSystem::activate(const std::string& name, bool on) {
    // FUN_1005e2b0: only the generator's active flag (+0x14); HG2_FOUNTALL=1 (test) keeps every generator on
    if (!on && getenv("HG2_FOUNTALL")) return;
    for (auto& g : gens_)
        if (!g.f.name.empty() && SDL_strcasecmp(g.f.name.c_str(), name.c_str()) == 0) g.f.active = on;
}

bool FountainSystem::fromFountain(int object) const {
    for (auto& l : live_)
        if (pools_[size_t(l.tpl)][size_t(l.slot)].object == object) return true;
    return false;
}

Vec3 FountainSystem::spawnOffset(float radius) {
    float a = 3.14159265f * 2.f * (0.5f - float(rand() % 32768) / 32767.f);
    float s = std::sin(a), c = std::cos(a);
    return Vec3(-s * radius, 0, (c - s * s) * radius);
}

float FountainSystem::nearest(const Vec3& p, const Vec3& user, const std::vector<Vec3>& squad) {
    // FUN_100d1320: the user's squad, leader first; the original stops early once a member is inside its threshold,
    // which never changes the comparison its callers make
    float best = cheb(user, p);
    for (auto& s : squad) best = std::min(best, cheb(s, p));
    return best;
}

void FountainSystem::update(float dt, const Vec3& user, const Vec3& userBack, const std::vector<Vec3>& squad, const Host& host) {
    if (!on_ || gens_.empty()) return;
    if (!bound_) {
        // FUN_1005dfc0, the first frame the system runs: every generator looks up the graph node it stands on
        // (FUN_1005c050 -> FUN_100c1da0 with its position +0xb0) and marks it taken (+0x20)
        bound_ = true;
        for (auto& g : gens_) {
            int graph = -1, node = -1;
            if (!host.findNode || !host.findNode(Vec3(g.f.pos[0], g.f.pos[1], g.f.pos[2]), graph, node)) continue;
            g.graph = graph, g.node = node;
            if (host.takeNode) host.takeNode(graph, node);
            if (getenv("HG2_FOUNTDBG"))
                SDL_Log("fountain: generator %s (%s, range %.0f..%.0f) at %.0f,%.0f,%.0f on graph %d node %d", g.f.name.c_str(),
                        g.f.active ? "on" : "off", g.f.range[0], g.f.range[1], g.f.pos[0], g.f.pos[1], g.f.pos[2], graph, node);
        }
    }
    timer_ -= dt;
    for (auto& g : gens_)  // the generators that spawned lately count their 3 s down (+0x34 list)
        if (g.cooling && (g.timer -= dt) < 0) g.cooling = false;
    switch (state_) {
        case 0: if (timer_ < 0) state_ = 1, timer_ = 0; break;
        case 1: cull(user, squad, host), state_ = 2, timer_ = 1.f; break;
        case 2: if (timer_ < 0) state_ = 3, timer_ = 0; break;
        case 3: score(user, userBack, squad, host), state_ = 4, timer_ = 0.2f; break;
        case 4: if (timer_ < 0) state_ = 5, timer_ = 0; break;
        case 5: spawn(host), state_ = 0, timer_ = 1.5f; break;
        default: state_ = 0, timer_ = 1.5f; break;
    }
}

void FountainSystem::cull(const Vec3& user, const std::vector<Vec3>& squad, const Host& host) {
    std::vector<Live> keep;
    for (auto& l : live_) {
        Slot& s = pools_[size_t(l.tpl)][size_t(l.slot)];
        Gen* g = l.gen >= 0 ? &gens_[size_t(l.gen)] : nullptr;
        float range = g ? g->f.range[2] : 580.f;
        bool out = false;
        if (!host.alive(s.object)) {
            // lost: counted against the system and the generator, which switches off once it has lost `total`
            lost_++;
            if (g) {
                g->lost++;
                if (g->alive > 0) g->alive--;
                if (g->f.total > 0 && g->lost >= g->f.total) g->f.active = false;
            }
            out = true;
        } else if ((!g || g->f.active) && !host.inCombat(s.object)) {
            // out of the fight and beyond the generator's recycle range of the user and the squad: back in the pool
            Vec3 p;
            if (host.position(s.object, p) && range < nearest(p, user, squad)) {
                host.recycle(s.object);
                if (g && g->alive > 0) g->alive--;
                out = true;
            }
        }
        if (out) alive_--, s.free = true;
        else keep.push_back(l);
    }
    live_.swap(keep);
}

void FountainSystem::score(const Vec3& user, const Vec3& back, const std::vector<Vec3>& squad, const Host& host) {
    int room = pool_ - alive_;
    // the queue is only rebuilt while the system is under its limits: past the cap the last round's queue stays,
    // and the spawn step goes through it again (the original's behaviour)
    if (!((pool_ == 0 || alive_ < pool_) && (cap_ == 0 || lost_ + alive_ < cap_))) return;
    heap_.resize(1);
    for (size_t gi = 0; gi < gens_.size(); gi++) {
        Gen& g = gens_[gi];
        if (!g.f.active || (g.f.maxAlive != 0 && g.alive >= g.f.maxAlive) || room < g.f.wave || g.timer >= 0) continue;
        Vec3 gp(g.f.pos[0], g.f.pos[1], g.f.pos[2]);
        if (host.inView && host.inView(gp, 4.f)) continue;  // FUN_1005c7d0: never where the camera can see
        float d = nearest(gp, user, squad);
        if (!(d < g.f.range[1] && (!g.f.minRange || g.f.range[0] < d))) continue;
        // FUN_1005d9d0: distance plus 70 x how far the generator lies behind the user (its bearing on the gear's
        // back axis); the max-heap takes them roughly highest first
        float dx = gp.x - user.x, dz = gp.z - user.z, n = std::sqrt(dx * dx + dz * dz);
        float k = n > 1e-6f ? 1.f / n : 0.f;
        float sc = (0.f * back.y + k * dz * back.z + k * dx * back.x) * 70.f + d;
        heap_.push_back({sc, int(gi)});
        size_t i = heap_.size() - 1;
        auto item = heap_[i];
        while (!(item.first < heap_[i >> 1].first)) heap_[i] = heap_[i >> 1], i >>= 1;
        heap_[i] = item;
    }
}

void FountainSystem::spawn(const Host& host) {
    int room = pool_ - alive_;
    for (size_t i = 1; i < heap_.size(); i++) {
        if (room < 1) return;
        Gen& g = gens_[size_t(heap_[i].second)];
        int made = 0;
        if (g.f.wave <= room && !templates_.empty() && !g.f.templates.empty()) {
            if (int(g.f.templates.size()) <= g.cursor) g.cursor = 0;
            made = fromPool(g.f.templates[size_t(g.cursor)], heap_[i].second, host);
            g.cursor++;
        }
        alive_ += made;
        g.timer = 3.f, g.cooling = true;
        room = pool_ - alive_;
    }
}

int FountainSystem::fromPool(int tpl, int gen, const Host& host) {
    if (tpl < 0 || size_t(tpl) >= pools_.size() || pools_[size_t(tpl)].empty()) return 0;
    auto& pool = pools_[size_t(tpl)];
    Gen& g = gens_[size_t(gen)];
    int free = int(std::count_if(pool.begin(), pool.end(), [](const Slot& s) { return s.free; }));
    if (free < g.f.wave) return 0;  // the whole wave or nothing
    int made = 0;
    for (size_t k = 0; k < pool.size() && made < g.f.wave; k++) {
        if (!pool[k].free) continue;
        int obj = host.spawn(tpl, g.f, pool[k].object);
        if (obj < 0) break;
        pool[k].object = obj, pool[k].free = false;
        if (g.graph >= 0 && g.node >= 0 && host.onGraph) host.onGraph(obj, g.graph, g.node);
        g.alive++;
        live_.push_back({tpl, int(k), gen});
        made++, total_++;
    }
    return made;
}

}  // namespace hg2
