#include <metal_stdlib>
using namespace metal;

// The 8x8 test dispatch exactly covers this texture. All channel values are
// representable in both half and UNORM8, so readback needs no rounding tolerance.
kernel void pipeline_pattern(texture2d<half, access::write> output [[texture(0)]],
                             uint2 position [[thread_position_in_grid]])
{
    output.write(half4(position.x & 1, position.y & 1,
                       ((position.x >> 1) ^ (position.y >> 1)) & 1, 1), position);
}
