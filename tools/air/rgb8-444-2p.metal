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
