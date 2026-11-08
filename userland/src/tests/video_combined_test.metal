/* Build for the device runtime explicitly; SDK defaults can emit newer AIR:
 * xcrun -sdk iphoneos metal -mios-version-min=26.0 -std=metal4.0 -c video_combined_test.metal -o video_combined_test.air
 * xcrun -sdk iphoneos metallib video_combined_test.air -o video_combined_test.metallib
 */
#include <metal_stdlib>
using namespace metal;
kernel void combined_sample(texture2d<float, access::sample> image [[texture(0)]],
                            texture2d<float, access::sample> guard [[texture(1)]],
                            device float4 *out [[buffer(0)]]) {
    constexpr sampler s(coord::normalized, filter::nearest);
    out[0] = image.sample(s, float2(0.5), level(0));
    out[1] = guard.sample(s, float2(0.5));
}
fragment float4 combined_fragment(texture2d<float, access::sample> image [[texture(0)]]) {
    constexpr sampler s(coord::normalized, filter::nearest);
    return image.sample(s, float2(0.5));
}
vertex float4 combined_vertex(uint vertex_id [[vertex_id]]) {
    return float4(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0, 0, 1);
}
