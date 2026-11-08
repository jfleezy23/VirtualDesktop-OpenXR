// D3: prefer an existing phase-warped endpoint only when genuine A/B depth
// supports it and the unwarped fallback belongs to a different layer.
// Also validate the original B-anchored flow correspondence: a phase pair
// alone can accidentally match two unrelated remote background locations.
// This does not estimate new motion, infer hidden surfaces, or produce depth.
Texture2D<float> DepthPrevious : register(t7);
Texture2D<float> DepthCurrent : register(t8);
cbuffer DepthGuidanceParameters : register(b1) {
    uint4 DepthHeader; // flags: enabled=1, compositionally opaque=2; reserved x3
    float4 DepthPreviousToCurrentRotation;
    float4 DepthPreviousToCurrentTranslation;
};

struct DepthFootprint {
    float inverseZ;
    bool valid;
};

MappedCoordinate depth_original_coordinate(float2 target, uint view, MappedCoordinate baseline) {
    CameraMapping mapping = PreviousMappings[view];
    MappedCoordinate result = baseline;
    // Exact identity is common with a stationary camera. The generic tangent
    // round trip can move an integer texel by an ULP across a depth edge.
    // Preserve the mathematical identity, without an epsilon or edge clamp.
    if (all(mapping.TargetToSourceRotation.xyz == 0.0) &&
        abs(mapping.TargetToSourceRotation.w) == 1.0 &&
        all(mapping.SourceTangents == mapping.TargetTangents) &&
        all(mapping.SourceRect == mapping.TargetRect) && all(mapping.SourceRect.zw > 0.0)) {
        result.coordinate = target;
        result.valid = all(target >= mapping.SourceRect.xy) &&
            all(target <= mapping.SourceRect.xy + mapping.SourceRect.zw - 1.0) ? 1.0 : 0.0;
    }
    return result;
}

DepthFootprint depth_footprint(float2 coordinate, uint slice, bool previous, float4 rect) {
    DepthFootprint result = (DepthFootprint)0;
    bool inBounds = !(any((asuint(coordinate) & 0x7f800000u) == 0x7f800000u) || any(coordinate < rect.xy) ||
        any(coordinate > rect.xy + rect.zw - 1.0) || any(coordinate < 0.0) ||
        coordinate.x > float(Width - 1u) || coordinate.y > float(Height - 1u));
    if (inBounds) {
        int2 lo = int2(floor(coordinate));
        int2 hi = min(lo + 1, int2(int(Width) - 1, int(Height) - 1));
        float2 f = coordinate - float2(lo);
        float weights[4] = {(1.0-f.x)*(1.0-f.y), f.x*(1.0-f.y), (1.0-f.x)*f.y, f.x*f.y};
        int2 positions[4] = {lo, int2(hi.x,lo.y), int2(lo.x,hi.y), hi};
        float minimum = 3.402823466e+38;
        float maximum = 0.0;
        float weighted = 0.0;
        bool tapsValid = true;
        [unroll] for (uint i = 0u; i < 4u; ++i) {
            if (weights[i] > 0.0) {
                int2 location = positions[i] + int2(int(slice * Width), 0);
                float value = previous ? DepthPrevious.Load(int3(location, 0)) : DepthCurrent.Load(int3(location, 0));
                bool tapValid = !((asuint(value) & 0x7f800000u) == 0x7f800000u || value < 0.0);
                tapsValid = tapsValid && tapValid;
                if (tapValid) {
                    minimum = min(minimum, value);
                    maximum = max(maximum, value);
                    weighted += weights[i] * value;
                }
            }
        }
        // Infinity is a direction, never an enormous finite point. Reject a
        // mixed finite/infinite footprint rather than growing its foreground.
        bool continuous = !((minimum == 0.0 && maximum != 0.0) ||
            (minimum > 0.0 && maximum > minimum * 1.03));
        bool finiteWeighted = (asuint(weighted) & 0x7f800000u) != 0x7f800000u &&
            (minimum == 0.0 || weighted > 0.0);
        if (tapsValid && continuous && finiteWeighted) {
            result.inverseZ = weighted;
            result.valid = true;
        }
    }
    return result;
}

bool depth_same_layer(float a, float b) {
    return (a == 0.0 || b == 0.0) ? (a == 0.0 && b == 0.0) : (max(a, b) <= min(a, b) * 1.03);
}

bool depth_opaque_footprint(float2 coordinate, uint slice, bool previous, float4 rect) {
    bool opaque = !any((asuint(coordinate) & 0x7f800000u) == 0x7f800000u) &&
        all(coordinate >= rect.xy) && all(coordinate <= rect.xy + rect.zw - 1.0) &&
        all(coordinate >= 0.0) && all(coordinate <= float2(float(Width-1u),float(Height-1u)));
    if (opaque && (DepthHeader.x & 2u) == 0u) {
        int2 lo = int2(floor(coordinate));
        int2 hi = min(lo + 1, int2(int(Width) - 1, int(Height) - 1));
        float2 f = coordinate - float2(lo);
        float weights[4] = {(1.0-f.x)*(1.0-f.y), f.x*(1.0-f.y), (1.0-f.x)*f.y, f.x*f.y};
        int2 positions[4] = {lo, int2(hi.x,lo.y), int2(lo.x,hi.y), hi};
        [unroll] for (uint i=0u; i<4u; ++i) {
            if (weights[i] > 0.0) {
                float alpha = previous ? PreviousFrame.Load(int4(positions[i],int(slice),0)).a :
                                         CurrentFrame.Load(int4(positions[i],int(slice),0)).a;
                opaque = opaque && alpha == 1.0;
            }
        }
    }
    return opaque;
}

