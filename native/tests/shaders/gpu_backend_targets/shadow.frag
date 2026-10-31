#version 450
// The shadow write's shape: adds 1 to the R32_UINT texel under the pixel. INTERLOCK: the read-modify-write sits in
// a fragment-shader interlock; without it, a compare-and-swap loop (the fallback the mask runs).
#ifdef INTERLOCK
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_ordered) in;
#endif
layout(set = 0, binding = 0, r32ui) uniform coherent uimage2D shadow;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
#ifdef INTERLOCK
    beginInvocationInterlockARB();
    uint o = imageLoad(shadow, p).x;
    imageStore(shadow, p, uvec4(o + 1u));
    endInvocationInterlockARB();
#else
    uint o = imageLoad(shadow, p).x;
    for (int i = 0; i < 1000; ++i) {
        uint n = imageAtomicCompSwap(shadow, p, o, o + 1u);
        if (n == o) break;
        o = n;
    }
#endif
}
