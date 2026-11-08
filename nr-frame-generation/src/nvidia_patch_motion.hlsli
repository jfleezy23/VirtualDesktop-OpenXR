#ifndef NVIDIA_PATCH_MOTION_HLSLI
#define NVIDIA_PATCH_MOTION_HLSLI
#if defined(XRFG_PATCH_MOTION) && XRFG_PATCH_MOTION
// Frozen patch-25-rgb-v1. Scores are linear RGB, independent of flow sRGB packing.
Texture2D<float4> NvidiaPatchProposals : register(t7);
RWTexture2D<float4> NvidiaPatchOutput : register(u3);

float2 patch_project(float2 motion) {
    return motion * min(1.0, 512.0 / max(length(motion), 512.0));
}

bool patch_inside(float2 pixel, float4 rect) {
    return all(pixel >= rect.xy) && all(pixel <= rect.xy + rect.zw - 1.0) &&
        all(pixel >= 0.0) && all(pixel <= float2(Width - 1u, Height - 1u));
}

bool patch_rgb(float2 target, uint slice, uint view, bool previous, out float3 rgb) {
    rgb = 0.0;
    CameraMapping mapping = PreviousMappings[view];
    bool valid = patch_inside(target, mapping.TargetRect);
    float2 source = target;
    if (previous && valid) {
        MappedCoordinate mapped = map_target_to_source(target, mapping);
        valid = mapped.valid > 0.5 && patch_inside(mapped.coordinate, mapping.SourceRect);
        source = mapped.coordinate;
    }
    // A single return avoids FXC's out-parameter analysis across early returns.
    if (valid) {
        int2 low = int2(floor(source));
        int2 high = min(low + 1, int2(Width - 1u, Height - 1u));
        float2 fraction = source - float2(low);
        float3 a = 0.0, b = 0.0, c = 0.0, d = 0.0;
        if (previous) {
            a = PreviousFrame.Load(int4(low, slice, 0)).rgb;
            b = PreviousFrame.Load(int4(high.x, low.y, slice, 0)).rgb;
            c = PreviousFrame.Load(int4(low.x, high.y, slice, 0)).rgb;
            d = PreviousFrame.Load(int4(high, slice, 0)).rgb;
        } else {
            a = CurrentFrame.Load(int4(low, slice, 0)).rgb;
            b = CurrentFrame.Load(int4(high.x, low.y, slice, 0)).rgb;
            c = CurrentFrame.Load(int4(low.x, high.y, slice, 0)).rgb;
            d = CurrentFrame.Load(int4(high, slice, 0)).rgb;
        }
        rgb = lerp(lerp(a, b, fraction.x), lerp(c, d, fraction.x), fraction.y);
    }
    return valid;
}

float patch_error(float3 a, float3 b) {
    float3 difference = abs(a - b);
    return max(difference.x, max(difference.y, difference.z));
}

