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
