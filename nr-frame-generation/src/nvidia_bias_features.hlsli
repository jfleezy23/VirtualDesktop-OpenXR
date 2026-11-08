#ifndef NVIDIA_BIAS_FEATURES_HLSLI
#define NVIDIA_BIAS_FEATURES_HLSLI
// Test-only for now. Include after the baseline synthesis sampling helpers.
struct NvidiaBiasFeatures {
    float2 features;
    float warped_error;
    float camera_error;
    float eligible;
};

NvidiaBiasFeatures nvidia_bias_features(
    float2 pixel, float2 motion, float fraction, uint slice, uint view_index,
    CameraSample previous_sample, CameraSample current_sample,
    CameraSample previous_fallback, CameraSample current_fallback) {
    float2 current_endpoint = pixel - motion * (1.0 - fraction);
    float magnitude = length(motion);
    float2 perpendicular = magnitude > 0.0
        ? float2(-motion.y, motion.x) / magnitude : float2(1.0, 0.0);
    CameraSample temporal_previous = sample_previous_target(current_endpoint, slice, view_index);
    CameraSample contrast_current = sample_current_target(
        current_endpoint + perpendicular / flow_input_scale(), slice, view_index);
    float4 packed_current = pack_flow_input_color(current_sample.color);
    NvidiaBiasFeatures output;
    output.features.x = rgb_error(pack_flow_input_color(temporal_previous.color), packed_current);
    output.features.y = rgb_error(pack_flow_input_color(contrast_current.color), packed_current);
    output.warped_error = rgb_error(pack_flow_input_color(previous_sample.color), packed_current);
    output.camera_error = rgb_error(pack_flow_input_color(previous_fallback.color),
        pack_flow_input_color(current_fallback.color));
    // Strict veto, not a probability or proof of physical stationarity.
    output.eligible = previous_sample.valid > 0.5 && current_sample.valid > 0.5 &&
        temporal_previous.valid > 0.5 && contrast_current.valid > 0.5 &&
        previous_fallback.valid > 0.5 && current_fallback.valid > 0.5 &&
        output.warped_error > max(output.features.x, output.camera_error) + 1.0e-6 ? 1.0 : 0.0;
    return output;
}
#endif
