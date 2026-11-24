#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
namespace jc3vr {
struct HudValue { std::uint64_t previous,next,interface;std::uint32_t type,padding;std::uint64_t value,aux; };
static_assert(sizeof(HudValue)==0x30);
inline bool HudNumber(const HudValue& value,double& out) {
    switch(value.type & 0x8f) {
    case 3:out=static_cast<std::int32_t>(value.value);break;
    case 4:out=static_cast<std::uint32_t>(value.value);break;
    case 5:std::memcpy(&out,&value.value,sizeof(out));break;
    default:return false;
    }
    return std::isfinite(out);
}
struct HudSnapshot { int magazine=-1,reserve=-1,capacity=-1;float health=-1;std::uint64_t revision=0; };
// Independent from rendering. Native argument types verified in the installed
// executable; health meaning and initial selection values require live checks.
// Unexpected argument layouts are left unavailable.
inline bool ApplyHudEvent(HudSnapshot& state,std::string_view method,const HudValue* args,unsigned count) {
    const auto suffix=method.find_last_of('.');if(suffix!=method.npos)method.remove_prefix(suffix+1);
    double number=0;
    if(method=="OnSelectWeapon") {
        state.magazine=state.reserve=state.capacity=-1;
        // Installed native producer calls GetMagazineSize(958690) for arg2.
        // This is ONLY capacity. Remaining rounds still come from their event.
        if(count==9 && args && HudNumber(args[2],number) && number>=1 &&
           number<=99999 && std::floor(number)==number) state.capacity=static_cast<int>(number);
        ++state.revision;return true;
    }
    if(count && !args) return false;
    if(method=="OnSetSelectedMagazineAmmo" && count==2 && HudNumber(args[0],number) &&
       number>=0 && number<=99999 && std::floor(number)==number && (args[1].type&0x8f)==2) {
        // The second argument is a native display flag; do not reinterpret it
        // as unlimited ammunition until its meaning is independently proven.
        state.magazine=static_cast<int>(number);++state.revision;return true;
    }
    if(method=="OnSetSelectedInventoryAmmo" && count==1 && HudNumber(args[0],number) &&
       number>=0 && number<=99999 && std::floor(number)==number) {
        state.reserve=static_cast<int>(number);++state.revision;return true;
    }
    // Live UpdateHealth(800, 0.012487) disproves treating argument 1 as a
    // normalized health fraction. It is consistent with frame delta time.
    // Argument 0's maximum is not validated either: do not invent a percent.
    // Keep the bounded trace, but leave health unavailable until semantics are
    // independently established from the actual HUD consumer.
    return false;
}
bool InstallHudSnapshotHook();
void ConfigureVrDamageSuppression(bool enabled);
void PublishHudVrFrame(bool active);
bool InstallVrDamageHooks();
inline bool SuppressVrDamage(bool enabled,bool fresh,bool local) {return enabled && fresh && local;}
// Installed PlayerDamageEffect::Apply reads only this input intensity at
// +0x140, then writes its own state before its native zero-effect branch.
// Use a private copy; never change the shared gameplay parameter block.
struct DamagePresentationParameters {unsigned char prefix[0x140];float intensity;};
static_assert(offsetof(DamagePresentationParameters,intensity)==0x140);
static_assert(sizeof(DamagePresentationParameters)==0x144);
inline DamagePresentationParameters ZeroDamagePresentation(DamagePresentationParameters copy) {
    copy.intensity=0.0f;
    return copy;
}
inline bool SuppressHudDamageVisual(std::string_view leaf,bool enabled,bool fresh,bool local) {
    // Installed HUD ABC: UpdateHealth drives only the local damage paint /
    // flare; character damage indicators drive incoming direction wedges.
    // Do not broadly match Health/Damage: enemy/mission bars and hit feedback
    // must keep working. Health simulation and heartbeat audio are not here.
    return (leaf=="OnOmniDamage" || leaf=="UpdateHealth" || leaf=="UpdateCharacterDmgIndicators") &&
        SuppressVrDamage(enabled,fresh,local);
}
HudSnapshot GetHudSnapshot();
}
