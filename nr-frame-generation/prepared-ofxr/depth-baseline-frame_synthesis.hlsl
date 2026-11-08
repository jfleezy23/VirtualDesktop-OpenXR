Texture2DArray<float4> PreviousFrame : register(t0);
SamplerState LinearClamp : register(s0);
Texture2DArray<float4> CurrentFrame : register(t1);
Texture2D<int2> BackwardFlow : register(t2);
Texture2D<uint> FlowAuxiliary : register(t3);
Texture2D<int2> ForwardFlow : register(t4);
Texture2D<uint> ForwardAuxiliary : register(t5);
Texture2DArray<float2> GameMotionVectors : register(t6);
RWTexture2D<float4> PackedColor : register(u0);
// The NVIDIA pack is compiled twice: once for BGRA8 inputs, and once with
// XRFG_NVIDIA_LUMA_INPUT for R8 inputs, which the engine takes as grayscale
// and which is a quarter of the bytes for the same flow - it matches on
// luma either way. The C++ side picks the pipeline by the format the driver
// accepted at creation.
#ifdef XRFG_NVIDIA_LUMA_INPUT
RWTexture2D<float> NvidiaPreviousColor : register(u1);
RWTexture2D<float> NvidiaCurrentColor : register(u2);
#else
RWTexture2D<float4> NvidiaPreviousColor : register(u1);
RWTexture2D<float4> NvidiaCurrentColor : register(u2);
#endif

struct CameraMapping {
    float4 TargetToSourceRotation;
    float4 SourceTangents;
    float4 TargetTangents;
    float4 SourceRect;
    float4 TargetRect;
};

cbuffer SynthesisParameters : register(b0) {
    uint Width;
    uint Height;
    uint ArraySize;
    uint PackedWidth;
    uint PackedHeight;
    uint PackedEyeStride;
    uint FlowWidth;
    uint FlowHeight;
    uint FlowBlockSize;
    uint ViewIndex;
    uint UseGameMotion;
    uint GameMotionPadding;
    float4 GameMotionRect;
    float2 GameMotionScale;
    float2 GameMotionJitterDelta;
    CameraMapping PreviousMappings[2];
    uint Slice;
    // Bit 0 is the repeated-capture flag. Bit 1 asks the pack shaders to
    // sRGB-encode the flow input. Bits 8-23 carry the synthetic's position
    // between the two captures in 1/65535ths - 0 at the previous capture, 1
    // at the current. They share a slot because the root signature is full:
    // 62 constants plus two descriptor tables is the whole 64-DWORD budget,
    // leaving no room for a 63rd.
    uint SynthesisFlags;
};

uint repeated_capture_flag() {
    return SynthesisFlags & 1u;
}

// The source views are the swapchain's own format. On an sRGB swapchain a
// Load returns linear light, which is what the composition needs to write
// back through its sRGB render target - but written as-is into the 8-bit
// flow input it leaves the darkest tenth of the perceptual range in two or
// three codes, and the optical flow matches on that. Filtering and the luma
// weighting stay in linear light; the encode is applied to the packed value.
bool encode_srgb_flow_input() {
    return (SynthesisFlags & 2u) != 0u;
}

float3 linear_to_srgb(float3 linear_color) {
    float3 low = linear_color * 12.92;
    float3 high = 1.055 * pow(max(linear_color, 0.0), 1.0 / 2.4) - 0.055;
    return linear_color <= 0.0031308 ? low : high;
}

float4 pack_flow_input_color(float4 color) {
    float4 packed = saturate(color);
    if (encode_srgb_flow_input()) {
        packed.rgb = linear_to_srgb(packed.rgb);
    }
    return packed;
}

// Rec. 709 weights give relative luminance from linear values; the encode
// then spreads it over the codes the way the swapchain spreads colour.
float pack_flow_input_luma(float4 color) {
    const float3 luma_weights = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(saturate(color).rgb, luma_weights);
    if (encode_srgb_flow_input()) {
        luma = linear_to_srgb(luma.xxx).x;
    }
    return luma;
}

// An unset field decodes to 0.5, which is the fixed midpoint this shader
// used before the layer began deriving it.
float synthesis_fraction() {
    uint packed = (SynthesisFlags >> 8) & 0xffffu;
    return packed == 0u ? 0.5 : float(packed) / 65535.0;
}

float3 rotate_by_quaternion(float4 quaternion, float3 input_vector) {
    return input_vector + 2.0 * cross(
        quaternion.xyz,
        cross(quaternion.xyz, input_vector) + quaternion.w * input_vector);
}

struct MappedCoordinate {
    float2 coordinate;
    float valid;
    float padding;
};

MappedCoordinate map_target_to_source(
    float2 target_coordinate,
    CameraMapping mapping) {
    MappedCoordinate output;
    output.coordinate = float2(0.0, 0.0);
    output.valid = 0.0;
    output.padding = 0.0;
    float2 target_minimum = mapping.TargetRect.xy;
    float2 target_extent = mapping.TargetRect.zw;
    float2 target_maximum = target_minimum + target_extent - 1.0;
    bool target_inside =
        target_coordinate.x >= target_minimum.x &&
        target_coordinate.y >= target_minimum.y &&
        target_coordinate.x <= target_maximum.x &&
        target_coordinate.y <= target_maximum.y;

    float2 target_uv =
        (target_coordinate - target_minimum + 0.5) / target_extent;
    float2 target_tangent = float2(
        lerp(mapping.TargetTangents.x, mapping.TargetTangents.y, target_uv.x),
        lerp(mapping.TargetTangents.z, mapping.TargetTangents.w, target_uv.y));
    float3 target_ray = float3(target_tangent, -1.0);
    float3 source_ray = rotate_by_quaternion(
        mapping.TargetToSourceRotation,
        target_ray);
    bool source_in_front = source_ray.z < -1.0e-5;
    float inverse_depth = 1.0 / max(-source_ray.z, 1.0e-5);
    float2 source_tangent = source_ray.xy * inverse_depth;
    float source_width = mapping.SourceTangents.y - mapping.SourceTangents.x;
    float source_height = mapping.SourceTangents.w - mapping.SourceTangents.z;
    bool valid_fov = source_width > 1.0e-7 && abs(source_height) > 1.0e-7;
    source_width = max(source_width, 1.0e-7);
    source_height = source_height < 0.0
                        ? min(source_height, -1.0e-7)
                        : max(source_height, 1.0e-7);
    float2 source_uv = float2(
        (source_tangent.x - mapping.SourceTangents.x) / source_width,
        (source_tangent.y - mapping.SourceTangents.z) / source_height);
    float2 source_minimum = mapping.SourceRect.xy;
    float2 source_extent = mapping.SourceRect.zw;
    float2 source_maximum = source_minimum + source_extent;
    output.coordinate = source_minimum + source_uv * source_extent - 0.5;
    output.valid = target_inside && source_in_front && valid_fov &&
        output.coordinate.x >= source_minimum.x - 0.5 &&
        output.coordinate.y >= source_minimum.y - 0.5 &&
        output.coordinate.x <= source_maximum.x - 0.5 &&
        output.coordinate.y <= source_maximum.y - 0.5
            ? 1.0
            : 0.0;
    return output;
}

