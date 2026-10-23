#version 450
#ifdef INTERLOCK
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_ordered) in;
#endif
layout(set=0, binding=0, r32ui) uniform coherent uimage2D shadow;
void main(){
  ivec2 p = ivec2(gl_FragCoord.xy);
#ifdef INTERLOCK
  beginInvocationInterlockARB();
#endif
#if MODE == 0
  // read-modify-write with plain load/store
  uint o = imageLoad(shadow, p).x;
  imageStore(shadow, p, uvec4(o + 1u));
#else
  // compare-and-swap increment loop
  uint o = imageLoad(shadow, p).x;
  for (int i = 0; i < 1000; ++i) {
    uint n = imageAtomicCompSwap(shadow, p, o, o + 1u);
    if (n == o) break;
    o = n;
  }
#endif
#ifdef INTERLOCK
  endInvocationInterlockARB();
#endif
}
