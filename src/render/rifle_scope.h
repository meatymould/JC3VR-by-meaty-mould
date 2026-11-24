#pragma once
#include <algorithm>
#include <cmath>

namespace jc3vr {
struct SniperOpticMount {
    bool valid=false;
    float fromGrip[3]{}; // weapon model axes: +X right, +Y up, +Z rear, JC3 units
    float radius=0;
};
inline SniperOpticMount SniperModelOptic(int icon) {
    // Least-squares fits to the actual rear eyepiece inner rings, stock LOD1.
    // No controller-space guess. Bundle fingerprints/fit residuals in docs.
    switch(icon) {
    case 161:return {true,{0,0.19167012f,0.01632029f},0.01271852f};
    case 162:return {true,{-0.00003429f,0.18997923f,-0.00667449f},0.01540620f};
    case 163:return {true,{0.00011701f,0.21458690f,0.01571307f},0.01638893f};
    default:return {};
    }
}
// The same two-hand barrel axis and right-hand roll used for native placement.
// Socket is provisional; never alter XR eye pose/FOV to align the optic.
inline bool BuildScopeLensFrame(const float grip[3],const float barrel[3],const float handUp[3],
                               float lens[3],float right[3],float up[3],float forward[3],
                               const SniperOpticMount* mount=nullptr,float unitsPerMeter=1) {
    if(mount && (!mount->valid || !std::isfinite(unitsPerMeter) || unitsPerMeter<=0)) return false;
    float length=0;
    for(int i=0;i<3;++i) {
        if(!std::isfinite(grip[i]) || !std::isfinite(barrel[i]) || !std::isfinite(handUp[i])) return false;
        length+=barrel[i]*barrel[i];
    }
    if(length<0.5f || length>1.5f) return false;
    length=std::sqrt(length);float dot=0;
    for(int i=0;i<3;++i) {forward[i]=barrel[i]/length;dot+=handUp[i]*forward[i];}
    length=0;
    for(int i=0;i<3;++i) {up[i]=handUp[i]-dot*forward[i];length+=up[i]*up[i];}
    if(length<0.01f || !std::isfinite(length)) return false;
    length=std::sqrt(length);
    for(int i=0;i<3;++i) {up[i]/=length;lens[i]=grip[i]+forward[i]*0.12f+up[i]*0.08f;}
    right[0]=forward[1]*up[2]-forward[2]*up[1];
    right[1]=forward[2]*up[0]-forward[0]*up[2];
    right[2]=forward[0]*up[1]-forward[1]*up[0];
    if(mount) for(int i=0;i<3;++i) {
        if(!std::isfinite(mount->fromGrip[i]) || std::abs(mount->fromGrip[i])>2) return false;
        lens[i]=grip[i]+(right[i]*mount->fromGrip[0]+up[i]*mount->fromGrip[1]-forward[i]*mount->fromGrip[2])/unitsPerMeter;
    }
    return true;
}
// LOCAL-space metres. Headset forward is NOT the eyeball's gaze direction:
// looking at the aperture in peripheral vision must not switch its image off.
// Fade at eye-relief/pupil limits instead of the old hard 7cm/20cm cutoffs.
inline float ScopeEyeVisibility(const float eye[3],const float lens[3],const float axis[3],float apertureRadius) {
    if(!std::isfinite(apertureRadius) || apertureRadius<.003f || apertureRadius>.05f) return 0;
    float length=0,along=0,distance=0;
    for(int i=0;i<3;++i) {
        if(!std::isfinite(eye[i]) || !std::isfinite(lens[i]) || !std::isfinite(axis[i])) return 0;
        length+=axis[i]*axis[i];
        const float delta=lens[i]-eye[i];along+=delta*axis[i];distance+=delta*delta;
    }
    if(length<0.5f || length>1.5f) return 0;
    along/=std::sqrt(length);
    const auto fade=[](float x,float low,float high) {
        const float t=std::clamp((x-low)/(high-low),0.0f,1.0f);return t*t*(3-2*t);
    };
    const float lateral=std::sqrt(std::max(0.0f,distance-along*along));
    return fade(along,.025f,.045f)*(1-fade(along,.25f,.35f))*
        (1-fade(lateral,apertureRadius*.65f,apertureRadius*1.35f));
}

// Nine float4 constants: lens centre/right/up in eye space, target tangents,
// measured source projection, scope ray axes in retained-source view space,
// parameters [radius metres, magnification, eye relief, aligned].
inline constexpr char kRifleScopeShader[]=R"(
cbuffer Scope : register(b0) {
    float4 centerEye; float4 rightEye; float4 upEye; float4 fov;
    float4 sourceProjection; float4 rayRight; float4 rayUp; float4 rayForward; float4 params;
};
Texture2D scene : register(t0); SamplerState sampleScene : register(s0);
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output VS(uint id : SV_VertexID) {
    const float2 corners[6]={float2(-1,1),float2(1,1),float2(-1,-1),float2(-1,-1),float2(1,1),float2(1,-1)};
    float2 p=corners[id];float3 v=centerEye.xyz+params.x*(p.x*rightEye.xyz+p.y*upEye.xyz);
    float d=-v.z; Output o;
    o.position=float4((2*v.x-(fov.x+fov.y)*d)/(fov.y-fov.x),
                      (2*v.y-(fov.z+fov.w)*d)/(fov.z-fov.w),d-0.01,d);
    o.uv=p;return o;
}
float4 PS(Output input) : SV_Target {
    float2 p=input.uv;float r=length(p);if(r>1) discard;
    if(r>0.90 || params.w<=0.001) return float4(0.012,0.012,0.015,1);
    // Optical axis, not the middle of the headset texture. Direction-only
    // angular magnifier: NOT a separate scope-camera scene render.
    float slope=params.x/(max(params.z,0.025)*params.y);
    float3 ray=rayForward.xyz+slope*(p.x*rayRight.xyz+p.y*rayUp.xyz);
    if(ray.z>=-0.001) return float4(0,0,0,1);
    float2 uv=float2(0.5*(sourceProjection.x*ray.x/-ray.z-sourceProjection.z+1),
                    0.5*(1-sourceProjection.y*ray.y/-ray.z+sourceProjection.w));
    if(any(uv<0) || any(uv>1)) return float4(0,0,0,1);
    float4 color=scene.Sample(sampleScene,uv);color.a=1;
    if((abs(p.x)<0.006 || abs(p.y)<0.006) && r<0.80) color.rgb=0.015;
    color.rgb=lerp(float3(0.012,0.012,0.015),color.rgb,saturate(params.w));
    return color;
}
)";
}
