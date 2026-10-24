# mh4n_glslang (N2-06): glslang 16.6.0 as one static library, GLSL to SPIR-V only.
#
# Added by native/src/gpu/shader/CMakeLists.txt (add_subdirectory), not by upstream's CMake machinery:
# no HLSL front end, no C interface, no spirv-tools (so no optimizer: ENABLE_OPT is 0), no tests, no
# StandAlone, no install rules. The sources are upstream's files at the pinned commit, unmodified
# (README.md lists their blob ids); only this file and glslang/build_info.h (upstream generates it from
# build_info.h.tmpl) are ours.
#
# Platforms: glslang/OSDependent has one file per OS and GLSLANG_OSINCLUDE_UNIX / GLSLANG_OSINCLUDE_WIN32
# says which one is built (upstream's CMake does the same). llvm-mingw sets WIN32; Linux and macOS build
# the Unix file. The code needs C++17 and uses exceptions and RTTI as upstream's default build does.
add_library(mh4n_glslang STATIC
    glslang/GenericCodeGen/CodeGen.cpp
    glslang/GenericCodeGen/Link.cpp
    glslang/MachineIndependent/Constant.cpp
    glslang/MachineIndependent/InfoSink.cpp
    glslang/MachineIndependent/Initialize.cpp
    glslang/MachineIndependent/IntermTraverse.cpp
    glslang/MachineIndependent/Intermediate.cpp
    glslang/MachineIndependent/ParseContextBase.cpp
    glslang/MachineIndependent/ParseHelper.cpp
    glslang/MachineIndependent/PoolAlloc.cpp
    glslang/MachineIndependent/RemoveTree.cpp
    glslang/MachineIndependent/Scan.cpp
    glslang/MachineIndependent/ShaderLang.cpp
    glslang/MachineIndependent/SpirvIntrinsics.cpp
    glslang/MachineIndependent/SymbolTable.cpp
    glslang/MachineIndependent/Versions.cpp
    glslang/MachineIndependent/attribute.cpp
    glslang/MachineIndependent/glslang_tab.cpp
    glslang/MachineIndependent/intermOut.cpp
    glslang/MachineIndependent/iomapper.cpp
    glslang/MachineIndependent/limits.cpp
    glslang/MachineIndependent/linkValidate.cpp
    glslang/MachineIndependent/parseConst.cpp
    glslang/MachineIndependent/propagateNoContraction.cpp
    glslang/MachineIndependent/reflection.cpp
    glslang/MachineIndependent/preprocessor/Pp.cpp
    glslang/MachineIndependent/preprocessor/PpAtom.cpp
    glslang/MachineIndependent/preprocessor/PpContext.cpp
    glslang/MachineIndependent/preprocessor/PpScanner.cpp
    glslang/MachineIndependent/preprocessor/PpTokens.cpp
    glslang/ResourceLimits/ResourceLimits.cpp
    SPIRV/GlslangToSpv.cpp
    SPIRV/InReadableOrder.cpp
    SPIRV/Logger.cpp
    SPIRV/SpvBuilder.cpp
    SPIRV/SpvPostProcess.cpp
    SPIRV/SpvTools.cpp
    SPIRV/disassemble.cpp
    SPIRV/doc.cpp
    $<IF:$<BOOL:${WIN32}>,glslang/OSDependent/Windows/ossource.cpp,glslang/OSDependent/Unix/ossource.cpp>
)
set_target_properties(mh4n_glslang PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                      POSITION_INDEPENDENT_CODE ON)
# The include root is this directory: "glslang/Public/ShaderLang.h", "SPIRV/GlslangToSpv.h",
# "glslang/build_info.h". SYSTEM, so that our headers' warnings (mh4n_warnings) do not apply to them.
target_include_directories(mh4n_glslang SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(mh4n_glslang PRIVATE
    ENABLE_OPT=0
    $<IF:$<BOOL:${WIN32}>,GLSLANG_OSINCLUDE_WIN32,GLSLANG_OSINCLUDE_UNIX>
    $<$<BOOL:${WIN32}>:NOMINMAX>)
# Third-party code: no warnings of ours (mh4n_warnings is not linked).
target_compile_options(mh4n_glslang PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)
find_package(Threads REQUIRED)
target_link_libraries(mh4n_glslang PUBLIC Threads::Threads)