MappedCoordinate map_source_to_target(
    float2 source_coordinate,
    CameraMapping mapping) {
    MappedCoordinate output;
    output.coordinate = float2(0.0, 0.0);
    output.valid = 0.0;
    output.padding = 0.0;
    float2 source_minimum = mapping.SourceRect.xy;
    float2 source_extent = mapping.SourceRect.zw;
    float2 source_maximum = source_minimum + source_extent - 1.0;
    bool source_inside =
        source_coordinate.x >= source_minimum.x &&
        source_coordinate.y >= source_minimum.y &&
        source_coordinate.x <= source_maximum.x &&
        source_coordinate.y <= source_maximum.y;

    float2 source_uv =
        (source_coordinate - source_minimum + 0.5) / source_extent;
    float2 source_tangent = float2(
        lerp(mapping.SourceTangents.x, mapping.SourceTangents.y, source_uv.x),
        lerp(mapping.SourceTangents.z, mapping.SourceTangents.w, source_uv.y));
    float3 source_ray = float3(source_tangent, -1.0);
    float4 source_to_target_rotation = float4(
        -mapping.TargetToSourceRotation.xyz,
        mapping.TargetToSourceRotation.w);
    float3 target_ray = rotate_by_quaternion(
        source_to_target_rotation,
        source_ray);
    bool target_in_front = target_ray.z < -1.0e-5;
    float inverse_depth = 1.0 / max(-target_ray.z, 1.0e-5);
    float2 target_tangent = target_ray.xy * inverse_depth;
    float target_width = mapping.TargetTangents.y - mapping.TargetTangents.x;
    float target_height = mapping.TargetTangents.w - mapping.TargetTangents.z;
    bool valid_fov = target_width > 1.0e-7 && abs(target_height) > 1.0e-7;
    target_width = max(target_width, 1.0e-7);
    target_height = target_height < 0.0
                        ? min(target_height, -1.0e-7)
                        : max(target_height, 1.0e-7);
    float2 target_uv = float2(
        (target_tangent.x - mapping.TargetTangents.x) / target_width,
        (target_tangent.y - mapping.TargetTangents.z) / target_height);
    float2 target_minimum = mapping.TargetRect.xy;
    float2 target_extent = mapping.TargetRect.zw;
    float2 target_maximum = target_minimum + target_extent;
    output.coordinate = target_minimum + target_uv * target_extent - 0.5;
    output.valid = source_inside && target_in_front && valid_fov &&
        output.coordinate.x >= target_minimum.x - 0.5 &&
        output.coordinate.y >= target_minimum.y - 0.5 &&
        output.coordinate.x <= target_maximum.x - 0.5 &&
        output.coordinate.y <= target_maximum.y - 0.5
            ? 1.0
            : 0.0;
    return output;
}

float4 bilinear_previous_source(float2 coordinate, uint slice) {
    float2 bounded = clamp(
        coordinate,
        float2(0.0, 0.0),
        float2(float(Width - 1), float(Height - 1)));
    // Source coordinates are pixel centers in an eye-local array slice.
    // Caller clamps to active SourceRect remain in force before normalization.
    float2 uv = (bounded + 0.5) / float2(float(Width), float(Height));
    return PreviousFrame.SampleLevel(LinearClamp, float3(uv, float(slice)), 0.0);
}

float4 bilinear_current_source(float2 coordinate, uint slice) {
    float2 bounded = clamp(
        coordinate,
        float2(0.0, 0.0),
        float2(float(Width - 1), float(Height - 1)));
    // Source coordinates are pixel centers in an eye-local array slice.
    // Caller clamps to active SourceRect remain in force before normalization.
    float2 uv = (bounded + 0.5) / float2(float(Width), float(Height));
    return CurrentFrame.SampleLevel(LinearClamp, float3(uv, float(slice)), 0.0);
}

// The nominal uniform scale excludes backend block/minimum-size padding.
// Zero keeps legacy injected-flow fixtures compatible; production packs
// and composition always carry an explicit scale in bits 2-3.
float2 flow_input_scale() {
    uint nominal = (SynthesisFlags >> 2u) & 3u;
    float ratio = nominal == 3u ? 0.5 : nominal == 2u ? 0.75 :
        nominal == 1u ? 1.0 : min(1.0, float(PackedWidth) / max(float(Width), 1.0));
    return float2(ratio, ratio);
}

float2 flow_input_coordinate(float2 full_resolution_coordinate) {
    return (full_resolution_coordinate + 0.5) *
        flow_input_scale() - 0.5;
}

struct CameraSample {
    float4 color;
    float valid;
    float3 padding;
};

CameraSample sample_previous_target(
    float2 target_coordinate,
    uint slice,
    uint view_index) {
    CameraSample output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.valid = 0.0;
    output.padding = float3(0.0, 0.0, 0.0);
    MappedCoordinate mapped = map_target_to_source(
        target_coordinate,
        PreviousMappings[view_index]);
    if (mapped.valid > 0.5) {
        float4 source_rect = PreviousMappings[view_index].SourceRect;
        float2 source_coordinate = clamp(mapped.coordinate,
            source_rect.xy, source_rect.xy + source_rect.zw - 1.0);
        output.color = bilinear_previous_source(source_coordinate, slice);
        output.valid = 1.0;
    }
    return output;
}