DepthFootprint depth_in_current(DepthFootprint source, float2 coordinate, uint view) {
    DepthFootprint result = source;
    if (source.valid) {
        CameraMapping mapping = PreviousMappings[view];
        float2 uv = (coordinate - mapping.SourceRect.xy + 0.5) / mapping.SourceRect.zw;
        float3 ray = float3(lerp(mapping.SourceTangents.x, mapping.SourceTangents.y, uv.x),
                           lerp(mapping.SourceTangents.z, mapping.SourceTangents.w, uv.y), -1.0);
        float3 direction = rotate_by_quaternion(DepthPreviousToCurrentRotation, ray);
        if (source.inverseZ == 0.0) {
            result.valid = direction.z < 0.0 &&
                !any((asuint(direction) & 0x7f800000u) == 0x7f800000u);
        } else {
            float3 surfacePoint = direction / source.inverseZ + DepthPreviousToCurrentTranslation.xyz;
            result.valid = !(any((asuint(surfacePoint) & 0x7f800000u) == 0x7f800000u) || surfacePoint.z >= 0.0);
            if (result.valid) {
                result.inverseZ = -1.0 / surfacePoint.z;
                result.valid = (asuint(result.inverseZ) & 0x7f800000u) != 0x7f800000u && result.inverseZ > 0.0;
            }
        }
    }
    return result;
}

float4 depth_endpoint_preference(float2 pixel, float2 backward, float fraction,
                                uint slice, uint view, float support, float photo,
                                float q, float cameraError, float phaseError, float4 previousColor, float4 currentColor,
                                float4 fallbackColor, float4 legacy) {
    float4 result = legacy;
    bool eligible = !((DepthHeader.x & 1u) == 0u || repeated_capture_flag() != 0u ||
        !(support > 0.0) || photo != 1.0 || phaseError != 0.0 || !(q > 0.0 && q < 1.0) ||
        ((DepthHeader.x & 2u) == 0u &&
         (previousColor.a != 1.0 || currentColor.a != 1.0 || fallbackColor.a != 1.0)));
    if (eligible) {
        float2 b = pixel - (1.0 - fraction) * backward;
        MappedCoordinate colourA = map_target_to_source(pixel + fraction * backward, PreviousMappings[view]);
        MappedCoordinate a = depth_original_coordinate(pixel + fraction * backward,view,colourA);
        if (a.valid >= 0.5) {
            CameraMapping mapping = PreviousMappings[view];
            DepthFootprint da = depth_footprint(a.coordinate, slice, true, mapping.SourceRect);
            DepthFootprint db = depth_footprint(b, slice, false, mapping.TargetRect);
            DepthFootprint dp = depth_footprint(pixel, slice, false, mapping.TargetRect);
            if (da.valid && db.valid && dp.valid) {
                DepthFootprint transformed = depth_in_current(da,a.coordinate,view);
                MappedCoordinate colourAnchor = map_target_to_source(pixel + backward, mapping);
                MappedCoordinate anchor = depth_original_coordinate(pixel + backward,view,colourAnchor);
                MappedCoordinate camera = map_target_to_source(pixel, mapping);
                DepthFootprint original = (DepthFootprint)0;
                if (anchor.valid >= 0.5)
                    original = depth_in_current(depth_footprint(anchor.coordinate,slice,true,mapping.SourceRect),anchor.coordinate,view);
                if (transformed.valid && original.valid && depth_same_layer(original.inverseZ,dp.inverseZ) &&
                    depth_same_layer(transformed.inverseZ, db.inverseZ) &&
                    camera.valid >= 0.5 && depth_opaque_footprint(camera.coordinate,slice,true,mapping.SourceRect) &&
                    depth_opaque_footprint(colourA.coordinate,slice,true,mapping.SourceRect) &&
                    depth_opaque_footprint(colourAnchor.coordinate,slice,true,mapping.SourceRect) &&
                    depth_opaque_footprint(b,slice,false,mapping.TargetRect) &&
                    depth_opaque_footprint(pixel,slice,false,mapping.TargetRect) &&
                    !depth_same_layer(dp.inverseZ, db.inverseZ)) {
                    CameraSample anchoredColor = sample_previous_target(pixel + backward,slice,view);
                    float anchoredError = rgb_error(pack_flow_input_color(anchoredColor.color),
                                                    pack_flow_input_color(fallbackColor));
                    // Depth agreement is not material correspondence. Do not
                    // override the hardware guard when the original match
                    // explains appearance no better than the camera alone.
                    if (anchoredColor.valid >= 0.5 && anchoredError < cameraError)
                        result.rgb = saturate(currentColor.rgb);
                }
            }
        }
    }
    return result;
}
