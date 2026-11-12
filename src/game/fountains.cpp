#include "fountains.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace hg2 {

void FountainSystem::init(const Mission& m) {
    templates_ = m.fountainTemplates;
    for (auto& f : m.fountains) fountains_.push_back({f, {}, 0, 2.f});
    if (!fountains_.empty()) SDL_Log("fountains: %zu generators, %zu unit templates", fountains_.size(), templates_.size());
}

void FountainSystem::activate(const std::string& name, bool on) {
    for (auto& s : fountains_)
        if (!s.f.name.empty() && SDL_strcasecmp(s.f.name.c_str(), name.c_str()) == 0) s.f.active = on, s.cooldown = 0;
}

void FountainSystem::update(float dt, const Vec3& user,
                            const std::function<int(const FountainTemplate&, const Fountain&, int)>& spawn,
                            const std::function<bool(int)>& alive) {
    for (auto& s : fountains_) {
        s.units.erase(std::remove_if(s.units.begin(), s.units.end(), [&](int o) { return !alive(o); }), s.units.end());
        if (!s.f.active || s.f.templates.empty()) continue;
        if (s.f.total > 0 && s.made >= s.f.total) continue;
        if ((s.cooldown -= dt) > 0) continue;
        // mode 0 runs regardless of the user; 1 and 2 wait for the user to come within the trigger range
        float dx = user.x - s.f.pos[0], dz = user.z - s.f.pos[2];
        if (s.f.mode != 0 && dx * dx + dz * dz > s.f.range[1] * s.f.range[1]) continue;
        int room = s.f.maxAlive - int(s.units.size());
        for (int k = 0; k < std::min(room, s.f.wave); k++) {
            int ti = s.f.templates[size_t(s.made) % s.f.templates.size()];
            if (ti < 0 || size_t(ti) >= templates_.size()) break;
            int obj = spawn(templates_[size_t(ti)], s.f, k);
            if (obj < 0) break;
            s.units.push_back(obj);
            s.made++, total_++;
            if (s.f.total > 0 && s.made >= s.f.total) break;
        }
        s.cooldown = 10.f;  // next wave once the field has room again
    }
}

}  // namespace hg2