CameraSample sample_current_target(
    float2 target_coordinate,
    uint slice,
    uint view_index) {
    CameraSample output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.valid = 0.0;
    output.padding = float3(0.0, 0.0, 0.0);
    float2 target_minimum = PreviousMappings[view_index].TargetRect.xy;
    float2 target_maximum = target_minimum +
        PreviousMappings[view_index].TargetRect.zw;
    bool valid = target_coordinate.x >= target_minimum.x &&
                 target_coordinate.y >= target_minimum.y &&
                 target_coordinate.x <= target_maximum.x - 1.0 &&
                 target_coordinate.y <= target_maximum.y - 1.0;
    if (valid) {
        output.color = bilinear_current_source(target_coordinate, slice);
        output.valid = 1.0;
    }
    return output;
}

[numthreads(8, 8, 1)]
void PackFlowInput(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= PackedWidth || thread_id.y >= PackedHeight) {
        return;
    }

    float4 current_color = float4(0.0, 0.0, 0.0, 1.0);
    uint slice = thread_id.y / PackedEyeStride;
    uint local_y = thread_id.y - slice * PackedEyeStride;
    // A reduced input is resampled rather than point-sampled, so the flow
    // sees a filtered image instead of every fourth pixel.
    float2 input_scale = flow_input_scale();
    if (slice >= ArraySize) {
        PackedColor[thread_id.xy] = pack_flow_input_color(current_color);
        return;
    }
    if (any(input_scale < 1.0)) {
        float2 source_coordinate =
            (float2(float(thread_id.x), float(local_y)) + 0.5) /
                max(input_scale, float2(1.0e-6, 1.0e-6)) - 0.5;
        if (source_coordinate.x < float(Width) && source_coordinate.y < float(Height)) {
            current_color =
                bilinear_current_source(source_coordinate, slice);
        }
    } else if (thread_id.x < Width && local_y < Height) {
        current_color = CurrentFrame.Load(
            int4(int2(thread_id.x, local_y), int(slice), 0));
    }
    PackedColor[thread_id.xy] = pack_flow_input_color(current_color);
}

// The view whose target rectangle holds this pixel, for the pack, which runs
// per slice rather than per view. An array swapchain gives each view its own
// slice with the same rectangle, so among the views that contain the pixel
// the one on this slice wins; a double-wide has disjoint rectangles on one
// slice. 2 when no view claims the pixel.
uint pack_view_for_pixel(float2 target_coordinate, uint slice) {
    uint view = 2u;
    [unroll] for (uint v = 0; v < 2; ++v) {
        float4 rect = PreviousMappings[v].TargetRect;
        float2 minimum = rect.xy - 0.5;
        float2 maximum = rect.xy + rect.zw - 0.5;
        bool inside = rect.z > 0.0 && rect.w > 0.0 &&
            target_coordinate.x >= minimum.x && target_coordinate.y >= minimum.y &&
            target_coordinate.x <= maximum.x && target_coordinate.y <= maximum.y;
        if (inside && (view == 2u || v == slice)) {
            view = v;
        }
    }
    return view;
}

[numthreads(8, 8, 1)]
void PackNvidiaFlowInput(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= PackedWidth || thread_id.y >= PackedHeight) {
        return;
    }

    float4 previous_color = float4(0.0, 0.0, 0.0, 1.0);
    float4 current_color = float4(0.0, 0.0, 0.0, 1.0);
    uint slice = Slice;
    bool downscaled = any(flow_input_scale() < 1.0);
    // The previous frame reaches the engine warped into the current camera
    // through the OpenXR pose delta, so what it estimates is the residual
    // scene motion rather than the head rotation with the scene motion on
    // top. Head turns no longer exceed its search, its cost no longer reads
    // "large displacement" everywhere under rotation, and the previous
    // pair's vectors (temporal hints) stay small and correlated. The
    // composition reads the previous endpoint through the same mapping, so
    // the two agree by construction. Where this camera sees beyond the
    // previous frame the input is black; the composition already treats
    // those pixels as uncovered.
    if (slice < ArraySize) {
        float2 target_coordinate = downscaled
            ? (float2(thread_id.xy) + 0.5) / flow_input_scale() - 0.5
            : float2(thread_id.xy);
        if (target_coordinate.x < float(Width) &&
            target_coordinate.y < float(Height)) {
            uint view_index = pack_view_for_pixel(target_coordinate, slice);
            if (view_index < 2u) {
                CameraSample previous = sample_previous_target(
                    target_coordinate, slice, view_index);
                if (previous.valid >= 0.5) {
                    previous_color = previous.color;
                }
            } else {
                previous_color =
                    bilinear_previous_source(target_coordinate, slice);
            }
            bool cropped_target = false;
            if (view_index < 2u) {
                cropped_target = any(PreviousMappings[view_index].TargetRect !=
                    float4(0.0, 0.0, float(Width), float(Height)));
            }
            if (downscaled && cropped_target) {
                // Match previous target-center coverage inside a crop,
                // preserving resource-edge replication for full images.
                current_color = sample_current_target(
                    target_coordinate, slice, view_index).color;
            } else {
                current_color = downscaled
                    ? bilinear_current_source(target_coordinate, slice)
                    : CurrentFrame.Load(int4(int2(thread_id.xy), int(slice), 0));
            }
        }
    }
#ifdef XRFG_NVIDIA_LUMA_INPUT
    // One channel is a quarter of the bytes of four, and the engine matches
    // structure, not colour.
    NvidiaPreviousColor[thread_id.xy] = pack_flow_input_luma(previous_color);
    NvidiaCurrentColor[thread_id.xy] = pack_flow_input_luma(current_color);
#else
    NvidiaPreviousColor[thread_id.xy] = pack_flow_input_color(previous_color);
    NvidiaCurrentColor[thread_id.xy] = pack_flow_input_color(current_color);
#endif
}

float2 load_backward_flow_clamped(
    int2 coordinate,
    int2 minimum,
    int2 maximum) {
    return float2(BackwardFlow.Load(int3(clamp(coordinate, minimum, maximum), 0)));
}

