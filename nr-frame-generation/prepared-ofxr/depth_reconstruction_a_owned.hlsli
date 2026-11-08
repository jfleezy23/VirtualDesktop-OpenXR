// Bounded inverse gather of A's own geometry. This discovers at most two
// roots; it is not a forward z-buffer or a hidden-surface reconstruction.
struct AOwnedCandidate {
    MappedCoordinate mapped;
    float currentInverseZ;
#ifdef XRFG_RECONSTRUCTION_REUSE
    float finalAInverseZ;
    bool fixedPoint;
#endif
};

AOwnedCandidate reconstruction_a_candidate(float2 target, MappedCoordinate seed,
                                           uint slice, uint view, ReconstructionDepth firstDepth, bool hasFirstDepth) {
    AOwnedCandidate result = (AOwnedCandidate)0;
    CameraMapping mapping = PreviousMappings[view];
    float4 rotation = reconstruction_rotation(view);
    float3 translation = reconstruction_translation(view);
    float4 inverseRotation = float4(-rotation.xyz,rotation.w);
    float3 rayA = rotate_by_quaternion(inverseRotation,
        reconstruction_ray(target,mapping.TargetRect,mapping.TargetTangents));
    float3 translationA = rotate_by_quaternion(inverseRotation,translation);
    MappedCoordinate candidate = seed;
    bool active = seed.valid >= 0.5 && rayA.z != 0.0 &&
        !any((asuint(rayA) & 0x7f800000u) == 0x7f800000u) &&
        !any((asuint(translationA) & 0x7f800000u) == 0x7f800000u);
#ifdef XRFG_RECONSTRUCTION_REUSE
    bool fixedPoint = false;
    ReconstructionDepth fixedDepth = (ReconstructionDepth)0;
#endif
    [unroll] for (uint iteration=0u; iteration<3u; ++iteration) {
        if (active
#ifdef XRFG_RECONSTRUCTION_REUSE
            && !fixedPoint
#endif
            ) {
            ReconstructionDepth da = (ReconstructionDepth)0;
#ifdef XRFG_RECONSTRUCTION_REUSE
            da = firstDepth;
            if (iteration != 0u || !hasFirstDepth)
#endif
                da = reconstruction_depth(candidate.coordinate,slice,true,mapping.SourceRect);
            active = da.valid;
            if (active) {
                float q = (da.inverseZ * translationA.z - 1.0) / rayA.z;
                active = q > 0.0 && (asuint(q) & 0x7f800000u) != 0x7f800000u;
                if (active) {
#ifdef XRFG_RECONSTRUCTION_REUSE
                    float2 inputCoordinate = candidate.coordinate;
#endif
                    // The solved plane constraint makes z exactly -1. Do not
                    // reintroduce cancellation by subtracting large z terms.
                    candidate = reconstruction_project_ray(float3(q*rayA.xy-da.inverseZ*translationA.xy,-1.0),
                        mapping.SourceRect,mapping.SourceTangents);
                    active = candidate.valid >= 0.5;
#ifdef XRFG_RECONSTRUCTION_REUSE
                    fixedPoint = active && all(asuint(candidate.coordinate) == asuint(inputCoordinate));
                    if (fixedPoint) fixedDepth = da;
#endif
                }
            }
        }
    }
    if (active) {
        // A proven bit-identical fixed point already sampled this exact footprint.
        // Every nonfixed root still resamples: stale depth can accept oscillation.
        ReconstructionDepth finalA = (ReconstructionDepth)0;
#ifdef XRFG_RECONSTRUCTION_REUSE
        if (fixedPoint) finalA = fixedDepth;
        else
#endif
            finalA = reconstruction_depth(candidate.coordinate,slice,true,mapping.SourceRect);
        if (finalA.valid) {
            float3 actualRayA = reconstruction_ray(candidate.coordinate,mapping.SourceRect,mapping.SourceTangents);
            float3 actualB = rotate_by_quaternion(rotation,actualRayA) + finalA.inverseZ*translation;
            float q = -actualB.z;
            if (q > 0.0 && (asuint(q) & 0x7f800000u) != 0x7f800000u) {
                MappedCoordinate returned = reconstruction_project_ray(actualB,mapping.TargetRect,mapping.TargetTangents);
                float rhoB = finalA.inverseZ / q;
                if (returned.valid >= 0.5 && distance(returned.coordinate,target) <= 0.5 &&
                    rhoB >= 0.0 && (finalA.inverseZ == 0.0 || rhoB > 0.0) &&
                    (asuint(rhoB) & 0x7f800000u) != 0x7f800000u) {
                    if (reconstruction_opaque(candidate.coordinate,slice,true,mapping.SourceRect)) {
                        result.mapped = candidate;
                        result.currentInverseZ = rhoB;
#ifdef XRFG_RECONSTRUCTION_REUSE
                        result.finalAInverseZ = finalA.inverseZ;
                        result.fixedPoint = fixedPoint;
#endif
                    } else {
                        // Depth cannot locate a transparent color footprint.
                        // Keep its legacy fallback, below every opaque root.
                        result.mapped = map_target_to_source(target,mapping);
                        result.currentInverseZ = -1.0;
                    }
                }
            }
        }
    }
    return result;
}

MappedCoordinate reconstruction_a_owned_history(float2 pixel, uint slice, uint view) {
    CameraMapping mapping = PreviousMappings[view];
    AOwnedCandidate best = (AOwnedCandidate)0;
    ReconstructionProjection direction = reconstruction_project(pixel,0.0,view);
    if (direction.valid) {
        MappedCoordinate seed = (MappedCoordinate)0;
        seed.coordinate = direction.coordinate;
        seed.valid = 1.0;
        best = reconstruction_a_candidate(pixel,seed,slice,view,(ReconstructionDepth)0,false);
    }
    // Current B supplies an optional second seed, not a cross-time veto.
    ReconstructionDepth db = reconstruction_depth(pixel,slice,false,mapping.TargetRect);
    if (db.valid) {
        ReconstructionProjection projected = reconstruction_project(pixel,db.inverseZ,view);
        if (projected.valid) {
            MappedCoordinate seed = (MappedCoordinate)0;
            seed.coordinate = projected.coordinate;
            seed.valid = 1.0;
            bool skip = false;
            bool hasFirstDepth = false;
            ReconstructionDepth firstDepth = (ReconstructionDepth)0;
#ifdef XRFG_RECONSTRUCTION_REUSE
            bool fixedOpaqueBest = best.mapped.valid >= 0.5 && best.currentInverseZ >= 0.0 && best.fixedPoint;
            skip = fixedOpaqueBest && all(asuint(seed.coordinate) == asuint(best.mapped.coordinate));
            if (!skip && fixedOpaqueBest) {
                firstDepth = reconstruction_depth(seed.coordinate,slice,true,mapping.SourceRect);
                hasFirstDepth = true;
                // Mapping depends only on this depth and the fixed camera.
                // The same depth reaches the same proven fixed point.
                skip = firstDepth.valid && asuint(firstDepth.inverseZ) == asuint(best.finalAInverseZ);
            }
#endif
            if (!skip) {
                AOwnedCandidate candidate = reconstruction_a_candidate(pixel,seed,slice,view,firstDepth,hasFirstDepth);
                if (candidate.mapped.valid >= 0.5 &&
                    (best.mapped.valid < 0.5 || candidate.currentInverseZ > best.currentInverseZ)) best = candidate;
            }
        }
    }
    return best.mapped;
}
