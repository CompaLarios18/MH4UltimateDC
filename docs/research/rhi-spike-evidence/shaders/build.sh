#!/bin/sh
# compile GLSL to SPIR-V C arrays in ../test/gen
set -e
cd "$(dirname "$0")"
mkdir -p ../test/gen
g() { glslangValidator -V --target-env vulkan1.2 --vn "$1" -o "../test/gen/$1.h" "$2" ${3:-} >/dev/null; }
g spv_fs_vert fs.vert
g spv_tri_vert tri.vert
g spv_tri_frag tri.frag
g spv_sample_frag sample.frag
g spv_blend_frag blend.frag
g spv_shadow_store_nil shadow.frag "-DMODE=0"
g spv_shadow_store_il shadow.frag "-DMODE=0 -DINTERLOCK"
g spv_shadow_cas_nil shadow.frag "-DMODE=1"
g spv_shadow_cas_il shadow.frag "-DMODE=1 -DINTERLOCK"
