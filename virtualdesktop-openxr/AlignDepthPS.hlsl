// MIT License
// Copyright(c) 2026
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

cbuffer config : register(b0)
{
    int2 sourceOrigin;
    int2 sourceExtent;
    int2 destinationOrigin;
    int2 destinationExtent;
};

Texture2D<float> sourceDepth : register(t0);

float main(float4 position : SV_Position) : SV_Depth
{
    // SV_Position includes the destination pixel-center offset. Point loads keep
    // discontinuities intact and cannot sample outside the submitted depth rect.
    float2 relative = position.xy - float2(destinationOrigin);
    int2 source = sourceOrigin + int2(relative * float2(sourceExtent) / float2(destinationExtent));
    source = clamp(source, sourceOrigin, sourceOrigin + sourceExtent - 1);
    return sourceDepth.Load(int3(source, 0));
}
