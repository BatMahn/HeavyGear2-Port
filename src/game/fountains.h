// FountainSystem (mission block 0x12a): unit generators that spawn reinforcements from unit templates when
// they are active and the user's gear comes within range. ActivateFountain(name, on) toggles them by name.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../assets/mission.h"
#include "../core/math.h"

namespace hg2 {

class FountainSystem {
public:
    void init(const Mission& m);
    // spawn(template index, fountain) -> new object index or -1; alive(object) -> still fighting
    void update(float dt, const Vec3& user, const std::function<int(const FountainTemplate&, const Fountain&, int n)>& spawn,
                const std::function<bool(int)>& alive);
    void activate(const std::string& name, bool on);
    int spawned() const { return total_; }

private:
    struct State { Fountain f; std::vector<int> units; int made = 0; float cooldown = 0; };
    std::vector<State> fountains_;
    std::vector<FountainTemplate> templates_;
    int total_ = 0;
};

}  // namespace hg2
