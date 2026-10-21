// Preserve encoded color and alpha while changing only the BGRA/RGBA storage layout.
// The source UNORM view already exposes logical RGBA; do not swizzle or apply gamma.
Texture2D<float4> sourceTexture : register(t0);
RWTexture2D<unorm float4> destinationTexture : register(u0);

[numthreads(16, 16, 1)]
void main(uint2 pos : SV_DispatchThreadID)
{
    uint width, height;
    destinationTexture.GetDimensions(width, height);
    if (pos.x < width && pos.y < height)
        destinationTexture[pos] = sourceTexture.Load(int3(pos, 0));
}
