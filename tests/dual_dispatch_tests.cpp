// Integration test of the ACTUAL implementation, not a copied dispatcher.
// Engine callbacks, tracking and objects below are explicitly synthetic. This
// cannot prove native hook entry, game ballistics, animations or headset hits.
#undef NDEBUG
#include "../src/game/dual_wield.cpp"
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

namespace lab {
alignas(16) std::array<std::byte,0xB00> character{};
alignas(16) std::array<std::byte,0x600> weapon{};
alignas(16) std::array<std::byte,0x350> gun[2]{};
std::uintptr_t components[2]{};
bool tracking=true,inVehicle=false;
int nativeCalls[2]{},projectiles[2]{},stops[2]{},ammo=40,throwOnHand=-1,failMuzzleHand=-1;
float direction[2][3]{{-1,0,0},{1,0,0}};
template<class T> void Put(std::uintptr_t address,const T& value) {
    std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(value));
}
template<class T> T Get(std::uintptr_t address) {
    T value{};std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value;
}
std::uintptr_t W() {return reinterpret_cast<std::uintptr_t>(weapon.data());}
int Index(void* data) {
    for(int i=0;i<2;++i) if(data==gun[i].data()) return i;
    assert(false);return -1;
}
float* __fastcall Muzzle(void* self,float* out,void* data) {
    assert(reinterpret_cast<std::uintptr_t>(self)==W());
    const int hand=Index(data);
    if(failMuzzleHand==hand) return nullptr;
    out[0]=hand?0.3f:-0.3f;out[1]=1.5f;out[2]=0;
    return out;
}
void __fastcall Stop(void*,void* data) {++stops[Index(data)];}
void __fastcall Projectile(void* self,void* data) {
    const int hand=Index(data);const auto w=reinterpret_cast<std::uintptr_t>(self);
    assert(jc3vr::scopedHand==hand);
    const auto cursor=Get<std::uint8_t>(w+0x160);
    assert(cursor<2 && components[cursor]==reinterpret_cast<std::uintptr_t>(data));
    assert(Get<int>(w+0x16C)==0);
    float muzzle[4]{};Muzzle(self,muzzle,data);
    const auto* aim=reinterpret_cast<const float*>(w+0x3FC);
    for(int c=0;c<3;++c) assert(std::abs(aim[c]-muzzle[c]-direction[hand][c]*1000)<0.001f);
    ++projectiles[hand];--ammo;
    // Emulate the native cursor advance to ensure next hand repins its cursor.
    Put<std::uint8_t>(w+0x160,static_cast<std::uint8_t>(1-cursor));
}
bool __fastcall Update(void* self,float dt,void* data,const float* input) {
    const int hand=Index(data);const auto w=reinterpret_cast<std::uintptr_t>(self);
    ++nativeCalls[hand];
    assert(jc3vr::scopedGun==reinterpret_cast<std::uintptr_t>(data) && jc3vr::scopedHand==hand);
    if(throwOnHand==hand) RaiseException(0xE0420002,0,0,nullptr);
    // Exact arithmetic of native +174 countdown (no clamp).
    float block=Get<float>(w+0x174);
    if(block>0) {Put(w+0x174,block-dt);return false;}
    const bool pulled=input[0]>0.5f;
    const bool wasFired=Get<std::uint8_t>(w+0x15E)!=0;
    Put<std::uint8_t>(w+0x15B,pulled);
    Put<std::uint8_t>(w+0x15A,pulled);
    if(!pulled) {
        Put<std::uint8_t>(w+0x15E,0);Put<std::uint8_t>(w+0x159,0);
        // Native release loops all components; production scope must filter.
        for(auto component:components) jc3vr::StopDetour(self,reinterpret_cast<void*>(component));
        return false;
    }
    if(!wasFired && ammo>0) {
        jc3vr::ProjectileDetour(self,data);
        Put<std::uint8_t>(w+0x15E,1);Put<std::uint8_t>(w+0x159,1);
        Put(w+0x198,0.0f);return true;
    }
    return false;
}
void Reset() {
    character={};weapon={};gun[0]={};gun[1]={};
    for(int h=0;h<2;++h) nativeCalls[h]=projectiles[h]=stops[h]=0;
    ammo=40;throwOnHand=failMuzzleHand=-1;tracking=true;inVehicle=false;
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    components[0]=reinterpret_cast<std::uintptr_t>(gun[0].data());
    components[1]=reinterpret_cast<std::uintptr_t>(gun[1].data());
    Put(reinterpret_cast<std::uintptr_t>(character.data())+0x998,W());
    Put(W(),image+0x23E3038);Put(W()+0x420,0);Put(W()+0x424,1);
    Put(W()+0x180,-1);Put(W()+0x16C,-1);
    Put(W()+0x568,reinterpret_cast<std::uintptr_t>(components));
    Put(W()+0x570,reinterpret_cast<std::uintptr_t>(components+2));
    const float originalAim[3]{10,20,30};std::memcpy(weapon.data()+0x3FC,originalAim,sizeof(originalAim));
    Put(W()+0x198,100.0f);
    jc3vr::cache={};jc3vr::placements={};jc3vr::fault=false;jc3vr::installed=true;
    jc3vr::scopedWeapon=jc3vr::scopedGun=0;jc3vr::scopedHand=-1;
    jc3vr::nativeFireFrom=&Muzzle;jc3vr::originalUpdate=&Update;
    jc3vr::originalProjectile=&Projectile;jc3vr::originalStop=&Stop;
    for(int h=0;h<2;++h) {
        Put(components[h],image+0x23F4750);Put(components[h]+0xC0,W());
        jc3vr::RecordDualGunPlacement(components[h],h);
    }
}
bool Tick(float left,float right,float permission=1) {
    assert(jc3vr::GetDualWieldWeapon()==W());
    jc3vr::RequestDualFire(W(),true,left,right);
    jc3vr::Pair pair{};assert(jc3vr::ReadPair(W(),pair));
    jc3vr::Request wanted{};
    {std::lock_guard lock(jc3vr::inputMutex);wanted=jc3vr::request;}
    assert(wanted.active && wanted.weapon==W() && jc3vr::requestedWeapon.load()==W());
    const float native[2]{permission,0};
    const bool result=jc3vr::RunPair(pair,wanted,1.f/30,native);
    assert(Get<int>(W()+0x16C)==-1 && Get<std::uint8_t>(W()+0x160)==0);
    const float originalAim[3]{10,20,30};
    assert(!std::memcmp(weapon.data()+0x3FC,originalAim,sizeof(originalAim)));
    assert(jc3vr::scopedWeapon==0 && jc3vr::scopedGun==0 && jc3vr::scopedHand==-1);
    return result;
}
void IndependentPulls() {
    Reset();assert(Tick(0,1));assert(projectiles[0]==0 && projectiles[1]==1);
    Tick(0,1);assert(projectiles[1]==1); // held semi-auto must not refire
    assert(Tick(1,1));assert(projectiles[0]==1 && projectiles[1]==1);
    Tick(1,0);assert(Tick(1,1));assert(projectiles[0]==1 && projectiles[1]==2);
    Tick(0,0);assert(Tick(1,1));assert(projectiles[0]==2 && projectiles[1]==3 && ammo==35);
    assert(nativeCalls[0]==nativeCalls[1] && !jc3vr::fault.load());
}
void PermissionsAndExpiry() {
    Reset();assert(!Tick(1,1,0));assert(ammo==40);
    // +188 is a scheduled-fire override veto, not a proven reload flag.
    Put<std::uint8_t>(W()+0x188,1);assert(!Tick(1,1));assert(ammo==40);
    Put<std::uint8_t>(W()+0x188,0);assert(Tick(1,1));assert(ammo==38);
    Reset();Put(W()+0x174,0.001f);
    assert(!Tick(1,1)); // native writes -.032333... on BOTH hands
    assert(!jc3vr::fault.load());assert(Tick(1,1));assert(ammo==38);
}
void IdentityAndTracking() {
    Reset();jc3vr::Pair pair{};assert(jc3vr::ReadPair(W(),pair));
    tracking=false;assert(!jc3vr::ReadPair(W(),pair));tracking=true;
    inVehicle=true;assert(!jc3vr::ReadPair(W(),pair));inVehicle=false;
    Put(components[1]+0xC0,std::uintptr_t{0});assert(!jc3vr::ReadPair(W(),pair));
    Put(components[1]+0xC0,W());jc3vr::RecordDualGunPlacement(components[1],0);
    assert(!jc3vr::ReadPair(W(),pair));
    jc3vr::RecordDualGunPlacement(components[1],1);
    Put(W()+0x420,1);assert(jc3vr::GetDualWieldWeapon()==0); // non-dual slot
    jc3vr::RequestDualFire(0,false,1,1);assert(jc3vr::requestedWeapon.load()==0);
}
void FaultDoesNotRetry() {
    Reset();throwOnHand=1;
    Tick(1,1);assert(jc3vr::fault.load());
    // Left already fired; right failed. Never retry left or restore spent ammo.
    assert(projectiles[0]==1 && projectiles[1]==0 && ammo==39);
    Reset();failMuzzleHand=1;
    Tick(1,1);assert(jc3vr::fault.load());
    assert(projectiles[0]==1 && projectiles[1]==0 && ammo==39);
}
void ReversedComponentOrder() {
    Reset();std::swap(components[0],components[1]);
    assert(Tick(1,0));assert(projectiles[0]==1 && projectiles[1]==0);
    assert(Tick(0,1));assert(projectiles[0]==1 && projectiles[1]==1);
    assert(!jc3vr::fault.load());
}
void RepeatedPullRelease() {
    Reset();ammo=1000;
    for(int i=0;i<100;++i) {
        assert(Tick(1,1));Tick(1,1);Tick(0,0);
    }
    assert(projectiles[0]==100 && projectiles[1]==100 && ammo==800);
    assert(!jc3vr::fault.load());
}
}
namespace jc3vr {
std::uintptr_t VrBodyCharacter() {return reinterpret_cast<std::uintptr_t>(lab::character.data());}
VehicleCameraAnchor GetVehicleCameraAnchor(bool) {VehicleCameraAnchor a;a.valid=lab::inVehicle;return a;}
bool GetVrBodyHandTarget(int hand,float grip[3],float fingers[3],float down[3]) {
    if(!lab::tracking || hand<0 || hand>1) return false;
    grip[0]=hand?0.3f:-0.3f;grip[1]=1.5f;grip[2]=0;
    std::memcpy(fingers,lab::direction[hand],12);down[0]=0;down[1]=-1;down[2]=0;return true;
}
bool VrHapticsEnabled() {return false;}
void RequestVrHapticPulse(unsigned,float) {}
void TraceRuntime(std::string_view) {}
}
int main() {
    lab::IndependentPulls();lab::PermissionsAndExpiry();lab::IdentityAndTracking();lab::FaultDoesNotRetry();
    lab::ReversedComponentOrder();lab::RepeatedPullRelease();
    std::cout<<"Actual dual dispatcher passed synthetic native-consumer contracts; NOT a JC3 hit test.\n";
}
