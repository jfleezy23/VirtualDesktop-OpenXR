// Both eyes are present in every b1 record: doublewide packing is one dispatch.
Texture2D<float> DepthPrevious : register(t7);
Texture2D<float> DepthCurrent : register(t8);
#ifdef XRFG_DEPTH_AVAILABILITY
ByteAddressBuffer DepthFrameAvailability : register(t9);
#endif
cbuffer DepthGuidanceParameters : register(b1) {
    uint4 DepthHeader;
    float4 DepthPreviousToCurrentRotation[2];
    float4 DepthPreviousToCurrentTranslation[2];
};

// One uniform pair decision is shared by packing, flow support and every
// synthesis role. Per-pixel invalidity remains the existing safety policy.
bool reconstruction_available() {
    bool enabled = (DepthHeader.x & 1u) != 0u;
#ifdef XRFG_DEPTH_AVAILABILITY
    enabled = enabled && (DepthFrameAvailability.Load(0u) & DepthFrameAvailability.Load(4u)) != 0u;
#endif
    return enabled;
}

// Keep all quaternion components on the same eye. FXC's optimized dynamic
// cbuffer-array indexing can assign xyz and w different temporary indices
// across the history/projection branches; constant-index selection avoids it.
float4 reconstruction_rotation(uint view) {
    return view == 0u ? DepthPreviousToCurrentRotation[0] : DepthPreviousToCurrentRotation[1];
}

float3 reconstruction_translation(uint view) {
    return view == 0u ? DepthPreviousToCurrentTranslation[0].xyz : DepthPreviousToCurrentTranslation[1].xyz;
}

struct ReconstructionDepth {
    float inverseZ;
    bool valid;
};

bool reconstruction_inside(float2 pixel, float4 rect) {
    return !any((asuint(pixel) & 0x7f800000u) == 0x7f800000u) &&
        all(pixel >= rect.xy) && all(pixel <= rect.xy + rect.zw - 1.0) &&
        all(pixel >= 0.0) && all(pixel <= float2(Width - 1u, Height - 1u));
}

bool reconstruction_same_layer(float a, float b) {
    return (a == 0.0 || b == 0.0) ? a == b : max(a, b) <= min(a, b) * 1.03;
}

ReconstructionDepth reconstruction_depth(float2 pixel, uint slice, bool previous, float4 rect) {
    ReconstructionDepth result = (ReconstructionDepth)0;
    if (reconstruction_inside(pixel, rect)) {
    int2 lo = int2(floor(pixel));
    float2 f = pixel - float2(lo);
    float weights[4] = {(1.0-f.x)*(1.0-f.y), f.x*(1.0-f.y), (1.0-f.x)*f.y, f.x*f.y};
    int2 positions[4] = {lo, lo+int2(1,0), lo+int2(0,1), lo+int2(1,1)};
    float minimum = 3.402823466e+38;
    float maximum = 0.0;
    float weighted = 0.0;
    bool valid = true;
    [unroll] for (uint i=0u; i<4u; ++i) {
        if (weights[i] > 0.0) {
            if (!reconstruction_inside(float2(positions[i]), rect)) {
                valid = false;
            } else {
                int2 location = positions[i] + int2(int(slice * Width), 0);
                float value = previous ? DepthPrevious.Load(int3(location,0)) : DepthCurrent.Load(int3(location,0));
                bool finiteNonnegative = !((asuint(value) & 0x7f800000u) == 0x7f800000u || value < 0.0);
                valid = valid && finiteNonnegative;
                if (finiteNonnegative) {
                    minimum = min(minimum, value);
                    maximum = max(maximum, value);
                    weighted += weights[i] * value;
                }
            }
        }
    }
    result.valid = valid && reconstruction_same_layer(minimum,maximum) &&
        (asuint(weighted) & 0x7f800000u) != 0x7f800000u && (minimum == 0.0 || weighted > 0.0);
    result.inverseZ = weighted;
    }
    return result;
}

bool reconstruction_opaque(float2 pixel, uint slice, bool previous, float4 rect) {
    bool opaque = reconstruction_inside(pixel,rect);
    if (opaque && (DepthHeader.x & 2u) == 0u) {
    int2 lo = int2(floor(pixel));
    float2 f = pixel - float2(lo);
    float weights[4] = {(1.0-f.x)*(1.0-f.y), f.x*(1.0-f.y), (1.0-f.x)*f.y, f.x*f.y};
    int2 positions[4] = {lo, lo+int2(1,0), lo+int2(0,1), lo+int2(1,1)};
    [unroll] for (uint i=0u; i<4u; ++i) {
        if (weights[i] > 0.0) {
            if (!reconstruction_inside(float2(positions[i]),rect)) opaque = false;
            else {
                float a = previous ? PreviousFrame.Load(int4(positions[i],int(slice),0)).a :
                                     CurrentFrame.Load(int4(positions[i],int(slice),0)).a;
                opaque = opaque && a == 1.0;
            }
        }
    }
    }
    return opaque;
}

float3 reconstruction_ray(float2 pixel, float4 rect, float4 tangents) {
    float2 uv = (pixel - rect.xy + 0.5) / rect.zw;
    return float3(lerp(tangents.x,tangents.y,uv.x),lerp(tangents.z,tangents.w,uv.y),-1.0);
}

MappedCoordinate reconstruction_project_ray(float3 ray, float4 rect, float4 tangents) {
    MappedCoordinate result = (MappedCoordinate)0;
    if (!any((asuint(ray) & 0x7f800000u) == 0x7f800000u) && ray.z < 0.0) {
    float2 uv = (ray.xy / -ray.z - tangents.xz) / (tangents.yw - tangents.xz);
    result.coordinate = rect.xy + uv * rect.zw - 0.5;
    result.valid = reconstruction_inside(result.coordinate,rect) ? 1.0 : 0.0;
    }
    return result;
}