float2 load_forward_flow_clamped(
    int2 coordinate,
    int2 minimum,
    int2 maximum) {
    return float2(ForwardFlow.Load(int3(clamp(coordinate, minimum, maximum), 0)));
}

struct FlowGridBounds {
    int2 minimum;
    int2 maximum;
};

FlowGridBounds flow_grid_bounds(
    float4 image_rect,
    uint slice) {
    uint2 image_minimum = uint2(image_rect.xy);
    uint2 image_maximum = image_minimum + uint2(image_rect.zw) - 1U;
    // Always applied: the scale is one when the input is full resolution,
    // so this is the identity on an unscaled path.
    {
        float2 input_scale = flow_input_scale();
        uint2 scaled_minimum = uint2(floor(
            float2(image_minimum) * input_scale));
        uint2 scaled_end = uint2(ceil(
            float2(image_maximum + 1U) * input_scale));
        image_minimum = scaled_minimum;
        image_maximum = max(scaled_minimum, scaled_end - 1U);
    }
    FlowGridBounds bounds;
    bounds.minimum = int2(
        int(image_minimum.x / FlowBlockSize),
        int((slice * PackedEyeStride + image_minimum.y) / FlowBlockSize));
    bounds.maximum = int2(
        int(image_maximum.x / FlowBlockSize),
        int((slice * PackedEyeStride + image_maximum.y) / FlowBlockSize));
    bounds.maximum = min(
        bounds.maximum,
        int2(int(FlowWidth) - 1, int(FlowHeight) - 1));
    return bounds;
}

struct NvidiaFlowSample {
    float2 motion;
    float support;
};

NvidiaFlowSample nvidia_confidence_flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale) {
    float2 scaled = flow_input_coordinate(pixel);
    float2 packed_coordinate = float2(
        scaled.x, scaled.y + float(slice * PackedEyeStride));
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect, slice);
    float2 bounded = clamp(
        flow_coordinate, float2(bounds.minimum), float2(bounds.maximum));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), bounds.maximum);
    float2 fraction = bounded - float2(top_left);
    float2 v00 = load_backward_flow_clamped(top_left, bounds.minimum, bounds.maximum);
    float2 v10 = load_backward_flow_clamped(
        int2(bottom_right.x, top_left.y), bounds.minimum, bounds.maximum);
    float2 v01 = load_backward_flow_clamped(
        int2(top_left.x, bottom_right.y), bounds.minimum, bounds.maximum);
    float2 v11 = load_backward_flow_clamped(bottom_right, bounds.minimum, bounds.maximum);
    uint c00 = FlowAuxiliary.Load(int3(top_left, 0)) & 0xffU;
    // Exact-zero spatial contributors do not affect reliability or uniformity.
    uint c10 = fraction.x > 0.0
        ? FlowAuxiliary.Load(int3(bottom_right.x, top_left.y, 0)) & 0xffU : c00;
    uint c01 = fraction.y > 0.0
        ? FlowAuxiliary.Load(int3(top_left.x, bottom_right.y, 0)) & 0xffU : c00;
    uint c11 = fraction.x > 0.0 && fraction.y > 0.0
        ? FlowAuxiliary.Load(int3(bottom_right, 0)) & 0xffU : c00;
    float2 top = lerp(v00, v10, fraction.x);
    float2 bottom = lerp(v01, v11, fraction.x);
    float2 result = lerp(top, bottom, fraction.y);
    NvidiaFlowSample sample;
    sample.support = saturate(1.0 - float(c00) / 255.0);
    // Retain AAF7 arithmetic and endpoint validity for every uniform cost,
    // including zero support: zeroing that vector would change alpha fallback.
    if (c00 != c10 || c00 != c01 || c00 != c11) {
        float q00 = sample.support;
        float q10 = saturate(1.0 - float(c10) / 255.0);
        float q01 = saturate(1.0 - float(c01) / 255.0);
        float q11 = saturate(1.0 - float(c11) / 255.0);
        sample.support = lerp(
            lerp(q00, q10, fraction.x), lerp(q01, q11, fraction.x), fraction.y);
        if (sample.support > 0.0) {
            result = lerp(
                lerp(v00 * q00, v10 * q10, fraction.x),
                lerp(v01 * q01, v11 * q11, fraction.x), fraction.y) / sample.support;
        }
    }
    result *= value_scale;
    result /= max(flow_input_scale(), float2(1.0e-6, 1.0e-6));
    float magnitude = length(result);
    sample.motion = magnitude > 512.0 ? result * (512.0 / magnitude) : result;
    return sample;
}

