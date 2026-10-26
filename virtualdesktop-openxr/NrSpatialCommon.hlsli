// Original spatial NR math. All views use raw UNORM storage; transfer is explicit.
cbuffer Parameters : register(b0) {
    uint4 sourceRect;
    uint4 depthRect;
    uint2 workExtent;
    uint isSrgb;
    uint hasDepth;
    uint outputX;
    uint3 reserved;
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
