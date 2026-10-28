#include "NrSpatialCommon.hlsli"

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    uint2 pixel = uint2(position.xy) - uint2(outputX, 0);
    float4 raw = nativeColor.Load(int4(sourceRect.xy + pixel, 0, 0));
    // The vendor writes only its exact crop. Never read the uninitialized outer answer.
    if (any(pixel < processingRect.xy) || any(pixel >= processingRect.xy + processingRect.zw) || all(raw.rgb == 0))
        return raw;
    float3 answerRaw = answer.Load(int3(uint2(position.xy), 0)).rgb;
    if (all(raw.rgb == answerRaw))
        return raw;
    float3 nativeLinear = Decode(raw.rgb);
    float3 residual = Decode(answerRaw) - nativeLinear;
    float3 protectedResidual = ProtectNrResidual(nativeLinear, nativeLinear, residual);
    // Above the toe preserve original answer bytes, avoiding an unnecessary transfer round trip.
    if (all(protectedResidual == residual))
        return float4(answerRaw, raw.a);
    return float4(Encode(nativeLinear + protectedResidual), raw.a);
}
