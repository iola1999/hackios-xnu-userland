#include <metal_stdlib>
using namespace metal;

struct FetchTargets {
    half4 first [[color(0)]];
    half4 second [[color(1)]];
};

vertex float4 fbf_vertex(uint vertex_id [[vertex_id]])
{
    float2 xy = vertex_id == 0 ? float2(-1, -1) :
                vertex_id == 1 ? float2(3, -1) : float2(-1, 3);
    return float4(xy, 0, 1);
}

// Binary channel values survive half and BGRA8 conversion without a tolerance.
fragment FetchTargets fbf_seed(float4 position [[position]])
{
    uint2 p = uint2(position.xy);
    return {half4(p.x & 1, (p.y >> 1) & 1, 0, 1),
            half4(1 - ((p.x >> 1) & 1), p.y & 1, 1, 0)};
}

fragment FetchTargets fbf_pass(half4 first [[color(0)]], half4 second [[color(1)]])
{
    return {first, second};
}

fragment FetchTargets fbf_swap(half4 first [[color(0)]], half4 second [[color(1)]])
{
    return {second, first};
}

// This deliberate wrong-target read must fail the same pixel comparisons.
fragment FetchTargets fbf_wrong_data1(half4 first [[color(0)]])
{
    return {first, first};
}
