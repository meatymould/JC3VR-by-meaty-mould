#undef NDEBUG
#include "../src/game/weapon_attachment_probe.cpp"
#include <cassert>
#include <iostream>
namespace lab {
alignas(16) std::array<std::byte,0xB00> rico{};
alignas(16) std::array<std::byte,0x600> weapon{};
alignas(16) std::array<std::byte,0x350> gun{};
alignas(16) std::array<std::byte,0x240> animation{};
std::uintptr_t list[1]{};
int nativeCalls=0,reports=0;bool nativeMoves=false;
template<class T> void Put(std::uintptr_t p,const T& v){std::memcpy(reinterpret_cast<void*>(p),&v,sizeof(v));}
std::uintptr_t W(){return reinterpret_cast<std::uintptr_t>(weapon.data());}
std::uintptr_t D(){return reinterpret_cast<std::uintptr_t>(gun.data());}
void __fastcall Native(void* self,void*) {
    assert(reinterpret_cast<std::uintptr_t>(self)==W()+8);++nativeCalls;
    if(nativeMoves)Put(D()+0x198+48,42.f);
}
void Reset() {
    rico={};weapon={};gun={};animation={};nativeCalls=reports=0;nativeMoves=false;
    Put(reinterpret_cast<std::uintptr_t>(rico.data())+0x998,W());
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    Put(D(),base+0x23F4750);Put(D()+0xC0,W());Put(W()+0x424,1);
    list[0]=D();Put(W()+0x568,reinterpret_cast<std::uintptr_t>(list));
    Put(W()+0x570,reinterpret_cast<std::uintptr_t>(list+1));
    Put(D()+0x128,reinterpret_cast<std::uintptr_t>(animation.data()));
    Put(reinterpret_cast<std::uintptr_t>(animation.data())+0x200,std::uintptr_t{0x1234});
    for(int c:{0,5,10,15}) {Put(D()+0x158+c*4,1.f);Put(D()+0x198+c*4,1.f);}
    jc3vr::until=0;jc3vr::count=jc3vr::dropped=0;jc3vr::leases={};
    jc3vr::busy.clear();jc3vr::originalRender=&Native;
}
void ReadOnlyObservation() {
    Reset();const auto oldGun=gun;const auto oldWeapon=weapon;
    jc3vr::RenderDetour(reinterpret_cast<void*>(W()+8),nullptr);
    assert(nativeCalls==1 && jc3vr::count==0); // disabled fast path
    jc3vr::until=GetTickCount64()+8000;
    jc3vr::RenderDetour(reinterpret_cast<void*>(W()+8),nullptr);
    assert(nativeCalls==2 && jc3vr::count==1);
    assert(gun==oldGun && weapon==oldWeapon);
    assert(jc3vr::records[0].after.models[0]==0x1234);
    assert(jc3vr::records[0].after.data==D());
}
void NativeMutationNotRestored() {
    Reset();jc3vr::until=GetTickCount64()+8000;nativeMoves=true;
    jc3vr::RenderDetour(reinterpret_cast<void*>(W()+8),nullptr);
    assert(nativeCalls==1 && jc3vr::count==1);
    assert(jc3vr::records[0].before.t1[12]==0 && jc3vr::records[0].after.t1[12]==42);
    float actual=0;std::memcpy(&actual,gun.data()+0x198+48,4);assert(actual==42);
}
void PlacementMatrixHistory() {
    Reset();jc3vr::until=GetTickCount64()+8000;
    float native[32]{};native[12]=3.f;native[16+12]=4.f;
    jc3vr::ObserveWeaponAttachmentPlacement(D(),reinterpret_cast<std::uintptr_t>(rico.data()),2,native);
    assert(jc3vr::count==1);
    assert(jc3vr::records[0].before.t0[12]==3.f && jc3vr::records[0].before.t1[12]==4.f);
    assert(jc3vr::records[0].after.t0[12]==0 && jc3vr::records[0].after.t1[12]==0);
}
void BoundedAndSafe() {
    Reset();jc3vr::Snapshot s{};assert(!jc3vr::Take(0,s));assert(jc3vr::Take(D(),s));
    Put(D()+0xC0,std::uintptr_t{1});assert(!jc3vr::Take(D(),s));Put(D()+0xC0,W());
    Put(W()+0x424,3);assert(!jc3vr::Take(D(),s));Put(W()+0x424,1);
    jc3vr::until=GetTickCount64()+8000;
    for(unsigned i=0;i<200;++i) {s.data=100+i;jc3vr::Publish(s,s,1,0);}
    assert(jc3vr::count==jc3vr::kCapacity && jc3vr::dropped==200-jc3vr::kCapacity);
    jc3vr::until=0;jc3vr::Publish(s,s,1,0);assert(jc3vr::count==jc3vr::kCapacity);
    jc3vr::busy.test_and_set();assert(!jc3vr::Export());assert(jc3vr::count==jc3vr::kCapacity);
    jc3vr::busy.clear();assert(jc3vr::Export());assert(jc3vr::count==0 && reports<700);
    const auto page=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
    assert(page);int value=0;assert(!jc3vr::Read(value,reinterpret_cast<std::uintptr_t>(page)));
    VirtualFree(page,0,MEM_RELEASE);
}
}
namespace jc3vr {
std::uintptr_t VrBodyCharacter(){return reinterpret_cast<std::uintptr_t>(lab::rico.data());}
bool GetVrBodyHandTarget(int,float[3],float[3],float[3]){return false;}
void TraceRuntime(std::string_view){++lab::reports;}
}
int main(){lab::ReadOnlyObservation();lab::NativeMutationNotRestored();lab::PlacementMatrixHistory();lab::BoundedAndSafe();std::cout<<"Attachment observer read-only/bounds/expiry/native-call contract PASS; not a live hook test.\n";}
