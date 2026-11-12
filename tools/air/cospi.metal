#include <metal_stdlib>
using namespace metal;

kernel void check(device const float2 *in [[buffer(0)]],
                  device float *out [[buffer(1)]],
                  uint i [[thread_position_in_grid]])
{
    out[i] = i < 65536 ? float(cospi(half(in[i].x))) : cospi(in[i].x);
}
