#include <metal_stdlib>
using namespace metal;
kernel void ubo_probe(constant float4 &a [[buffer(3)]], constant float4 &b [[buffer(21)]],
                      constant half4 &c [[buffer(5)]], constant half4 &d [[buffer(22)]],
                      device float4 *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i == 0) { out[0] = a; out[1] = b; out[2] = float4(c); out[3] = float4(d); }
}