[numthreads(8, 8, 1)]
void BuildNvidiaPatchProposalsCS(uint3 thread_id : SV_DispatchThreadID) {
    uint2 cell = thread_id.xy;
    if (cell.x >= FlowWidth || cell.y >= FlowHeight) return;
    float2 original = patch_project(float2(BackwardFlow.Load(int3(cell, 0))) / 16.0);
    NvidiaPatchOutput[cell] = float4(original, 0.0, 0.0);
    if ((SynthesisFlags & 0x20u) == 0u || Slice >= ArraySize || FlowBlockSize != 4u || any(flow_input_scale() != 0.5) ||
        Width % 8u != 0u || Height % 8u != 0u || repeated_capture_flag() != 0u ||
        FlowAuxiliary.Load(int3(cell, 0)) == 255u || all(original == 0.0)) return;
    float2 center = float2(cell) * 8.0 + 3.5;
    uint view = pack_view_for_pixel(center, Slice);
    if (view >= 2u) return;

    float2 candidates[10];
    candidates[0] = original;
    candidates[1] = 0.0;
    uint count = 2u;
    float radius = min(0.5, 0.5 * length(original));
    // CPU direction order is x-major, then y-major. Stable ties keep original.
    [unroll] for (int x = -1; x <= 1; ++x) {
        [unroll] for (int y = -1; y <= 1; ++y) {
            if (x == 0 && y == 0) continue;
            float2 direction = float2(x, y);
            float2 candidate = patch_project(original + radius * direction / length(direction));
            bool duplicate = false;
            [loop] for (uint j = 0u; j < count; ++j) duplicate = duplicate || all(candidate == candidates[j]);
            if (!duplicate) candidates[count++] = candidate;
        }
    }
    float3 current[25];
    float3 minimum = 1.0, maximum = 0.0;
    [loop] for (uint current_index = 0u; current_index < 25u; ++current_index) {
        float2 coordinate = center + float2(int(current_index % 5u) * 2 - 4, int(current_index / 5u) * 2 - 4);
        float3 current_rgb = 0.0;
        if (!patch_rgb(coordinate, Slice, view, false, current_rgb)) return;
        current[current_index] = current_rgb;
        minimum = min(minimum, current_rgb);
        maximum = max(maximum, current_rgb);
    }
    float3 range = maximum - minimum;
    if (max(range.x, max(range.y, range.z)) < 4.0 / 255.0) return;
    float xx = 0.0, xy = 0.0, yy = 0.0;
    [unroll] for (uint row = 1u; row <= 3u; ++row) {
        [unroll] for (uint col = 1u; col <= 3u; ++col) {
            uint index = row * 5u + col;
            float3 gx = (current[index + 1u] - current[index - 1u]) / 4.0;
            float3 gy = (current[index + 5u] - current[index - 5u]) / 4.0;
            xx += dot(gx, gx); xy += dot(gx, gy); yy += dot(gy, gy);
        }
    }
    xx /= 27.0; xy /= 27.0; yy /= 27.0;
    float discriminant = sqrt(max(0.0, (xx - yy) * (xx - yy) + 4.0 * xy * xy));
    float low = max(0.0, (xx + yy - discriminant) * 0.5);
    float high = max(0.0, (xx + yy + discriminant) * 0.5);
    if (low < (1.0 / 1024.0) * (1.0 / 1024.0) || high <= 0.0 || low / high < 0.05) return;

    const float2 aliases[4] = {float2(8, 0), float2(0, 8), float2(4, 4), float2(4, -4)};
    [unroll] for (uint probe = 0u; probe < 4u; ++probe) {
        float score = 0.0;
        [loop] for (uint alias_index = 0u; alias_index < 25u; ++alias_index) {
            float2 coordinate = center + float2(int(alias_index % 5u) * 2 - 4, int(alias_index / 5u) * 2 - 4);
            float3 rgb_value = 0.0;
            if (!patch_rgb(coordinate + aliases[probe], Slice, view, false, rgb_value)) return;
            score += patch_error(rgb_value, current[alias_index]);
        }
        if (score / 25.0 <= 1.0 / 1024.0) return;
    }
    float scores[10];
    [loop] for (uint candidate = 0u; candidate < count; ++candidate) {
        float score = 0.0;
        [loop] for (uint sample_index = 0u; sample_index < 25u; ++sample_index) {
            float2 coordinate = center + float2(int(sample_index % 5u) * 2 - 4, int(sample_index / 5u) * 2 - 4);
            float3 rgb_value = 0.0;
            if (!patch_rgb(coordinate + candidates[candidate], Slice, view, true, rgb_value)) return;
            score += patch_error(rgb_value, current[sample_index]);
        }
        scores[candidate] = score / 25.0;
    }
    uint winner = 0u;
    [loop] for (uint best_index = 1u; best_index < count; ++best_index)
        if (scores[best_index] < scores[winner]) winner = best_index;
    if (winner == 0u) return;
    float best = scores[winner], runner = 1.0e30;
    [loop] for (uint runner_index = 0u; runner_index < count; ++runner_index)
        if (runner_index != winner) runner = min(runner, scores[runner_index]);
    if (scores[0] - best < 1.0 / 1024.0 || best > 0.75 * scores[0] ||
        runner - best < 1.0 / 1024.0 || best > 0.75 * runner) return;
    NvidiaPatchOutput[cell] = float4(candidates[winner], all(candidates[winner] == 0.0) ? 1.0 : 2.0, 0.0);
}

float3 patch_current_rgb(float2 pixel, float2 original, float fraction, uint slice,
    uint view_index, float3 original_current_rgb) {
    float3 result = original_current_rgb;
    bool supported = (SynthesisFlags & 0x20u) != 0u && slice < ArraySize && view_index < 2u &&
        repeated_capture_flag() == 0u && FlowBlockSize == 4u && all(flow_input_scale() == 0.5) && any(original != 0.0);
    if (supported) {
        int2 owner = int2(floor(pixel / 8.0));
        if (all(owner >= 0) && owner.x < int(FlowWidth) && owner.y < int(FlowHeight)) {
            float4 proposal = NvidiaPatchProposals.Load(int3(owner, 0));
            if (proposal.z == 1.0 || proposal.z == 2.0) {
                float2 cell_original = patch_project(float2(BackwardFlow.Load(int3(owner, 0))) / 16.0);
                if (length(original - cell_original) <= 0.5) {
                    float2 delta = proposal.xy - cell_original;
                    float delta_length = length(delta);
                    float2 candidate = proposal.z == 1.0 ? float2(0.0, 0.0) :
                        patch_project(original + delta * min(1.0, min(0.5, length(original) * 0.5) / max(delta_length, 1.0e-20)));
                    float2 endpoint = pixel - candidate * (1.0 - fraction);
                    if (patch_inside(endpoint, PreviousMappings[view_index].TargetRect)) {
                        // Transfer uses the baseline synthesis sampler, blending complete B RGB samples.
                        CameraSample full = sample_current_target(endpoint, slice, view_index);
                        if (full.valid > 0.5) {
                            float2 local = pixel - float2(owner) * 8.0;
                            float2 edge = min(local + 0.5, 7.5 - local);
                            float feather = smoothstep(0.0, 2.0, min(edge.x, edge.y));
                            result = lerp(original_current_rgb, full.color.rgb, feather);
                        }
                    }
                }
            }
        }
    }
    return result;
}

#endif
#endif