float2 flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale,
    bool use_nvidia_input_scale) {
    // The stride is zero where the backend packs eyes separately, so this
    // is the scaled coordinate on that path and the scaled coordinate plus
    // the eye offset on the stacked one.
    float2 scaled = flow_input_coordinate(pixel);
    float2 packed_coordinate = float2(
        scaled.x,
        scaled.y + float(slice * PackedEyeStride));
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect,
        slice);
    float2 bounded = clamp(
        flow_coordinate,
        float2(bounds.minimum),
        float2(bounds.maximum));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), bounds.maximum);
    float2 fraction = bounded - float2(top_left);
    float2 top = lerp(
        load_backward_flow_clamped(
            top_left,
            bounds.minimum,
            bounds.maximum),
        load_backward_flow_clamped(
            int2(bottom_right.x, top_left.y),
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 bottom = lerp(
        load_backward_flow_clamped(
            int2(top_left.x, bottom_right.y),
            bounds.minimum,
            bounds.maximum),
        load_backward_flow_clamped(
            bottom_right,
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 result = lerp(top, bottom, fraction.y) * value_scale;
    // Flow measured on a reduced input has proportionally smaller vectors.
    result /= max(flow_input_scale(), float2(1.0e-6, 1.0e-6));
    float magnitude = length(result);
    // FFX raw flow still includes head rotation. Limit only residual
    // scene motion after converting it into the target camera below.
    return use_nvidia_input_scale && magnitude > 512.0
        ? result * (512.0 / magnitude) : result;
}

float2 forward_flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale,
    // True where the previous input was warped into the current camera in
    // the pack: its pixels then lie in the target rectangle, not the source.
    bool warped_previous) {
    float2 input_scale = flow_input_scale();
    float2 packed_coordinate = flow_input_coordinate(pixel);
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        warped_previous
            ? PreviousMappings[view_index].TargetRect
            : PreviousMappings[view_index].SourceRect,
        slice);
    float2 bounded = clamp(
        flow_coordinate,
        float2(bounds.minimum),
        float2(bounds.maximum));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), bounds.maximum);
    float2 fraction = bounded - float2(top_left);
    float2 top = lerp(
        load_forward_flow_clamped(
            top_left,
            bounds.minimum,
            bounds.maximum),
        load_forward_flow_clamped(
            int2(bottom_right.x, top_left.y),
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 bottom = lerp(
        load_forward_flow_clamped(
            int2(top_left.x, bottom_right.y),
            bounds.minimum,
            bounds.maximum),
        load_forward_flow_clamped(
            bottom_right,
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 result = lerp(top, bottom, fraction.y) * value_scale /
        max(input_scale, float2(1.0e-6, 1.0e-6));
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

float2 game_motion_for_pixel(float2 pixel, uint slice, uint view_index) {
    // In the game-motion branch these otherwise-unused optical-flow constants
    // carry the DLSS output rectangle associated with this eye stream.
    float4 output_rect = float4(
        float(FlowWidth), float(FlowHeight),
        float(FlowBlockSize), float(GameMotionPadding));
    float2 uv = (pixel - output_rect.xy + 0.5) /
        max(output_rect.zw, float2(1.0, 1.0));
    float2 coordinate = GameMotionRect.xy + uv * GameMotionRect.zw - 0.5;
    float2 minimum = GameMotionRect.xy;
    float2 maximum = GameMotionRect.xy + GameMotionRect.zw - 1.0;
    float2 bounded = clamp(coordinate, minimum, maximum);
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), int2(maximum));
    float2 fraction = bounded - float2(top_left);
    // Each descriptor block owns one eye's independent mono DLSS resource.
    // Rotation produces a spatially varying field, so nearest-neighbour lookup
    // creates an avoidable endpoint error that uniform translation conceals.
    float2 top = lerp(
        GameMotionVectors.Load(int4(top_left, 0, 0)),
        GameMotionVectors.Load(int4(bottom_right.x, top_left.y, 0, 0)),
        fraction.x);
    float2 bottom = lerp(
        GameMotionVectors.Load(int4(top_left.x, bottom_right.y, 0, 0)),
        GameMotionVectors.Load(int4(bottom_right, 0, 0)),
        fraction.x);
    float2 raw = lerp(top, bottom, fraction.y);
    float2 result = raw * GameMotionScale + GameMotionJitterDelta;
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

float nvidia_cost_for_pixel(float2 pixel, uint slice, uint view_index) {
    float2 packed_coordinate = flow_input_coordinate(pixel);
    int2 coordinate = int2(round(
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5));
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect,
        slice);
    return float(FlowAuxiliary.Load(int3(clamp(
        coordinate,
        bounds.minimum,
        bounds.maximum), 0)) & 0xffU);
}

float nvidia_forward_cost_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    bool warped_previous) {
    float2 packed_coordinate = flow_input_coordinate(pixel);
    int2 coordinate = int2(round(
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5));
    FlowGridBounds bounds = flow_grid_bounds(
        warped_previous
            ? PreviousMappings[view_index].TargetRect
            : PreviousMappings[view_index].SourceRect,
        slice);
    return float(ForwardAuxiliary.Load(int3(clamp(
        coordinate,
        bounds.minimum,
        bounds.maximum), 0)) & 0xffU);
}

bool coordinate_inside_rect(float2 coordinate, float4 rect) {
    float2 minimum = rect.xy - 0.5;
    float2 maximum = rect.xy + rect.zw - 0.5;
    return coordinate.x >= minimum.x && coordinate.y >= minimum.y &&
        coordinate.x <= maximum.x && coordinate.y <= maximum.y;
}

float rgb_error(float4 a, float4 b) {
    float3 difference = abs(a.rgb - b.rgb);
    return max(difference.r, max(difference.g, difference.b));
}

// Fast can report a plausible low-cost match on a different repeated edge.
// Validate against the ORIGINAL endpoints, not just two already-warped samples:
// those can both hit the same unrelated dark/bright patch and agree perfectly.
float fast_endpoint_confidence(
    float2 pixel, float2 backward, uint slice, bool warped_previous) {
    static const float2 offsets[5] = {
        float2(0, 0), float2(-2, 0), float2(2, 0),
        float2(0, -2), float2(0, 2)
    };
    float error = 0.0;
    bool valid = true;
    [unroll] for (uint i = 0; i < 5; ++i) {
        float2 current = pixel + offsets[i];
        float2 previous = current + backward;
        if (!coordinate_inside_rect(current, PreviousMappings[ViewIndex].TargetRect)) {
            valid = false;
        }
        float4 previous_color;
        if (warped_previous) {
            // The previous endpoint is a target coordinate: read it through
            // the mapping, as the pack did when it built the engine's input.
            CameraSample sample = sample_previous_target(previous, slice, ViewIndex);
            if (sample.valid < 0.5) {
                valid = false;
            }
            previous_color = sample.color;
        } else {
            if (!coordinate_inside_rect(previous, PreviousMappings[ViewIndex].SourceRect)) {
                valid = false;
            }
            previous_color = bilinear_previous_source(previous, slice);
        }
        error = max(error, rgb_error(
            bilinear_current_source(current, slice),
            previous_color));
    }
    return valid ? 1.0 - smoothstep(8.0 / 255.0, 48.0 / 255.0, error) : 0.0;
}

// FidelityFX vectors map unwarped B pixels to unwarped A pixels. Validate
// those original endpoints, not two warped samples that can agree by chance.
// Compare in the same encoded RGB space used for optical flow, including sRGB
// views. Clamp each bilinear footprint to its view, excluding inactive padding.
float fidelityfx_endpoint_error(float2 current, float2 previous, uint slice) {
    float4 current_rect = PreviousMappings[ViewIndex].TargetRect;
    float4 previous_rect = PreviousMappings[ViewIndex].SourceRect;
    float4 b = bilinear_current_source(clamp(
        current, current_rect.xy, current_rect.xy + current_rect.zw - 1.0), slice);
    float4 a = bilinear_previous_source(clamp(
        previous, previous_rect.xy, previous_rect.xy + previous_rect.zw - 1.0), slice);
    return rgb_error(pack_flow_input_color(b), pack_flow_input_color(a));
}

float fidelityfx_endpoint_confidence(float2 pixel, float2 backward, uint slice) {
    float4 current_rect = PreviousMappings[ViewIndex].TargetRect;
    float4 previous_rect = PreviousMappings[ViewIndex].SourceRect;
    bool center_valid = coordinate_inside_rect(pixel, current_rect) &&
        coordinate_inside_rect(pixel + backward, previous_rect);
    float center_rejection = smoothstep(8.0 / 255.0, 48.0 / 255.0,
        fidelityfx_endpoint_error(pixel, pixel + backward, slice));
    // Keep a correct fine-detail center even when adjacent geometry has
    // different motion; avoid the extra neighborhood reads in that case.
    static const float2 offsets[4] = {
        float2(-2, 0), float2(2, 0), float2(0, -2), float2(0, 2)
    };
    float neighbor_rejection = 0.0;
    float valid_neighbors = 0.0;
    [branch] if (center_valid && center_rejection > 0.0) {
        [unroll] for (uint i = 0; i < 4; ++i) {
            float2 current = pixel + offsets[i];
            float2 previous = current + backward;
            if (coordinate_inside_rect(current, current_rect) &&
                coordinate_inside_rect(previous, previous_rect)) {
                neighbor_rejection += smoothstep(8.0 / 255.0, 48.0 / 255.0,
                    fidelityfx_endpoint_error(current, previous, slice));
                valid_neighbors += 1.0;
            }
        }
    }
    // Missing border neighbors do not invalidate a good center. When none
    // exist, the center alone decides; never divide by zero.
    float support = valid_neighbors > 0.0 ? neighbor_rejection / valid_neighbors : 1.0;
    return center_valid ? 1.0 - center_rejection * support : 0.0;
}

// Preserve a stationary neighbourhood after the existing XR camera mapping.
// Testing only the centre is insufficient (a moving edge can cross flat pixels).
bool fast_stationary_patch(float2 pixel, uint slice, float4 current) {
    CameraSample previous = sample_previous_target(pixel, slice, ViewIndex);
    static const float2 offsets[4] = {
        float2(-2, 0), float2(2, 0), float2(0, -2), float2(0, 2)
    };
    bool stationary = previous.valid >= 0.5 &&
        rgb_error(previous.color, current) <= 1.0 / 255.0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        CameraSample a = sample_previous_target(pixel + offsets[i], slice, ViewIndex);
        CameraSample b = sample_current_target(pixel + offsets[i], slice, ViewIndex);
        if (a.valid < 0.5 || b.valid < 0.5 || rgb_error(a.color, b.color) > 1.0 / 255.0) {
            stationary = false;
        }
    }
    return stationary;
}

