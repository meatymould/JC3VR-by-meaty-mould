#pragma once
namespace jc3vr {
inline constexpr char kHudCompositeShader[]=R"(
Texture2D Before : register(t0);
Texture2D After : register(t1);
SamplerState Linear : register(s0);
cbuffer Hud : register(b0) { float4 Shift; float4 Size; float4 Crosshair; float4 Crosshair2; };  // dx, dy, scale, threshold; W, H; target x, y, half-size, enabled (gun, grapple)
float4 VS(uint id : SV_VertexID) : SV_Position {
    const float2 p[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    return float4(p[id], 0.0, 1.0);
}
float4 PS(float4 position : SV_Position) : SV_Target {
    const float4 scene = Before.Load(int3(int2(position.xy), 0));
    const float2 centre = 0.5 * Size.xy;
    // The crosshair region (HUD centre) drawn where each hand points: the
    // gun hand, and the grapple (left) hand.
    for (int k = 0; k < 2; ++k) {
        const float4 target = k == 0 ? Crosshair : Crosshair2;
        if (target.w < 0.5) continue;
        const float2 c = centre + (position.xy - target.xy) / Shift.z;
        if (all(abs(c - centre) < target.z)) {
            const float2 uv = c / Size.xy;
            const float4 hud = After.SampleLevel(Linear, uv, 0);
            const float4 under = Before.SampleLevel(Linear, uv, 0);
            const float3 d = abs(hud.rgb - under.rgb);
            if (max(d.r, max(d.g, d.b)) > Shift.w) return float4(hud.rgb, 1.0);
        }
    }
    const float2 q = centre + (position.xy - centre - Shift.xy) / Shift.z;
    const float2 statusUv=q/Size.xy;
    if((Size.z>.5 && all(statusUv>=float2(.015,.015)) && all(statusUv<=float2(.34,.34))) ||
       (Size.w>.5 && all(statusUv>=float2(.76,.73)) && all(statusUv<=float2(.995,.995)))) return scene;
    // With the crosshair relocated, its original place is left out here.
    if (all(q >= 0.0) && all(q < Size.xy) && !((Crosshair.w > 0.5 || Crosshair2.w > 0.5) && all(abs(q - centre) < max(Crosshair.z, Crosshair2.z)))) {
        const float2 uv = q / Size.xy;
        const float4 hud = After.SampleLevel(Linear, uv, 0);
        const float4 under = Before.SampleLevel(Linear, uv, 0);
        const float3 d = abs(hud.rgb - under.rgb);
        if (max(d.r, max(d.g, d.b)) > Shift.w) return float4(hud.rgb, 1.0);
    }
    return scene;
}
// Menu panel: the whole frame, scaled and shifted like the HUD, on a dark
// backdrop.
float4 PSMenu(float4 position : SV_Position) : SV_Target {
    const float2 centre = 0.5 * Size.xy;
    const float2 q = centre + (position.xy - centre - Shift.xy) / Shift.z;
    if (all(q >= 0.0) && all(q < Size.xy)) return float4(After.SampleLevel(Linear, q / Size.xy, 0).rgb, 1.0);
    return float4(0.02, 0.02, 0.025, 1.0);
}
// Coverage pass: survives (and is counted by the occlusion query) only where
// JC3 drew HUD/menu pixels over the world.
float4 PSCoverage(float4 position : SV_Position) : SV_Target {
    const int3 at = int3(int2(position.xy), 0);
    const float3 d = abs(After.Load(at).rgb - Before.Load(at).rgb);
    if (max(d.r, max(d.g, d.b)) <= Shift.w) discard;
    return 0.0;
}
)";
}