struct ReconstructionProjection {
    float2 coordinate;
    float expectedInverseZ;
    bool valid;
};

ReconstructionProjection reconstruction_project(float2 pixel, float inverseZ, uint view) {
    ReconstructionProjection result = (ReconstructionProjection)0;
    CameraMapping mapping = PreviousMappings[view];
    if (reconstruction_inside(pixel,mapping.TargetRect) &&
        (asuint(inverseZ) & 0x7f800000u) != 0x7f800000u && inverseZ >= 0.0) {
    float4 rotation = reconstruction_rotation(view);
    float3 translation = reconstruction_translation(view);
    if (all(rotation.xyz == 0.0) && abs(rotation.w) == 1.0 && all(translation == 0.0) &&
        all(mapping.SourceTangents == mapping.TargetTangents) && all(mapping.SourceRect == mapping.TargetRect)) {
        result.coordinate = pixel;
        result.expectedInverseZ = inverseZ;
        result.valid = true;
    } else {
    float3 ray = reconstruction_ray(pixel,mapping.TargetRect,mapping.TargetTangents);
    // Homogeneous inverse-depth form is finite at infinity and never builds 1/rho.
    float3 h = rotate_by_quaternion(float4(-rotation.xyz,rotation.w),
        ray - inverseZ * translation);
    MappedCoordinate mapped = reconstruction_project_ray(h,mapping.SourceRect,mapping.SourceTangents);
    if (mapped.valid >= 0.5) {
        result.coordinate = mapped.coordinate;
        result.expectedInverseZ = inverseZ / -h.z;
        result.valid = (asuint(result.expectedInverseZ) & 0x7f800000u) != 0x7f800000u &&
            (inverseZ == 0.0 || result.expectedInverseZ > 0.0);
    }
    }
    }
    return result;
}

#ifdef XRFG_A_OWNED_DEPTH_HISTORY
#include "depth_reconstruction_a_owned.hlsli"
#endif

MappedCoordinate reconstruction_history_coordinate(float2 pixel, uint slice, uint view) {
    CameraMapping mapping = PreviousMappings[view];
    MappedCoordinate legacy = map_target_to_source(pixel,mapping);
    MappedCoordinate result = legacy;
#ifdef XRFG_DEPTH_AVAILABILITY
    // Gate before every identity/rotation shortcut and translated gather.
    if (reconstruction_available()) {
#endif
    float4 rotation = reconstruction_rotation(view);
    float3 translation = reconstruction_translation(view);
    bool noTranslation = all(translation == 0.0);
    if ((DepthHeader.x & 1u) != 0u && noTranslation && all(rotation.xyz == 0.0) && abs(rotation.w) == 1.0 &&
        all(mapping.SourceTangents == mapping.TargetTangents) && all(mapping.SourceRect == mapping.TargetRect)) {
        result.coordinate = pixel;
        result.valid = reconstruction_inside(pixel,mapping.SourceRect) ? 1.0 : 0.0;
    } else if ((DepthHeader.x & 1u) != 0u && noTranslation) {
        ReconstructionProjection direction = reconstruction_project(pixel,0.0,view);
        result.coordinate = direction.coordinate;
        result.valid = direction.valid ? 1.0 : 0.0;
    } else if ((DepthHeader.x & 1u) != 0u && !noTranslation
#ifndef XRFG_A_OWNED_DEPTH_HISTORY
               && reconstruction_opaque(pixel,slice,false,mapping.TargetRect)
#endif
               ) {
#ifdef XRFG_A_OWNED_DEPTH_HISTORY
        result = reconstruction_a_owned_history(pixel,slice,view);
#else
    // Rotation/FOV is independent of scene depth. Do not erase object motion.
    result = (MappedCoordinate)0;
    ReconstructionDepth db = reconstruction_depth(pixel,slice,false,mapping.TargetRect);
    if (db.valid) {
        ReconstructionProjection projected = reconstruction_project(pixel,db.inverseZ,view);
        if (projected.valid) {
            ReconstructionDepth da = reconstruction_depth(projected.coordinate,slice,true,mapping.SourceRect);
            if (da.valid && reconstruction_same_layer(da.inverseZ,projected.expectedInverseZ)) {
                if (!reconstruction_opaque(projected.coordinate,slice,true,mapping.SourceRect)) result = legacy;
                else {
                    float3 rayA = reconstruction_ray(projected.coordinate,mapping.SourceRect,mapping.SourceTangents);
                    float3 hB = rotate_by_quaternion(rotation,rayA) + da.inverseZ * translation;
                    MappedCoordinate returned = reconstruction_project_ray(hB,mapping.TargetRect,mapping.TargetTangents);
                    if (returned.valid >= 0.5 && distance(returned.coordinate,pixel) <= 0.5) {
                        result.coordinate = projected.coordinate;
                        result.valid = 1.0;
                    }
                }
            }
        }
    }
#endif
    }
#ifdef XRFG_DEPTH_AVAILABILITY
    }
#endif
    return result;
}

float reconstruction_cell_support(float2 pixel, int2 cell, uint slice, uint view, ReconstructionDepth target, bool depthGate) {
    CameraMapping mapping = PreviousMappings[view];
    float support = 1.0;
    if (depthGate) {
    float2 center = ((float2(cell)+0.5)*float(FlowBlockSize) -
        float2(0.0,float(slice*PackedEyeStride))) / flow_input_scale() - 0.5;
    ReconstructionDepth contributor = reconstruction_depth(center,slice,false,mapping.TargetRect);
    support = target.valid && contributor.valid && reconstruction_same_layer(target.inverseZ,contributor.inverseZ) ? 1.0 : 0.0;
    }
    return support;
}
