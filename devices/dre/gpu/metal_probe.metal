// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

kernel void dre_transform(device const uint *input [[buffer(0)]],
    device uint *output [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    if (i >= 257) return;
    uint shift = i & 31;
    uint value = input[i];
    uint rotated = shift ? (value << shift) | (value >> (32 - shift)) : value;
    output[i] = (rotated ^ 0xa619d22a) + i * 0x10203;
}

struct VertexOut { float4 position [[position]]; };
vertex VertexOut dre_vertex(uint i [[vertex_id]])
{
    const float2 positions[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    VertexOut output;
    output.position = float4(positions[i], 0, 1);
    return output;
}
fragment float4 dre_fragment(VertexOut input [[stage_in]])
{
    uint2 p = uint2(input.position.xy);
    uint blue = (p.x * 3 + p.y * 5) & 255;
    uint green = p.x ^ p.y;
    return float4(165.0f / 255.0f, float(green) / 255.0f, float(blue) / 255.0f, 1);
}
