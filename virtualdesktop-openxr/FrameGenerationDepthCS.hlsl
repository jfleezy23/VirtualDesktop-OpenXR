// Canonical guide values: inverse view-axis meters, zero at a valid
// infinite endpoint, -1 for unavailable samples. No filtering or readback.
Texture2DArray<float> OriginalDepth : register(t0);
RWTexture2D<float> CanonicalDepth : register(u0);
cbuffer DepthParameters : register(b0) {
    float4 OriginalColour; // x,y,width,height in the original color raster.
    float4 ColourCrop;     // verified affine crop represented by the final eye.
    float4 DepthRect;
    float4 DepthRange; // min,max,reciprocal(nearZ),reciprocal(farZ).
    uint4 Output;      // final eye width,height,atlas eye,infinite endpoint bits.
};

// Exact floor of the pixel-center affine map. The host bounds all extents
// and rectangle endpoints to 16384. Decomposing the rational expression
// keeps uNum < 2^29, wholeMap < 2^28, fracNumer < 2^30 and fracDen <= 2^29.
// Integer depth edges are nearest-center ties and select the upper texel.
uint MapDepthAxis(uint pixel,
                  uint outputExtent,
                  uint colourOffset,
                  uint colourExtent,
                  uint cropOffset,
                  uint cropExtent,
                  uint depthOffset,
                  uint depthExtent) {
    uint result = depthOffset + pixel;
    [branch] if (cropOffset != colourOffset || cropExtent != colourExtent || depthExtent != colourExtent ||
                 outputExtent != cropExtent) {
        const uint uNum = (2 * pixel + 1) * cropExtent;
        const uint uDen = 2 * outputExtent;
        const uint wholeColour = uNum / uDen;
        const uint remColour = uNum % uDen;
        const uint wholeMap = ((cropOffset - colourOffset) + wholeColour) * depthExtent;
        const uint wholeDepth = wholeMap / colourExtent;
        const uint remDepth = wholeMap % colourExtent;
        const uint fracNumer = remDepth * uDen + remColour * depthExtent;
        const uint fracDen = uDen * colourExtent;
        result = depthOffset + wholeDepth + fracNumer / fracDen;
    }
    return result;
}

// Exceptional binary32 intervals need integer decoding: SM5 arithmetic
// flushes subnormal operands and differences. A common power-of-two scale
// makes every nonzero admitted [0,1] sample and difference normal.
float ScaledDepth(uint bits, uint maximumExponent) {
    uint exponent = bits >> 23;
    uint significand = (bits & 0x7fffffu) | (exponent != 0u ? 0x800000u : 0u);
    return float(significand) * asfloat((127u + max(exponent, 1u) - maximumExponent) << 23);
}

void SplitPositive(float value, out float mantissa, out int exponent) {
    uint bits = asuint(value);
    uint encoded = bits >> 23;
    uint fraction = bits & 0x7fffffu;
    if (encoded != 0u) {
        mantissa = asfloat(0x3f800000u | fraction);
        exponent = int(encoded) - 127;
    } else if (fraction != 0u) {
        int leading = firstbithigh(fraction);
        mantissa = float(fraction) / asfloat(uint(127 + leading) << 23);
        exponent = leading - 149;
    } else {
        mantissa = 0.0;
        exponent = -512;
    }
}

