// Original spatial NR math. All views use raw UNORM storage; transfer is explicit.
cbuffer Parameters : register(b0) {
    uint4 sourceRect;
    uint4 depthRect;
    uint4 processingRect;
    uint2 workExtent;
    uint isSrgb;
    uint hasDepth;
    uint outputX;
    uint transitionWidth;
    uint2 reserved;
};
Texture2DArray<float4> nativeColor : register(t0);
Texture2DArray<float> nativeDepth : register(t1);
Texture2D<float4> storedProxy : register(t2);
Texture2D<float4> answer : register(t3);

float3 Decode(float3 v) {
    if (!isSrgb)
        return v;
    return float3(v.r <= 0.04045 ? v.r / 12.92 : pow(max((v.r + 0.055) / 1.055, 0.0), 2.4),
                  v.g <= 0.04045 ? v.g / 12.92 : pow(max((v.g + 0.055) / 1.055, 0.0), 2.4),
                  v.b <= 0.04045 ? v.b / 12.92 : pow(max((v.b + 0.055) / 1.055, 0.0), 2.4));
}
float3 Encode(float3 v) {
    if (!isSrgb)
        return v;
    // Negative edits remain signed until the final UNORM store. max protects
    // speculative pow evaluation; the linear branch retains negative values.
    float3 high = 1.055 * pow(max(v, 0.0), 1.0 / 2.4) - 0.055;
    return float3(v.r <= 0.0031308 ? 12.92 * v.r : high.r,
                  v.g <= 0.0031308 ? 12.92 * v.g : high.g,
                  v.b <= 0.0031308 ? 12.92 * v.b : high.b);
}

// Anchor model edits to the original signal, never to the enhanced answer.
// All thresholds are linear-light values, independent of source storage encoding.
// Preserve shared RGB structure outside the narrow black toe while limiting
// shadow chroma excursions. Never create a channel edit the model did not request.
float3 ProtectNrResidual(float3 sourceLinear, float3 guideLinear, float3 residual) {
    float sourceLevel = max(sourceLinear.r, max(sourceLinear.g, sourceLinear.b));
    float guideLevel = max(guideLinear.r, max(guideLinear.g, guideLinear.b));
    float level = max(0, min(sourceLevel, guideLevel));
    if (level >= 1.0 / 128.0)
        return residual;
    float lumaWeight = smoothstep(0, 1.0 / 1024.0, level);
    float chromaWeight = smoothstep(0, 1.0 / 128.0, level);
    // Only a same-sign edit shared by all channels is neutral structure. A
    // luminance projection would spread a single-channel bias into untouched
    // channels. Mixed-sign edits have no common neutral component.
    float neutral = min(max(residual.r, 0), min(max(residual.g, 0), max(residual.b, 0))) +
                    max(min(residual.r, 0), max(min(residual.g, 0), min(residual.b, 0)));
    return lumaWeight * neutral.xxx + chromaWeight * (residual - neutral.xxx);
}
