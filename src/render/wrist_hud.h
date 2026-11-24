#pragma once
#include <cmath>
namespace jc3vr {
inline bool ReadableHudFrame(const float toHead[3],float right[3],float up[3]) {
    const float length=std::sqrt(toHead[0]*toHead[0]+toHead[1]*toHead[1]+toHead[2]*toHead[2]);
    if(!std::isfinite(length) || length<.12f || length>1.25f) return false;
    const float nx=toHead[0]/length,ny=toHead[1]/length,nz=toHead[2]/length;
    const float horizontal=std::hypot(nx,nz);if(horizontal<.01f) return false;
    right[0]=nz/horizontal;right[1]=0;right[2]=-nx/horizontal;
    up[0]=ny*right[2];up[1]=nz*right[0]-nx*right[2];up[2]=-ny*right[0];
    return true;
}
// Controller-attached, metre-sized geometry, projected through the SAME eye
// pose and asymmetric FOV as the other tracked overlays. No screen-space crop.
inline constexpr char kWristHudShader[]=R"(
cbuffer Wrist:register(b0) {
    float4 centerEye; float4 rightEye; float4 upEye; float4 fov;
    float4 data; // magazine, reserve, right capacity / left health, left/right
    float4 style; // half-width, half-height, has native weapon icon, icon aspect
    float4 sourceRect; // native status region, normalized original HUD coordinates
};
Texture2D icon:register(t0); SamplerState linearSample:register(s0);
Texture2D nativeAfter:register(t1); Texture2D nativeBefore:register(t2);
struct Output { float4 position:SV_Position; float2 uv:TEXCOORD0; };
Output VS(uint id:SV_VertexID) {
    const float2 corners[6]={float2(-1,1),float2(1,1),float2(-1,-1),float2(-1,-1),float2(1,1),float2(1,-1)};
    float2 p=corners[id]; float3 v=centerEye.xyz+p.x*style.x*rightEye.xyz+p.y*style.y*upEye.xyz;
    float w=-v.z; Output o;
    o.position=float4((2*v.x-(fov.x+fov.y)*w)/(fov.y-fov.x),
                      (2*v.y-(fov.z+fov.w)*w)/(fov.z-fov.w),w-0.01,w);
    o.uv=float2(p.x*.5+.5,.5-p.y*.5); return o;
}
// Own tiny vector numerals, not a raster HUD crop or shipped game texture.
float digit(float2 uv,int n) {
    if(any(uv<0)||any(uv>1)) return 0;
    const uint masks[10]={63,6,91,79,102,109,125,7,127,111};
    uint m=n<0?64:masks[clamp(n,0,9)];
    bool a=uv.x>.18 && uv.x<.82 && uv.y<.12;
    bool b=uv.x>.80 && uv.y>.10 && uv.y<.48;
    bool c=uv.x>.80 && uv.y>.52 && uv.y<.90;
    bool d=uv.x>.18 && uv.x<.82 && uv.y>.88;
    bool e=uv.x<.20 && uv.y>.52 && uv.y<.90;
    bool f=uv.x<.20 && uv.y>.10 && uv.y<.48;
    bool g=uv.x>.18 && uv.x<.82 && abs(uv.y-.5)<.06;
    return ((a&&(m&1))||(b&&(m&2))||(c&&(m&4))||(d&&(m&8))||(e&&(m&16))||(f&&(m&32))||(g&&(m&64)))?1:0;
}
float number(float2 p,float value) {
    if(any(p<0)||p.y>1||p.x>=4) return 0;
    int slot=(int)p.x;
    if(value<0) return digit(float2(frac(p.x)/.8,p.y),-1);
    int v=(int)min(value,9999);int divisor=slot==0?1000:slot==1?100:slot==2?10:1;
    if(slot<3 && v<divisor) return 0;
    return digit(float2(frac(p.x)/.8,p.y),(v/divisor)%10);
}
float centeredNumber(float2 p,float2 center,float cellWidth,float height,float value) {
    float slots=value<0?2:value<10?1:value<100?2:value<1000?3:4;
    // Preserve consistent physical digit size, but remove unused leading slots.
    float2 q=float2((p.x-center.x)/cellWidth+slots*.5,(p.y-center.y)/height);
    if(q.x<0 || q.x>=slots || q.y<0 || q.y>1) return 0;
    int slot=(int)q.x, v=(int)clamp(value,0,9999);
    int power=(int)slots-slot-1;
    int divisor=power==3?1000:power==2?100:power==1?10:1;
    return digit(float2(frac(q.x)/.80,q.y),value<0?-1:(v/divisor)%10);
}
float4 PS(Output i):SV_Target {
    float2 p=i.uv;
    float3 color=0;
    float alpha=0;
    if(centerEye.w>.5) {
        // Actual native labels/artwork/data, not guessed counters. Letterbox
        // the source in physical card dimensions. Unchanged scene pixels are
        // discarded; blended native artwork can retain colour
        // from the scene (before/after cannot uniquely reconstruct alpha).
        float2 uv=p;
        float boxAspect=style.x/style.y;
        float sourceAspect=upEye.w;
        if(sourceAspect<boxAspect) uv.x=(uv.x-.5)*boxAspect/sourceAspect+.5;
        else uv.y=(uv.y-.5)*sourceAspect/boxAspect+.5;
        if(all(uv>=0)&&all(uv<=1)) {
            float2 at=lerp(sourceRect.xy,sourceRect.zw,uv);
            float3 hud=nativeAfter.SampleLevel(linearSample,at,0).rgb;
            float3 under=nativeBefore.SampleLevel(linearSample,at,0).rgb;
            float3 delta=abs(hud-under);
            if(max(delta.r,max(delta.g,delta.b))>rightEye.w) return float4(hud,1);
        }
        discard; // no card, border, or letterbox background in native mode
    }
    if(data.w>.5) {
        // Native-HUD-inspired circular magazine dial, measured in metres so
        // the rectangular wrist card cannot stretch the circle into an oval.
        float2 physical=(p-.5)*float2(2*style.x,2*style.y);
        float radius=length(physical);
        float aa=max(fwidth(radius),.0001);
        float ring=1-smoothstep(.0016,.0016+aa,abs(radius-.046));
        float angle=atan2(physical.x,-physical.y)+2.5;
        bool arc=angle>=0 && angle<=5;
        float remaining=(data.z>0 && data.x>=0)?saturate(data.x/data.z):-1;
        float3 ringColor=(remaining>=0 && angle<remaining*5)?float3(.88,.94,.95):float3(.22,.27,.29);
        if(arc) {color=ringColor*ring;alpha=ring;}
        if(style.z>.5 && p.y>.20 && p.y<.40 && p.x>.24 && p.x<.76) {
            float2 uv=float2((p.x-.24)/.52,(p.y-.20)/.20);
            float boxAspect=(.52*style.x)/(.20*style.y);
            float imageAspect=max(style.w,.001);
            if(imageAspect<boxAspect) uv.x=(uv.x-.5)*boxAspect/imageAspect+.5;
            else uv.y=(uv.y-.5)*imageAspect/boxAspect+.5;
            // Letterbox in physical card dimensions; never stretch native art.
            if(all(uv>=0)&&all(uv<=1)) {
                float4 c=icon.Sample(linearSample,uv);
                color=lerp(color,c.rgb,c.a);
                alpha=max(alpha,c.a);
            }
        }
        float ink=centeredNumber(p,float2(.41,.47),.075,.26,data.x);
        ink=max(ink,centeredNumber(p,float2(.64,.55),.038,.15,data.y));
        if(p.y>.53 && p.y<.70 && abs(p.x-(.55-(p.y-.615)*.12))<.008) ink=1;
        color=lerp(color,float3(.88,.95,.97),ink);
        alpha=max(alpha,ink);
    } else {
        alpha=1; // unverified health fallback remains gated off
        // Health cross and native normalized health bar. Unknown = dashed.
        if((abs(p.x-.19)<.028&&abs(p.y-.28)<.15)||(abs(p.x-.19)<.09&&abs(p.y-.28)<.046)) color=float3(.2,.8,.67);
        color=lerp(color,float3(.88,.95,.97),number(float2((p.x-.32)/.135,(p.y-.14)/.29),data.z<0?-1:round(data.z*100)));
        if(p.y>.58&&p.y<.76&&p.x>.09&&p.x<.91) {
            color=float3(.045,.075,.083);
            if(data.z>=0 && (p.x-.09)/.82<data.z) color=lerp(float3(.85,.17,.08),float3(.08,.72,.55),data.z);
        }
    }
    if(alpha<.01) discard;
    return float4(color,alpha);
}
)";
}