struct FullscreenVertex {
    float4 position : SV_Position;
};

FullscreenVertex FullscreenTriangleVS(uint vertex_id : SV_VertexID) {
    FullscreenVertex output;
    float2 position = vertex_id == 0 ? float2(-1.0, -1.0)
                     : vertex_id == 1 ? float2(-1.0, 3.0)
                                      : float2(3.0, -1.0);
    output.position = float4(position, 0.0, 1.0);
    return output;
}

float4 synthesize_midpoint(
    FullscreenVertex input,
    float flow_value_scale,
    bool use_nvidia_cost,
    bool use_nvidia_bidirectional,
    bool validate_fast,
    bool use_game_motion_pipeline,
    // The NVIDIA pack warps the previous frame into the current camera, so
    // its flow is the residual already and every previous endpoint is a
    // target coordinate. FidelityFX keeps its own unwarped history, so its
    // flow still carries the pose term and it is subtracted here.
    bool input_pose_compensated) {
    float4 output_color = float4(0.0, 0.0, 0.0, 1.0);
    uint2 integer_pixel = uint2(input.position.xy);
    // Shared synthetic atlases retain the array history's eye-local domain.
    // Default/native layouts keep their original pixel calculation.
    if ((SynthesisFlags & 0x10u) != 0u)
        integer_pixel.x -= Slice * Width;
    bool in_bounds = integer_pixel.x < Width && integer_pixel.y < Height &&
        Slice < ArraySize;
    if (in_bounds) {
        float2 pixel = float2(integer_pixel);
        CameraSample current_fallback = sample_current_target(
            pixel,
            Slice,
            ViewIndex);
        output_color = saturate(current_fallback.color);
        MappedCoordinate previous_coverage = map_target_to_source(
            pixel,
            PreviousMappings[ViewIndex]);
        bool scene_changed = repeated_capture_flag() == 0 && !use_game_motion_pipeline &&
            !use_nvidia_cost &&
            (FlowAuxiliary.Load(int3(1, 0, 0)) & 0x0fU) != 0;
        if (previous_coverage.valid >= 0.5 && !scene_changed) {
            bool preserve_stationary = validate_fast && repeated_capture_flag() == 0 &&
                current_fallback.valid >= 0.5 &&
                fast_stationary_patch(pixel, Slice, current_fallback.color);
            if (preserve_stationary) {
                output_color = saturate(lerp(
                    sample_previous_target(pixel, Slice, ViewIndex).color,
                    current_fallback.color,
                    synthesis_fraction()));
            } else {
                float2 raw_backward = float2(0.0, 0.0);
                bool use_nvidia_vector_confidence = use_nvidia_cost && !validate_fast && !use_nvidia_bidirectional;
                float backward_flow_support = 1.0;
                CameraSample previous_sample = (CameraSample)0;
                CameraSample current_sample = (CameraSample)0;
                bool endpoints_valid = true;
                if (use_game_motion_pipeline) {
                    // A B-to-A vector is attached to its B endpoint. Sampling
                    // it once at the desired midpoint is exact only for a
                    // constant translation field. Yaw is spatially varying:
                    // solve cB = midpoint - 0.5 * (cA(cB) - cB), while mapping
                    // cA through the previous OpenXR camera exactly.
                    float2 current_endpoint = pixel;
                    MappedCoordinate previous_endpoint;
                    [unroll] for (uint iteration = 0; iteration < 3; ++iteration) {
                        raw_backward = repeated_capture_flag() != 0
                            ? float2(0.0, 0.0)
                            : game_motion_for_pixel(
                                current_endpoint, Slice, ViewIndex);
                        previous_endpoint = map_source_to_target(
                            current_endpoint + raw_backward,
                            PreviousMappings[ViewIndex]);
                        if (previous_endpoint.valid < 0.5) {
                            endpoints_valid = false;
                            break;
                        }
                        float2 target_displacement =
                            previous_endpoint.coordinate - current_endpoint;
                        current_endpoint =
                            pixel - (1.0 - synthesis_fraction()) * target_displacement;
                    }
                    if (endpoints_valid) {
                        raw_backward = repeated_capture_flag() != 0
                            ? float2(0.0, 0.0)
                            : game_motion_for_pixel(
                                current_endpoint, Slice, ViewIndex);
                        previous_endpoint = map_source_to_target(
                            current_endpoint + raw_backward,
                            PreviousMappings[ViewIndex]);
                        endpoints_valid = previous_endpoint.valid >= 0.5;
                    }
                    if (endpoints_valid) {
                        previous_sample = sample_previous_target(
                            previous_endpoint.coordinate, Slice, ViewIndex);
                        current_sample = sample_current_target(
                            current_endpoint, Slice, ViewIndex);
                    }
                } else {
                    if (use_nvidia_vector_confidence && repeated_capture_flag() == 0) {
                        NvidiaFlowSample reconstructed = nvidia_confidence_flow_for_pixel(
                            pixel, Slice, ViewIndex, flow_value_scale);
                        raw_backward = reconstructed.motion;
                        backward_flow_support = reconstructed.support;
                    } else {
                    raw_backward = repeated_capture_flag() != 0
                        ? float2(0.0, 0.0)
                        : flow_for_pixel(
                            pixel,
                            Slice,
                            ViewIndex,
                            flow_value_scale,
                            use_nvidia_cost);
                    }
                    float2 residual_backward = input_pose_compensated
                        ? raw_backward
                        : raw_backward - (previous_coverage.coordinate - pixel);
                    if (!input_pose_compensated && repeated_capture_flag() == 0) {
                        // FFX flow ends in A's source camera. Convert the
                        // endpoint to B's camera before subtracting pixels.
                        MappedCoordinate endpoint = map_source_to_target(
                            pixel + raw_backward, PreviousMappings[ViewIndex]);
                        endpoints_valid = endpoint.valid >= 0.5;
                        residual_backward = endpoint.coordinate - pixel;
                        float magnitude = length(residual_backward);
                        residual_backward = magnitude > 512.0
                            ? residual_backward * (512.0 / magnitude) : residual_backward;
                    }
                    previous_sample = sample_previous_target(
                        pixel + residual_backward * synthesis_fraction(), Slice, ViewIndex);
                    current_sample = sample_current_target(
                        pixel - residual_backward * (1.0 - synthesis_fraction()),
                        Slice,
                        ViewIndex);
                }
                if (endpoints_valid) {
                    if (previous_sample.valid >= 0.5 && current_sample.valid >= 0.5) {
                    float4 flow_midpoint = lerp(
                        previous_sample.color, current_sample.color, synthesis_fraction());
                    float disagreement = max(
                        abs(previous_sample.color.r - current_sample.color.r),
                        max(
                            abs(previous_sample.color.g - current_sample.color.g),
                            abs(previous_sample.color.b - current_sample.color.b)));
                    float confidence = saturate(1.0 - disagreement * 6.0);
                    CameraSample previous_fallback = sample_previous_target(pixel, Slice, ViewIndex);
                    float4 stable_midpoint = lerp(
                        previous_fallback.color,
                        current_fallback.color,
                        synthesis_fraction());
                    if (use_nvidia_cost) {
                        float consistency_confidence = 1.0;
                        float cost_confidence = 1.0;
                        if (repeated_capture_flag() == 0) {
                            if (validate_fast) {
                                confidence *= fast_endpoint_confidence(
                                    pixel, raw_backward, Slice, input_pose_compensated);
                            }
                            if (use_nvidia_bidirectional) {
                                float2 previous_coordinate = pixel + raw_backward;
                                if (coordinate_inside_rect(
                                        previous_coordinate,
                                        input_pose_compensated
                                            ? PreviousMappings[ViewIndex].TargetRect
                                            : PreviousMappings[ViewIndex].SourceRect)) {
                                    float2 raw_forward = forward_flow_for_pixel(
                                        previous_coordinate,
                                        Slice,
                                        ViewIndex,
                                        flow_value_scale,
                                        input_pose_compensated);
                                    float cycle_error = length(
                                        raw_backward + raw_forward);
                                    consistency_confidence =
                                        1.0 - smoothstep(2.0, 6.0, cycle_error);
                                    float maximum_cost = max(
                                        nvidia_cost_for_pixel(
                                            pixel,
                                            Slice,
                                            ViewIndex),
                                        nvidia_forward_cost_for_pixel(
                                            previous_coordinate,
                                            Slice,
                                            ViewIndex,
                                            input_pose_compensated));
                                    cost_confidence = saturate(
                                        1.0 - maximum_cost / 255.0);
                                } else {
                                    consistency_confidence = 0.0;
                                    cost_confidence = 0.0;
                                }
                            } else {
                                cost_confidence = use_nvidia_vector_confidence ? backward_flow_support : saturate(
                                    1.0 - nvidia_cost_for_pixel(
                                        pixel,
                                        Slice,
                                        ViewIndex) / 255.0);
                            }
                        }
                        confidence *= consistency_confidence * cost_confidence;
                        output_color = saturate(lerp(
                            stable_midpoint,
                            flow_midpoint,
                            confidence));
                        if (!validate_fast && !use_nvidia_bidirectional && repeated_capture_flag() == 0) {
                            float camera_only_error = rgb_error(
                                pack_flow_input_color(previous_fallback.color), pack_flow_input_color(current_fallback.color));
                            float warped_error = rgb_error(
                                pack_flow_input_color(previous_sample.color), pack_flow_input_color(current_sample.color));
                            float rgb_confidence = saturate(1.0 + (camera_only_error - warped_error) * 8.0);
                            rgb_confidence *= cost_confidence;
                            output_color.rgb = saturate(lerp(
                                current_fallback.color.rgb, current_sample.color.rgb, rgb_confidence));
                        }
                    } else {
                        output_color = saturate(lerp(
                            stable_midpoint,
                            flow_midpoint,
                            confidence));
                        if (!use_game_motion_pipeline && repeated_capture_flag() == 0) {
                            // A rejected original endpoint uses current B, rather than
                            // crossfading unrelated history into disoccluded pixels.
                            output_color = saturate(lerp(
                                current_fallback.color, output_color,
                                fidelityfx_endpoint_confidence(pixel, raw_backward, Slice)));
                        }
                    }
                    }
                }
            }
        }
    }
    return output_color;
}

