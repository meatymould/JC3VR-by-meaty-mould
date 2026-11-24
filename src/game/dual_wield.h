#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace jc3vr {
// Precision-aim/zoom availability is deliberately NOT a firing permission.
// Only mechanics whose shared clocks we cannot safely split are excluded.
inline bool SupportedDualFireMechanics(bool heat,bool forced,bool alternate,float forceTimer,int forceCount,float charge) {
    return !heat && !forced && !alternate && std::isfinite(forceTimer) && forceTimer==0 &&
        // Native constructor +976FAA writes -1 (unset); +992D88 tests >0,
        // not !=0. Accept only the verified idle sentinel and exhausted zero.
        (forceCount==-1 || forceCount==0) && std::isfinite(charge) && charge==0;
}
class DualGunPlacements {
public:
    void Record(std::uintptr_t data,int hand,std::uint64_t now) {
        if(!data || hand<0 || hand>1) return;
        auto* destination=&entries_[0];
        for(auto& entry:entries_) {
            if(entry.data==data) {destination=&entry;break;}
            if(entry.tick<destination->tick) destination=&entry;
        }
        *destination={data,hand,now};
    }
    int HandFor(std::uintptr_t data,std::uint64_t now) const {
        if(!data) return -1;
        for(const auto& entry:entries_)
            if(entry.data==data && now>=entry.tick && now-entry.tick<=250) return entry.hand;
        return -1;
    }
private:
    struct Entry {std::uintptr_t data=0;int hand=-1;std::uint64_t tick=0;};
    std::array<Entry,32> entries_{};
};
// Native transient trigger fields only. Ammo, reload, heat, tuning and gun
// component ownership stay in JC3; these are not cloned WeaponBase objects.
struct DualFireState {
    std::uint8_t flags[7]{}; // native +158..15E
    float blocked=0,buffer=0,pullTime=0,sinceShot=0,firingTime=0;
};
inline bool SaneDualFireState(const DualFireState& s) {
    for(auto flag:s.flags) if(flag>1) return false;
    // +992EE6..+992F00 subtracts dt when +174 is positive, WITHOUT a
    // zero clamp. A slightly negative expired timer is valid native state.
    // Preserve it verbatim: changing it is not necessary to allow firing.
    if(!std::isfinite(s.blocked) || s.blocked< -3600 || s.blocked>3600) return false;
    for(float time:{s.buffer,s.pullTime,s.sinceShot,s.firingTime})
        if(!std::isfinite(time) || time< -0.001f || time>3600) return false;
    return true;
}
inline void AdvanceDualFireClock(DualFireState& s,float dt) {
    s.sinceShot=std::fmin(s.sinceShot+dt,3599.0f);
    s.pullTime=std::fmin(s.pullTime+dt,3599.0f);
}
inline float PermittedDualTrigger(float hand,float native) {
    return std::isfinite(hand) && std::isfinite(native) && native>0?
        std::fmax(0.0f,std::fmin(hand,native)):0;
}
inline DualFireState CombinedDualFireState(const DualFireState& a,const DualFireState& b) {
    DualFireState out=a;
    for(int i=0;i<7;++i) out.flags[i]=a.flags[i] || b.flags[i];
    // Cancellation is scoped to its hand, not a reason to cancel both.
    out.flags[4]=a.flags[4] && b.flags[4];
    out.sinceShot=std::fmin(a.sinceShot,b.sinceShot);
    out.pullTime=std::fmin(a.pullTime,b.pullTime);
    out.firingTime=std::fmax(a.firingTime,b.firingTime);
    out.buffer=std::fmax(a.buffer,b.buffer);
    out.blocked=std::fmax(a.blocked,b.blocked);
    return out;
}
bool InstallDualWieldHooks();
std::uintptr_t GetDualWieldWeapon();
void RecordDualGunPlacement(std::uintptr_t data,int hand);
void RequestDualFire(std::uintptr_t weapon,bool active,float left,float right);
void DualWieldPresentTick();
}
