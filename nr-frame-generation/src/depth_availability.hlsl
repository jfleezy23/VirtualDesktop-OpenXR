// Exact finite-nonnegative IEEE classifier. No float arithmetic/comparison:
// positive subnormals and either zero sign must survive shader FTZ modes.
Texture2D<float> CanonicalDepth : register(t0);
RWByteAddressBuffer TileAvailability : register(u0);
RWByteAddressBuffer FrameAvailability : register(u1);
cbuffer AvailabilityParameters : register(b0) {
    uint TilesX, TilesY, HistoryIndex, Reserved;
    uint4 PublishedRects[2];
};
groupshared uint2 Available[256];

[numthreads(16, 16, 1)] void Tiles(uint3 group : SV_GroupID,
                                   uint3 lane : SV_GroupThreadID,
                                   uint index : SV_GroupIndex) {
    uint4 rect = group.z == 0u ? PublishedRects[0] : PublishedRects[1];
    uint2 local = group.xy * 16u + lane.xy;
    uint usable = 0u;
    if (all(local < rect.zw)) {
        uint bits = asuint(CanonicalDepth.Load(int3(rect.xy + local, 0)));
        uint magnitude = bits & 0x7fffffffu;
        usable = (bits & 0x7f800000u) != 0x7f800000u && ((bits & 0x80000000u) == 0u || magnitude == 0u) ? 1u : 0u;
    }
    Available[index] = uint2(usable, 0u);
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (index < stride)
            Available[index] |= Available[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0u) {
        uint tile = group.z * TilesX * TilesY + group.y * TilesX + group.x;
        TileAvailability.Store(tile * 4u, Available[0].x);
    }
}

    [numthreads(256, 1, 1)] void Reduce(uint index : SV_GroupIndex) {
    uint count = TilesX * TilesY;
    uint2 available = uint2(0u, 0u);
    for (uint tile = index; tile < count; tile += 256u) {
        available.x |= TileAvailability.Load(tile * 4u);
        available.y |= TileAvailability.Load((count + tile) * 4u);
    }
    Available[index] = available;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (index < stride)
            Available[index] |= Available[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0u)
        FrameAvailability.Store(HistoryIndex * 4u, Available[0].x != 0u && Available[0].y != 0u ? 1u : 0u);
}
