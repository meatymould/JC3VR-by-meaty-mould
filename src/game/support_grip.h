#pragma once
#include <cmath>
#include <cstdint>
namespace jc3vr {
struct SupportGripProfile {
    bool eligible=false, nativePrecision=false;
    float minimum=0.12f, maximum=0.50f, radius=0.10f;
};
// Primary (offset-zero) RTTI tables from the fingerprinted executable.
// Physical support does not depend on a weapon's precision/zoom override.
inline SupportGripProfile SupportProfileForTable(std::uintptr_t rva) {
    switch(rva) {
    case 0x23E3038: return {true,true,0.12f,0.50f,0.10f}; // bullet rifle
    case 0x23F0A48: return {true,false,0.12f,0.65f,0.10f}; // sniper: physical lens, no fullscreen zoom
    case 0x23F9D18: return {true,true,0.12f,0.70f,0.12f}; // preserve existing RPG precision
    case 0x23FB318: case 0x23FB5E8: case 0x23FBA68:
    case 0x2418F48: // RPG / Bloodhound / Fire Leach / Javelin / NoSpawn
    case 0x23F8948: case 0x23F64D8: case 0x23F6028: case 0x23F5BA8:
        return {true,false,0.12f,0.70f,0.12f}; // missile / Typhoon / Sungun / lightning
    default: return {};
    }
}
// All positions are from one XR frame in LOCAL metres, never game/world units.
struct SupportGripInput {
    bool eligible=false, tracking=false, gameplay=false;
    std::uintptr_t weapon=0;
    float squeeze=0;
    float left[3]{}, right[3]{}, forward[3]{};
    float minimum=0.12f, maximum=0.50f, radius=0.10f;
};
struct SupportGripState {
    bool held=false, squeezeDown=false, consumed=false;
    std::uintptr_t weapon=0;
    float socketDistance=0.30f;
};
struct SupportGripResult { bool precision=false, consumeGrapple=false; };
inline SupportGripResult UpdateSupportGrip(SupportGripState& s,const SupportGripInput& in) {
    const bool down=std::isfinite(in.squeeze) && in.squeeze>(s.squeezeDown?0.35f:0.55f);
    float norm=0, along=0, distance=0;
    bool finite=true;
    for (int i=0;i<3;++i) {
        finite=finite && std::isfinite(in.left[i]) && std::isfinite(in.right[i]) && std::isfinite(in.forward[i]);
        norm+=in.forward[i]*in.forward[i];
        const float delta=in.left[i]-in.right[i];
        along+=delta*in.forward[i]; distance+=delta*delta;
    }
    if (norm>0.5f && norm<1.5f) along/=std::sqrt(norm); else finite=false;
    const float radial=distance-along*along;
    const bool profileValid=std::isfinite(in.minimum) && std::isfinite(in.maximum) && std::isfinite(in.radius) &&
        in.minimum>=0.06f && in.maximum>=in.minimum && in.maximum<=0.75f && in.radius>0 && in.radius<=0.15f;
    const bool allowed=in.eligible && in.weapon && in.tracking && in.gameplay && finite && profileValid;
    // Initial fore-end interaction capsule; authored per-weapon sockets can
    // replace these bounds later without changing input or precision ownership.
    const bool inside=along>=in.minimum && along<=in.maximum && radial<=in.radius*in.radius;
    // Once grabbed, follow the support hand even if it rotates away from the
    // original one-hand aim cone. Otherwise angling a two-handed gun detaches it.
    const bool retained=distance>=0.06f*0.06f && distance<=0.85f*0.85f;
    if (!down) s.held=s.consumed=false;
    if (s.held && (!allowed || !retained || in.weapon!=s.weapon)) s.held=false;
    if (!s.squeezeDown && down && allowed && inside) {
        s.held=s.consumed=true; s.weapon=in.weapon;
        s.socketDistance=std::sqrt(distance);
    }
    // Once a squeeze was a gun grab, leaving the region must not fire grapple.
    // Tracking loss releases precision but keeps that press consumed to release.
    s.squeezeDown=down;
    return {s.held,s.consumed && down};
}
inline bool SupportGripFrame(const SupportGripState& s,const float left[3],const float right[3],float forward[3],float socket[3]) {
    if (!s.held || !std::isfinite(s.socketDistance) || s.socketDistance<0.06f || s.socketDistance>0.80f) return false;
    float length=0;
    for(int i=0;i<3;++i) {
        if(!std::isfinite(left[i]) || !std::isfinite(right[i])) return false;
        forward[i]=left[i]-right[i]; length+=forward[i]*forward[i];
    }
    if(length<0.06f*0.06f || length>0.85f*0.85f) return false;
    length=std::sqrt(length);
    for(int i=0;i<3;++i) { forward[i]/=length; socket[i]=right[i]+forward[i]*s.socketDistance; }
    return true;
}
}
