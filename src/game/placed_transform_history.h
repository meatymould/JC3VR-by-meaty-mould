#pragma once
#include "game/weapon_pose.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace jc3vr {
// Recent successfully placed transforms only. Retained eye uploads can lag
// the newest simulation attachment; never widen the ownership tolerance.
struct PlacedTransformHistory {
    struct Entry { float matrix[16]{}; std::uint64_t tick = 0; };
    std::array<Entry, 16> entries{};
    unsigned next = 0;
    void Publish(const float matrix[16], std::uint64_t tick) {
        for (auto& entry : entries) {
            if (entry.tick && MatchesPlacedWeapon(matrix, entry.matrix)) {
                entry.tick = tick;
                return;
            }
        }
        auto& entry = entries[next];
        std::memcpy(entry.matrix, matrix, sizeof(entry.matrix));
        entry.tick = tick;
        next = (next+1)%entries.size();
    }
    bool Contains(const float matrix[16], std::uint64_t now) const {
        for (const auto& entry : entries)
            if (entry.tick && now >= entry.tick && now-entry.tick < 200 && MatchesPlacedWeapon(matrix, entry.matrix)) return true;
        return false;
    }
};
}