float ScaledInverseZ(uint raw, uint minimum, uint maximum) {
    uint commonExponent = max(maximum >> 23, 1u);
    float lo = ScaledDepth(minimum, commonExponent);
    float hi = ScaledDepth(maximum, commonExponent);
    float value = ScaledDepth(raw, commonExponent);
    float2 numerator = float2(hi - value, value - lo);
    float2 products;
    int2 exponents;
    [unroll] for (uint i = 0u; i < 2u; ++i) {
        float a, b;
        int ea, eb;
        SplitPositive(numerator[i], a, ea);
        SplitPositive(i == 0u ? DepthRange.z : DepthRange.w, b, eb);
        products[i] = a * b;
        exponents[i] = products[i] > 0.0 ? ea + eb : -512;
    }
    int common = max(exponents.x, exponents.y);
    // Both endpoint reciprocals cannot be zero for an admitted range. Retain
    // unavailable if finite reciprocal underflow nevertheless erased both.
    if (common == -512)
        return -1.0;
    float sum = 0.0;
    [unroll] for (uint j = 0u; j < 2u; ++j) {
        int delta = exponents[j] - common;
        // Positive terms cannot cancel; a term this far below the largest
        // cannot affect its binary32 rounding. Avoid an invalid exponent.
        if (delta >= -126)
            sum += products[j] * asfloat(uint(127 + delta) << 23);
    }
    float denominatorMantissa;
    int denominatorExponent;
    SplitPositive(hi - lo, denominatorMantissa, denominatorExponent);
    float quotient = sum / denominatorMantissa;
    uint quotientBits = asuint(quotient);
    int resultExponent = int(quotientBits >> 23) + common - denominatorExponent;
    // Canonical guides represent finite positive normal values or explicit
    // infinity endpoints. Apply the final exponent once, after summation.
    return resultExponent > 0 && resultExponent < 255
               ? asfloat((quotientBits & 0x7fffffu) | (uint(resultExponent) << 23))
               : -1.0;
}

[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Output.xy))
        return;
    // These integer-valued constants are exactly representable in float32.
    const uint4 colour = uint4(OriginalColour);
    const uint4 crop = uint4(ColourCrop);
    const uint4 depth = uint4(DepthRect);
    const uint2 pixel = uint2(MapDepthAxis(id.x, Output.x, colour.x, colour.z, crop.x, crop.z, depth.x, depth.z),
                              MapDepthAxis(id.y, Output.y, colour.y, colour.w, crop.y, crop.w, depth.y, depth.w));
    float inverseZ = -1.0;
    if (all(pixel >= depth.xy) && all(pixel < depth.xy + depth.zw)) {
        const float raw = OriginalDepth.Load(int4(pixel, 0, 0));
        uint rawBits = asuint(raw);
        rawBits = (rawBits & 0x7fffffffu) == 0u ? 0u : rawBits;
        uint minimum = asuint(DepthRange.x) & 0x7fffffffu;
        uint maximum = asuint(DepthRange.y);
        // Nonnegative finite float bits are monotonically ordered. This also
        // rejects negative subnormals and NaNs without FTZ changing admission.
        if (rawBits >= minimum && rawBits <= maximum) {
            bool atMinimum = rawBits == minimum;
            bool atMaximum = rawBits == maximum;
            bool infinity = (atMinimum && (Output.w & 1u) != 0u) || (atMaximum && (Output.w & 2u) != 0u);
            if (atMinimum || atMaximum) {
                float endpoint = atMinimum ? DepthRange.z : DepthRange.w;
                inverseZ = infinity ? 0.0 : endpoint > 0.0 ? endpoint : -1.0;
            } else {
                const float u = (raw - DepthRange.x) / (DepthRange.y - DepthRange.x);
                const float complement = (DepthRange.y - raw) / (DepthRange.y - DepthRange.x);
                const float nearTerm = complement * DepthRange.z;
                const float farTerm = u * DepthRange.w;
                float candidate = nearTerm + farTerm;
                // Ordinary depth keeps the existing arithmetic. The rare fallback
                // must also avoid underflowing a weight before a large reciprocal
                // restores its contribution to a representable finite depth.
                [branch] if (!isfinite(u) || !isfinite(complement) || !(u > 0.0 && u < 1.0) ||
                             !(complement > 0.0 && complement < 1.0) || (minimum != 0u && minimum < 0x00800000u) ||
                             (rawBits != 0u && rawBits < 0x00800000u) ||
                             (asuint(DepthRange.z) != 0u && nearTerm == 0.0) ||
                             (asuint(DepthRange.w) != 0u && farTerm == 0.0)) candidate =
                    ScaledInverseZ(rawBits, minimum, maximum);
                if (isfinite(candidate) && candidate > 0.0)
                    inverseZ = candidate;
            }
        }
    }
    CanonicalDepth[uint2(id.x + Output.z * Output.x, id.y)] = inverseZ;
}