float4 SynthesizeMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0, false, false, false, false, false);
}

float4 SynthesizeGameMotionMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0, false, false, false, true, false);
}

// The NVIDIA variants read flow in signed S10.5 fixed point, and from a
// previous input the pack warped into the current camera (PackNvidiaFlowInput).
float4 SynthesizeNvidiaMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0 / 32.0, true, false, false, false, true);
}

float4 SynthesizeNvidiaBidirectionalMidpointPS(
    FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0 / 32.0, true, true, false, false, true);
}

float4 SynthesizeNvidiaFastMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0 / 32.0, true, false, true, false, true);
}

float4 SynthesizeNvidiaFastBidirectionalMidpointPS(
    FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0 / 32.0, true, true, true, false, true);
}

// Only the private medium-NVIDIA, nonrepeated atlas path selects this PS.
// Keep the single-output entry points above unchanged for differential replay.
struct NvidiaSharedSynthesisCommon {
    float2 pixel;
    CameraSample current_fallback;
    CameraSample previous_fallback;
    float2 raw_backward;
    float backward_flow_support;
    float camera_only_error;
    bool valid;
};

NvidiaSharedSynthesisCommon prepare_nvidia_shared_synthesis(FullscreenVertex input) {
    NvidiaSharedSynthesisCommon common = (NvidiaSharedSynthesisCommon)0;
    common.current_fallback.color = float4(0.0, 0.0, 0.0, 1.0);
    uint2 integer_pixel = uint2(input.position.xy);
    if ((SynthesisFlags & 0x10u) != 0u)
        integer_pixel.x -= Slice * Width;
    bool in_bounds = integer_pixel.x < Width && integer_pixel.y < Height && Slice < ArraySize;
    if (in_bounds) {
        common.pixel = float2(integer_pixel);
        common.current_fallback = sample_current_target(common.pixel, Slice, ViewIndex);
        MappedCoordinate previous_coverage = map_target_to_source(
            common.pixel, PreviousMappings[ViewIndex]);
        if (previous_coverage.valid >= 0.5) {
            NvidiaFlowSample reconstructed = nvidia_confidence_flow_for_pixel(
                common.pixel, Slice, ViewIndex, 1.0 / 32.0);
            common.raw_backward = reconstructed.motion;
            common.backward_flow_support = reconstructed.support;
            common.previous_fallback = sample_previous_target(common.pixel, Slice, ViewIndex);
            common.camera_only_error = rgb_error(
                pack_flow_input_color(common.previous_fallback.color),
                pack_flow_input_color(common.current_fallback.color));
            common.valid = true;
        }
    }
    return common;
}

