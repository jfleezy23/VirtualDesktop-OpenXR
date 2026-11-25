// Game color is pre-exposed linear light. Private textures start at (0,0) and match one eye.
#include "PerceptualTransfer.hlsli"

cbuffer Parameters : register(b0) {
    uint2 sourceOrigin;
    uint2 renderExtent;
    float preExposure;
    uint2 depthOrigin;
    uint extractDepth;
    uint packedColor;
};
Texture2D<float4> sceneColor : register(t0);
Texture2D<float4> modelInput : register(t1);
Texture2D<float4> modelAnswer : register(t2);
Texture2D<float> depthPlane : register(t3);
RWTexture2D<float4> destination : register(u0);
RWTexture2D<float> privateDepth : register(u1);

bool ValidScene(float3 rgb, float exposure) {
    // Check the bits before arithmetic; DXIL fast math can assume float operands are finite.
    uint exposureBits = asuint(exposure);
    return exposureBits >= 0x00800000u && exposureBits < 0x7f800000u &&
           all((asuint(rgb) & 0x7fffffffu) <= 0x477fe000u) && all(rgb >= 0);
}

float Peak(float3 v) {
    return max(v.r, max(v.g, v.b));
}

float ToSrgb(float v) {
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float FromSrgb(float v) {
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

float3 EncodeScene(float3 rgb, float exposure) {
    if (!ValidScene(rgb, exposure))
        return 0;
    // Equivalent to scene / (1 + peak(scene)), with scene = rgb / exposure.
    float3 proxy = rgb / (exposure + Peak(rgb));
    return float3(ToSrgb(proxy.r), ToSrgb(proxy.g), ToSrgb(proxy.b));
}

float4 ResolveScene(float4 original, float exposure, float3 guide, float3 answer) {
    if (!ValidScene(original.rgb, exposure) || all(original.rgb == 0) || all(guide == answer))
        return original;
    // Both model views are raw UNORM. Check defensively before transfer or nonlinear arithmetic.
    if (any(asuint(guide) > 0x3f800000u) || any(asuint(answer) > 0x3f800000u))
        return original;
    float denominator = exposure + Peak(original.rgb);
    float3 nativeProxy = original.rgb / denominator;
    float3 guideLinear = float3(FromSrgb(guide.r), FromSrgb(guide.g), FromSrgb(guide.b));
    float3 answerLinear = float3(FromSrgb(answer.r), FromSrgb(answer.g), FromSrgb(answer.b));

    // Near black, respect the model input's narrow shadow toe. Once the highlight shoulder
    // approaches saturation the eight-bit model input carries very little scene information.
    float level = Peak(nativeProxy);
    float weight = smoothstep(0, 1.0 / 128.0, min(level, Peak(guideLinear))) *
                   (1.0 - smoothstep(0.75, 63.0 / 64.0, level));
    if (weight == 0)
        return original;
    float3 edit = TransferPerceptualEdit(nativeProxy, LinearToPerceptual(nativeProxy), guideLinear, answerLinear);
    // Limit even extreme vendor edits to 25% of this channel plus a small cross-channel allowance.
    // Convert only the difference back to the original exposure, preserving its HDR headroom.
    float3 budget = 0.25 * nativeProxy + level / 64.0;
    float3 result = original.rgb + clamp(edit, -budget, budget) * (weight * denominator);
    return float4(clamp(result, 0.0, 65504.0), original.a);
}

[numthreads(8, 8, 1)]
void Encode(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= renderExtent))
        return;
    float3 rgb = sceneColor.Load(int3(sourceOrigin + id.xy, 0)).rgb;
    destination[id.xy] = float4(EncodeScene(rgb, preExposure), 1);
    if (extractDepth != 0)
        privateDepth[id.xy] = depthPlane.Load(int3(depthOrigin + id.xy, 0));
}

[numthreads(8, 8, 1)]
void Resolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= renderExtent))
        return;
    float4 raw = sceneColor.Load(int3(sourceOrigin + id.xy, 0));
    float3 guide = modelInput.Load(int3(id.xy, 0)).rgb;
    float3 answer = modelAnswer.Load(int3(id.xy, 0)).rgb;
    float4 result = ResolveScene(raw, preExposure, guide, answer);
    // Keep positive edits within the finite limits of the eleven- and ten-bit float channels.
    if (packedColor != 0 && ValidScene(raw.rgb, preExposure))
        result.rgb = min(result.rgb, float3(65024.0, 65024.0, 64512.0));
    destination[id.xy] = result;
}
