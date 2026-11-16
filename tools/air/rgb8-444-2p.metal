#include <metal_stdlib>
using namespace metal;

kernel void rgb8_444_2p(texture2d<float, access::sample> image [[texture(0)]],
                        device float4 *output [[buffer(0)]],
                        uint2 position [[thread_position_in_grid]])
{
    constexpr uint2 extent(17, 9);
    if (any(position >= extent)) return;
    uint2 size(image.get_width(), image.get_height());
    constexpr sampler nearest(coord::normalized, filter::nearest, address::clamp_to_edge);
    // Keep output writes bounded independently of the driver's reported texture size.
    uint index = (position.y * extent.x + position.x) * 2;
    output[index] = image.sample(nearest, (float2(position) + .5f) / float2(size));
    output[index + 1] = image.read(position);
}

struct FullscreenOut {
    float4 position [[position]];
    float2 uv;
};

vertex FullscreenOut rgb8_444_2p_vs(uint vid [[vertex_id]])
{
    float2 pos = float2((vid == 1) ? 3.0f : -1.0f, (vid == 2) ? 3.0f : -1.0f);
    FullscreenOut out;
    out.position = float4(pos, 0.0f, 1.0f);
    out.uv = float2(0.5f * (pos.x + 1.0f), 0.5f * (1.0f - pos.y));
    return out;
}

fragment float4 rgb8_444_2p_blend_fs(FullscreenOut in [[stage_in]],
                                     constant float4 &color [[buffer(0)]])
{
    (void)in;
    return color;
}

fragment float4 rgb8_444_2p_linear_fs(FullscreenOut in [[stage_in]],
                                      texture2d<float, access::sample> src [[texture(0)]])
{
    constexpr sampler lin(coord::normalized, filter::linear, address::clamp_to_edge);
    return src.sample(lin, in.uv);
}