float4 synthesize_nvidia_shared_phase(NvidiaSharedSynthesisCommon common, float fraction) {
    float4 output_color = saturate(common.current_fallback.color);
    if (common.valid) {
        CameraSample previous_sample = sample_previous_target(
            common.pixel + common.raw_backward * fraction, Slice, ViewIndex);
        CameraSample current_sample = sample_current_target(
            common.pixel - common.raw_backward * (1.0 - fraction), Slice, ViewIndex);
        if (previous_sample.valid >= 0.5 && current_sample.valid >= 0.5) {
            float4 flow_midpoint = lerp(previous_sample.color, current_sample.color, fraction);
            float disagreement = max(abs(previous_sample.color.r - current_sample.color.r),
                max(abs(previous_sample.color.g - current_sample.color.g),
                    abs(previous_sample.color.b - current_sample.color.b)));
            float confidence = saturate(1.0 - disagreement * 6.0);
            float4 stable_midpoint = lerp(
                common.previous_fallback.color, common.current_fallback.color, fraction);
            float consistency_confidence = 1.0;
            float cost_confidence = common.backward_flow_support;
            confidence *= consistency_confidence * cost_confidence;
            // Alpha retains the original stable/flow policy for each phase.
            output_color = saturate(lerp(stable_midpoint, flow_midpoint, confidence));
            float warped_error = rgb_error(
                pack_flow_input_color(previous_sample.color), pack_flow_input_color(current_sample.color));
            float rgb_confidence = saturate(1.0 + (common.camera_only_error - warped_error) * 8.0);
            rgb_confidence *= cost_confidence;
            output_color.rgb = saturate(lerp(
                common.current_fallback.color.rgb, current_sample.color.rgb, rgb_confidence));
        }
    }
    return output_color;
}

struct NvidiaShared3xTargets {
    float4 first : SV_Target0;
    float4 second : SV_Target1;
};

NvidiaShared3xTargets SynthesizeNvidiaShared3xPS(FullscreenVertex input) {
    NvidiaSharedSynthesisCommon common = prepare_nvidia_shared_synthesis(input);
    uint second_packed = (GameMotionPadding >> 8) & 0xffffu;
    float second_fraction = second_packed == 0u ? 0.5 : float(second_packed) / 65535.0;
    NvidiaShared3xTargets output;
    output.first = synthesize_nvidia_shared_phase(common, synthesis_fraction());
    output.second = synthesize_nvidia_shared_phase(common, second_fraction);
    return output;
}
