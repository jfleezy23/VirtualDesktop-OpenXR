#include "NrSpatialCommon.hlsli"

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    uint2 pixel = uint2(position.xy) - uint2(outputX, 0);
    float4 raw = nativeColor.Load(int4(sourceRect.xy + pixel, 0, 0));
    if (any(pixel < processingRect.xy) || any(pixel >= processingRect.xy + processingRect.zw))
        return raw;
    if (all(raw.rgb == 0))
        return raw;
    float3 nativeLinear = Decode(raw.rgb);
    float2 localPoint = float2(pixel - processingRect.xy) + 0.5;
    float2 q = localPoint * float2(workExtent) / float2(processingRect.zw) - 0.5;
    int2 base = int2(floor(q));
    float2 fraction = frac(q);
    float3 edit = 0, bilinearEdit = 0;
    float normalization = 0;
    bool zero = true;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            int2 p = clamp(base + int2(x, y), int2(0, 0), int2(workExtent) - 1);
            float3 guideRaw = storedProxy.Load(int3(p, 0)).rgb;
            float3 answerRaw = answer.Load(int3(p, 0)).rgb;
            float3 guide = Decode(guideRaw);
            float3 residual = Decode(answerRaw) - guide;
            residual = ProtectNrResidual(nativeLinear, guide, residual);
            zero = zero && all(guideRaw == answerRaw);
            float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
            float3 distance = guide - nativeLinear;
            float guided = weight * (1.0 / 1024.0) / (1.0 / 1024.0 + dot(distance, distance) / 3.0);
            edit += guided * residual;
            normalization += guided;
            bilinearEdit += weight * residual;
        }
    }
    // Raw early return avoids any sRGB round trip for an unchanged edit.
    if (zero)
        return raw;
    float3 residual = normalization > 0 && isfinite(normalization) ? edit / normalization : bilinearEdit;
    float strength = 1;
    if (transitionWidth) {
        float2 edge = min(localPoint, float2(processingRect.zw) - localPoint);
        strength = smoothstep(0, float(transitionWidth), min(edge.x, edge.y));
    }
    return float4(Encode(nativeLinear + strength * residual), raw.a);
}
