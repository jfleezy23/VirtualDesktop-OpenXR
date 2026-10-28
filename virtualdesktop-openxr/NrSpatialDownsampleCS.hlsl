#include "NrSpatialCommon.hlsli"
RWTexture2D<float4> proxyOutput : register(u0);
RWTexture2D<float> depthOutput : register(u1);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= workExtent))
        return;
    float2 lo = float2(id.xy) * float2(processingRect.zw) / float2(workExtent);
    float2 hi = float2(id.xy + 1) * float2(processingRect.zw) / float2(workExtent);
    float4 sum = 0.0;
    // A source texel represents a constant cell [i,i+1). Integrate its exact
    // overlap, never sampling outside this eye's valid source rectangle.
    for (uint y = (uint)floor(lo.y); y < min((uint)ceil(hi.y), processingRect.w); ++y) {
        float wy = max(0.0, min(hi.y, y + 1.0) - max(lo.y, (float)y));
        for (uint x = (uint)floor(lo.x); x < min((uint)ceil(hi.x), processingRect.z); ++x) {
            float wx = max(0.0, min(hi.x, x + 1.0) - max(lo.x, (float)x));
            float4 n = nativeColor.Load(int4(sourceRect.xy + processingRect.xy + uint2(x, y), 0, 0));
            sum += float4(Decode(n.rgb), n.a) * wx * wy;
        }
    }
    float4 mean = sum / ((hi.x - lo.x) * (hi.y - lo.y));
    proxyOutput[id.xy] = float4(Encode(mean.rgb), mean.a);
    if (hasDepth) {
        uint2 p = min(uint2((float2(id.xy) + 0.5) * float2(depthRect.zw) / float2(workExtent)), depthRect.zw - 1);
        // Preserve the full-eye expression above, including its rounding. A crop
        // is normalized through the complete color eye into its independent depth rect.
        if (any(processingRect.xy != 0) || any(processingRect.zw != sourceRect.zw)) {
            float2 nativePoint =
                float2(processingRect.xy) + (float2(id.xy) + 0.5) * float2(processingRect.zw) / float2(workExtent);
            p = min(uint2(nativePoint * float2(depthRect.zw) / float2(sourceRect.zw)), depthRect.zw - 1);
        }
        depthOutput[id.xy] = nativeDepth.Load(int4(depthRect.xy + p, 0, 0));
    }
}
