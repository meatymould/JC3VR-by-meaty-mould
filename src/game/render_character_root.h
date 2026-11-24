#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace jc3vr {
// Own a COPY of the engine's completed render root, never a stack pointer.
struct RenderRootSample {
    std::uintptr_t character{};std::uint32_t frame{};std::uint64_t tick{};
    float alpha{};std::array<float,16> world{};
};
struct RenderRootMeta {std::uint32_t frame{};std::uint64_t tick{};float alpha{};};
inline bool ValidRenderRoot(const float* m) {
    if(!m)return false;
    for(unsigned i=0;i<16;++i)if(!std::isfinite(m[i]))return false;
    if(std::abs(m[15]-1)>0.001f || std::abs(m[3])+std::abs(m[7])+std::abs(m[11])>0.001f)return false;
    for(unsigned i=0;i<3;++i)for(unsigned j=i;j<3;++j){
        float dot=0;for(unsigned k=0;k<3;++k)dot+=m[i*4+k]*m[j*4+k];
        if(std::abs(dot-(i==j?1.0f:0.0f))>0.02f)return false;
    }
    const float det=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);
    return det>0.98f && det<1.02f;
}
// Synchronisation is supplied by the caller. Stale/foreign frames fail closed;
// no interpolation, velocity estimate or extrapolation is performed here.
class RenderRootCache {
public:
    bool Publish(std::uintptr_t character,std::uint32_t frame,std::uint64_t tick,float alpha,const float* world){
        if(!character || !std::isfinite(alpha) || alpha<0 || alpha>1 || !ValidRenderRoot(world)){sample_={};return false;}
        sample_.character=character;sample_.frame=frame;sample_.tick=tick;sample_.alpha=alpha;
        std::copy_n(world,16,sample_.world.begin());return true;
    }
    bool Get(std::uintptr_t character,std::uint32_t frame,std::uint64_t now,RenderRootSample& out)const{
        if(!character || character!=sample_.character || frame!=sample_.frame || now<sample_.tick || now-sample_.tick>100)return false;
        out=sample_;return true;
    }
    RenderRootMeta DiagnosticMetadata()const{return {sample_.frame,sample_.tick,sample_.alpha};}
private:RenderRootSample sample_{};
};
bool InstallRenderCharacterRoot(bool enabled);
bool GetCharacterRenderWorld(std::uintptr_t character,float world[16]);
void ReportRenderCharacterRoot(); // explicit F3 diagnostics, never per-draw logging
}
