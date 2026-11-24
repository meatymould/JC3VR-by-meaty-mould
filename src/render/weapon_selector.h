#pragma once
#include <array>
#include <cmath>
namespace jc3vr {
// Actual installed gamepad bindings, not guessed selector categories.
inline constexpr std::array<int,4> kWeaponSelectorSlots{0,4,6,1};
inline constexpr char kWeaponIconShader[]=R"(
struct Input { float4 position:POSITION; float2 uv:TEXCOORD; float4 tint:COLOR; };
struct Output { float4 position:SV_Position; float2 uv:TEXCOORD; float4 tint:COLOR; };
Output VS(Input i) { Output o; o.position=i.position; o.uv=i.uv; o.tint=i.tint; return o; }
Texture2D Icon:register(t0); SamplerState Linear:register(s0);
float4 PS(Output i):SV_Target {
    if (i.uv.x<0) return i.tint; // solid HUD backing, no texture sampling
    float4 c=Icon.Sample(Linear,i.uv)*i.tint; clip(c.a-0.01); return c;
}
)";
struct WeaponIconCorner { float x,y,u,v; };
inline std::array<WeaponIconCorner,6> WeaponIconQuad(unsigned width,unsigned height,bool picked) {
    if (!width || !height || width>4096 || height>4096) return {};
    const float x=(picked?0.112f:0.096f)*0.5f;
    const float y=x*static_cast<float>(height)/width;
    return {{{-x,y,0,0},{x,y,1,0},{x,-y,1,1},{-x,y,0,0},{x,-y,1,1},{-x,-y,0,1}}};
}
inline std::array<WeaponIconCorner,6> WeaponIconBacking(unsigned width,unsigned height,bool picked) {
    auto corners=WeaponIconQuad(width,height,picked);
    if (!width || !height || width>4096 || height>4096) return {};
    for (auto& c:corners) {
        c.x+=c.x<0?-0.006f:0.006f;
        c.y+=c.y<0?-0.006f:0.006f;
        c.u=c.v=-1;
    }
    return corners;
}
}
