#define JC3VR_HAVE_MINHOOK 0
#include "../src/game/render_character_root.cpp"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace {int forwards{};void* forwardedCharacter{};const float* forwardedWorld{};float forwardedAlpha{};
void __fastcall Native(void* c,float a,const float* m){++forwards;forwardedCharacter=c;forwardedAlpha=a;forwardedWorld=m;}}
namespace jc3vr {std::uintptr_t VrBodyCharacter(){return 0x12345678;}void TraceRuntime(std::string_view){} }
void Check(bool v){if(!v){std::cerr<<"render-root invariant failed\n";std::abort();}}
int main(){
    float world[16]{1,0,0,0,0,1,0,0,0,0,1,0,11,2,3,1};
    jc3vr::RenderRootCache cache;jc3vr::RenderRootSample result;
    Check(cache.Publish(1,7,100,.5f,world));world[12]=55;
    Check(cache.Get(1,7,100,result) && result.world[12]==11); // copied, not aliased
    Check(!cache.Get(2,7,100,result));Check(!cache.Get(1,8,100,result));
    Check(!cache.Get(1,7,201,result));Check(!cache.Get(1,7,99,result));
    world[12]=11;
    // An exact completed native root fixes a one-metre simulation/render offset.
    const float target=11.5f,simRoot=10,renderRoot=11;
    const float oldLocal=target-simRoot,newLocal=target-renderRoot;
    Check(std::abs((renderRoot+oldLocal)-target)==1);
    Check(std::abs((renderRoot+newLocal)-target)<1e-6f);
    // We consume arbitrary valid engine rotations verbatim, never lerp axes.
    float rotated[16]{0,0,-1,0,0,1,0,0,1,0,0,0,11,2,3,1};
    Check(cache.Publish(1,8,102,.37f,rotated));Check(cache.Get(1,8,102,result));
    Check(!std::memcmp(result.world.data(),rotated,64));
    world[0]=-1;Check(!cache.Publish(1,9,103,.5f,world));Check(!cache.Get(1,8,103,result));world[0]=1;
    Check(!cache.Publish(1,9,103,1.1f,world));
    world[12]=std::numeric_limits<float>::quiet_NaN();Check(!cache.Publish(1,9,103,.5f,world));world[12]=11;
    constexpr auto size=0x2D3B000u;
    void* memory=VirtualAlloc(nullptr,size,MEM_RESERVE,PAGE_NOACCESS);Check(memory!=nullptr);
    const auto base=reinterpret_cast<std::uintptr_t>(memory);
    Check(VirtualAlloc(reinterpret_cast<void*>(base+0x2D3A000),4096,MEM_COMMIT,PAGE_READWRITE)!=nullptr);
    auto* counter=reinterpret_cast<unsigned*>(base+0x2D3A6AC);auto* frame=reinterpret_cast<unsigned*>(base+0x2D3A6B0);
    *counter=*frame=9;jc3vr::image=base;jc3vr::active.store(true);
    auto* character=reinterpret_cast<void*>(jc3vr::VrBodyCharacter()-8);
    jc3vr::Observe(character,.5f,world,base+0x7FA3DF);
    float observed[16]{};Check(jc3vr::GetCharacterRenderWorld(jc3vr::VrBodyCharacter(),observed));
    Check(!std::memcmp(world,observed,64));*frame=10;
    Check(!jc3vr::GetCharacterRenderWorld(jc3vr::VrBodyCharacter(),observed));
    *counter=10;jc3vr::Observe(character,.7f,world,base+0x7FA3DF);
    Check(jc3vr::GetCharacterRenderWorld(jc3vr::VrBodyCharacter(),observed));
    jc3vr::originalRoot=&Native;const auto before=jc3vr::captures.load();
    jc3vr::RootDetour(character,.2f,world); // foreign caller rejected, original still runs
    Check(forwards==1 && forwardedCharacter==character && forwardedAlpha==.2f && forwardedWorld==world);
    Check(jc3vr::captures.load()==before && world[12]==11);
    Check(!jc3vr::InstallRenderCharacterRoot(false));
    Check(!jc3vr::GetCharacterRenderWorld(jc3vr::VrBodyCharacter(),observed));
    VirtualFree(memory,0,MEM_RELEASE);
    std::cout<<"native render-root cache, actual observer/forwarding and frame rejection passed\n";
}
