# glslang (N2-06)

glslang, the Khronos reference GLSL front end and SPIR-V back end, as a library: it compiles the vertex shaders
`native/src/gpu/shader/` generates (decision D27: Azahar's decompiler, compiled with glslang at run time). Only
what GLSL to SPIR-V needs is here.

| | |
|---|---|
| Release | `16.6.0` (CHANGES.md: 2026-09-10), github.com/KhronosGroup/glslang |
| Commit | `e1b562a8bed273a02f30b59b66a5d499793cede5` (committed 2026-09-11) |
| Licences | `LICENSE.txt` and `LICENSES/` are upstream's; `LICENSE.txt` lists these, and a file can carry more than one of them: **3-clause BSD** (the core: every file of `glslang/` and `SPIRV/` not named below; `LICENSES/BSD-3-Clause.txt`); **the NVIDIA licence** (`LICENSES/AML-glslang.txt`) and the **Khronos 2014-2016 MIT-style licence** on top of the 3-clause BSD one for the preprocessor, `glslang/MachineIndependent/preprocessor/{Pp.cpp,PpAtom.cpp,PpContext.cpp,PpContext.h,PpScanner.cpp,PpTokens.cpp,PpTokens.h}`; **2-clause BSD** (Khronos 2020, `LICENSES/BSD-2-Clause.txt`) for `glslang/Include/{glslang_c_interface.h,glslang_c_shader_types.h}`; **MIT** (the Khronos "Materials" licence of `LICENSE.txt`, `LICENSES/MIT.txt`, `LICENSES/MIT-Khronos-old.txt`) for `SPIRV/{GLSL.std.450.h,GLSL.ext.AMD.h,GLSL.ext.ARM.h,GLSL.ext.EXT.h,GLSL.ext.KHR.h,GLSL.ext.NV.h,GLSL.ext.QCOM.h,NonSemanticDebugPrintf.h,NonSemanticShaderDebugInfo.h,spirv.hpp11}` and, under its own licence that `LICENSE.txt` points to, `glslang/MachineIndependent/gl_types.h`; **Apache-2.0** (`LICENSES/Apache-2.0.txt`) for `SPIRV/{hex_float.h,bitutils.h}`; **GPL-3.0-or-later WITH Bison-exception-2.2** (`LICENSES/GPL-3.0-or-later.txt`, `LICENSES/Bison-exception-2.2.txt`) for the Bison-generated `glslang/MachineIndependent/{glslang_tab.cpp,glslang_tab.cpp.h}` (upstream says the parser can be built from the grammar with other tools; the generated files are vendored as upstream ships them) |

What is vendored: `glslang/{GenericCodeGen,Include,MachineIndependent (with preprocessor),OSDependent,Public,ResourceLimits}`
(the OS-dependent file of Unix and of Windows, `GLSLANG_OSINCLUDE_UNIX` / `GLSLANG_OSINCLUDE_WIN32` choose one) and
`SPIRV/` (`GlslangToSpv`, `SpvBuilder`, `SpvPostProcess` and what they include). Not vendored: the HLSL front end (upstream
deprecates it), the C interfaces, `StandAlone`, the tests, `External/`, the grammar `glslang.y`, spirv-tools (so there is
no `spirv-opt`; `ENABLE_OPT` is 0) and upstream's CMake machinery.

Three files are ours: `CMakeLists.txt` (the static library `mh4n_glslang`; `native/src/gpu/shader/CMakeLists.txt` adds
this directory), `glslang/build_info.h` (upstream generates it from `build_info.h.tmpl` with `build_info.py`; the
version is 16.6.0) and this `README.md`. Every other file is byte-identical to upstream at the commit above.

The blob ids are what `git rev-parse <commit>:<path>` gives in the upstream repository (`git hash-object <file>` gives the
same for the vendored file):

| File | Blob id |
|---|---|
| `LICENSE.txt` | `1ea89c03d7d20e987481c7c56fac71831ba749cc` |
| `LICENSES/AML-glslang.txt` | `2a24aeac2248381b8b58813805d7bc4db0610daa` |
| `LICENSES/Apache-2.0.txt` | `7dfc7aa18a2e891a77ccc1bc2e893708d8e6bb5f` |
| `LICENSES/BSD-2-Clause.txt` | `5682049fdbd719cafbcf1edfc52e35d2bcf70b00` |
| `LICENSES/BSD-3-Clause.txt` | `ea890afbc70c94011b765722a95e2c8c7d4f1233` |
| `LICENSES/Bison-exception-2.2.txt` | `bb5a6b6e80703d4ba691b9c6d66dc729919cf741` |
| `LICENSES/GPL-3.0-or-later.txt` | `f288702d2fa16d3cdf0035b15a9fcbc552cd88e7` |
| `LICENSES/MIT-Khronos-old.txt` | `430863bc98ad8ee1d0a4876a4c2a5b6f5d32fd74` |
| `LICENSES/MIT.txt` | `d817195dad53ec992418c28ffca5fbd1cd86502a` |
| `SPIRV/GLSL.ext.AMD.h` | `009d2f1cf09bc476e08d1859c34608c87ff6aaa0` |
| `SPIRV/GLSL.ext.ARM.h` | `eef5b03e10629a4a0ff83f4a2715239a8740efb6` |
| `SPIRV/GLSL.ext.EXT.h` | `0a96a3e57564ae1bf6e8fc59c09968b6223f19bb` |
| `SPIRV/GLSL.ext.KHR.h` | `1c67b490436957916df5912e163615f5cfd67d06` |
| `SPIRV/GLSL.ext.NV.h` | `47868c8d45ae5c7299d4e63b37c494d4bc27b514` |
| `SPIRV/GLSL.ext.QCOM.h` | `c85521b182472df9067a9964d496d5d8e9f9aee2` |
| `SPIRV/GLSL.std.450.h` | `86d3da80654ed70cf506009c736215e88bd2a679` |
| `SPIRV/GlslangToSpv.cpp` | `23e7e4e5ab00bfaef880586ab42c3a8b496fa28d` |
| `SPIRV/GlslangToSpv.h` | `7ca0987eb107d8da2c1a966ec287c83b91c679d2` |
| `SPIRV/InReadableOrder.cpp` | `18e487d87767550667cda783564d548941b6209a` |
| `SPIRV/Logger.cpp` | `48bd4e3ade6260eaf81909f982cd7bb331813202` |
| `SPIRV/Logger.h` | `c64a3b8a3659b4888044e4a2786752061adb75ee` |
| `SPIRV/NonSemanticDebugPrintf.h` | `3ca7247f2b0eb13cd65731f54ff1a229a9e14b43` |
| `SPIRV/NonSemanticShaderDebugInfo.h` | `f7ffd905f64a2dd30ccf0b0d6e38bfe8d727bf3c` |
| `SPIRV/SpvBuilder.cpp` | `ba31dcea2fdccbc3c1fccdf01795da75c2d4612b` |
| `SPIRV/SpvBuilder.h` | `7061b4e7d52975dcf25802a62206b2949aecce99` |
| `SPIRV/SpvPostProcess.cpp` | `1bf79fee887bb654d9a8655315f4cdbc98fb5508` |
| `SPIRV/SpvTools.cpp` | `6c64fdd65d0f070eb4d13fe2c01691da025babbf` |
| `SPIRV/SpvTools.h` | `6d72beb06d1aa44f41c9813b1b6206e28519f74e` |
| `SPIRV/bitutils.h` | `22e44cec26ea08b08fcbea2fc6e6cb293e3fa50f` |
| `SPIRV/disassemble.cpp` | `f61a3ae9fc574e2c5a30d882db78a6fa64b9009d` |
| `SPIRV/disassemble.h` | `3bded14f2f904aa7c3a3887722dab1f7a4e7e64b` |
| `SPIRV/doc.cpp` | `ca4e6ac4d3b92a7006857d2743f7efe4c813d7ba` |
| `SPIRV/doc.h` | `c818308e2d2b4010660f39c6bcd20c86766208b2` |
| `SPIRV/hex_float.h` | `cc40fe6190eb149158aaab45e5196b4912e897b6` |
| `SPIRV/spirv.hpp11` | `613f740f690a794f9a96a15607eabce5e69f598a` |
| `SPIRV/spvIR.h` | `20dd66bd5a5f67a7eb41adfa4b192ca7d019e6a3` |
| `SPIRV/spvUtil.h` | `d772a30dd433861b6e76eb215ee1f9208ec2d556` |
| `glslang/GenericCodeGen/CodeGen.cpp` | `1ef2449625b95d24a7dd77168e645d12c8073c03` |
| `glslang/GenericCodeGen/Link.cpp` | `5df39b814a7b91b44f47b21b9bff88373f805de4` |
| `glslang/Include/BaseTypes.h` | `4ff7372ac36fc3a3cf50e59b0fab24ddec16fb32` |
| `glslang/Include/Common.h` | `04c11dc412b04ad3702cb278db03fc8fb23f0b54` |
| `glslang/Include/ConstantUnion.h` | `bf6b8a88b647958e5eff70088801d5361c1f6956` |
| `glslang/Include/InfoSink.h` | `8c9b42bb60c1625cfccaf07a07129fabdd76d34f` |
| `glslang/Include/InitializeGlobals.h` | `b7fdd7aabcb0199e157b9cd3b6ddd1416edface7` |
| `glslang/Include/PoolAlloc.h` | `21236edf35408725a2d7acace80e91e8fc8f4287` |
| `glslang/Include/ResourceLimits.h` | `b36c8d6273b06daaace77062b2d4f6b54a8567af` |
| `glslang/Include/ShHandle.h` | `dee47c0dfd44eedf75e189e3772d1b8db3e3c8b9` |
| `glslang/Include/SpirvIntrinsics.h` | `0082a4d4eb325e8d20995080a4e677692f11df7f` |
| `glslang/Include/Types.h` | `2d8eec7095b2d83e343a1f8b2403d86a9c1acfcd` |
| `glslang/Include/arrays.h` | `d47622661e70bfce04e70d25316544cc8546cac4` |
| `glslang/Include/defer.h` | `0fd21e0bb53b6fed24d0c7a3f200dab0bb8f64c9` |
| `glslang/Include/glslang_c_interface.h` | `5d7cc2d3f478d9e4047e5a9649660e8c073f64eb` |
| `glslang/Include/glslang_c_shader_types.h` | `ab91648b658c2bd6aad98d2e2f9a38fdb3ecfaa5` |
| `glslang/Include/intermediate.h` | `029ece522e5ce29357847bcb1b0d34e313bdfa4e` |
| `glslang/Include/visibility.h` | `dd32351f832d2b9d62ace9f8172aecfd0905f2f8` |
| `glslang/MachineIndependent/Constant.cpp` | `f402233aab4e36252fef5a5f9218c0f354c3afb5` |
| `glslang/MachineIndependent/InfoSink.cpp` | `d00c4225666e5a82e6a71f2daf0252936efde925` |
| `glslang/MachineIndependent/Initialize.cpp` | `5b2825bd0610a67bf4412189bc6fd2326513c9bf` |
| `glslang/MachineIndependent/Initialize.h` | `62e9dcae57980f97f054baaa8d55715f97a55a5a` |
| `glslang/MachineIndependent/IntermTraverse.cpp` | `447e8cb6cdce118796b8e71168e3c9c6efcbbaf1` |
| `glslang/MachineIndependent/Intermediate.cpp` | `4004a6ec6ae6380d4a2d13acf73a90b524926e88` |
| `glslang/MachineIndependent/LiveTraverser.h` | `76ccfec12d311ac4607427e27bab87e4d591f9e4` |
| `glslang/MachineIndependent/ParseContextBase.cpp` | `e43cf5134588b9eccd25efb1312b369878d85f3e` |
| `glslang/MachineIndependent/ParseHelper.cpp` | `ec6d553735b510af1a865393b4f9e56f86dd1a48` |
| `glslang/MachineIndependent/ParseHelper.h` | `f508694cc673de94eb4efb86e2773b5f106afac7` |
| `glslang/MachineIndependent/PoolAlloc.cpp` | `40d4c8d1f75d5d1a1b57988507fb8b2cb54f5687` |
| `glslang/MachineIndependent/RemoveTree.cpp` | `29baf1f1a58b8666ba77f0ede24153f2204c6e88` |
| `glslang/MachineIndependent/RemoveTree.h` | `1ed015626b8f3708d76e032d37fab62ac6dd472c` |
| `glslang/MachineIndependent/Scan.cpp` | `ebe35d2bfbe9b0c14b7e6aeee388295aef4334f2` |
| `glslang/MachineIndependent/Scan.h` | `24b75cf7ca659fe52a9bcee7af4deaf17651c02b` |
| `glslang/MachineIndependent/ScanContext.h` | `ce2145eb99d8154c3f1b0ae271cf430aacf5f7a2` |
| `glslang/MachineIndependent/ShaderLang.cpp` | `791044fec4717c016fdfebe39e1c8f0b281dded5` |
| `glslang/MachineIndependent/SpirvIntrinsics.cpp` | `9cb043aad96644a3ab16990e85a0febe7eb02910` |
| `glslang/MachineIndependent/SymbolTable.cpp` | `333d76dff562971621863945c9a2dee82a998392` |
| `glslang/MachineIndependent/SymbolTable.h` | `b9aecb56471693adb0a41a4d3b7cc52a5d0d76aa` |
| `glslang/MachineIndependent/Versions.cpp` | `e80a1ca857db40cea3a466b62baf2d96bcc81663` |
| `glslang/MachineIndependent/Versions.h` | `ed028b39db6fcc9eeead573c918c9f65b1969444` |
| `glslang/MachineIndependent/attribute.cpp` | `e30fd1bb65231fde2f6dfb11b40657edc7540ef4` |
| `glslang/MachineIndependent/attribute.h` | `b049fa898d89312d4bfef891eae6a0c4f8ec369c` |
| `glslang/MachineIndependent/gl_types.h` | `d6c939374a847b6e54b01d1bfffafad644fee887` |
| `glslang/MachineIndependent/glslang_tab.cpp` | `ebde28d7cf67b71d9c5fbaf4df4eefacb221fe22` |
| `glslang/MachineIndependent/glslang_tab.cpp.h` | `43e2b1f576006611d0d522b1d95636ac50c8ae3c` |
| `glslang/MachineIndependent/intermOut.cpp` | `9237de2ea237b06fe04682dc39385319ab079cdf` |
| `glslang/MachineIndependent/iomapper.cpp` | `f38ef56c73ae7939d2d77f3cde9f5103fbb0e3c5` |
| `glslang/MachineIndependent/iomapper.h` | `72ec58c2aaff5429c80bc538eca54c89cf833942` |
| `glslang/MachineIndependent/limits.cpp` | `4404beca4f43690f3b3fd5bb54b76a085ecb526d` |
| `glslang/MachineIndependent/linkValidate.cpp` | `0b3d4e2b8f28c14e52b9bfd19afd8e6c3f649596` |
| `glslang/MachineIndependent/localintermediate.h` | `c5af0bdea05fd2086c0d1f8443d7524c44de6d82` |
| `glslang/MachineIndependent/parseConst.cpp` | `835097234ea5a66b21560a6ba7f87395864dd203` |
| `glslang/MachineIndependent/parseVersions.h` | `cbc2836ec32fa1a780d1a17f242490963103b19b` |
| `glslang/MachineIndependent/preprocessor/Pp.cpp` | `f5a9793c159b11f14d00ccafa66e809d6e5b15e2` |
| `glslang/MachineIndependent/preprocessor/PpAtom.cpp` | `06c2333ef1048a7a008b0a2dcbb85d9a37021a12` |
| `glslang/MachineIndependent/preprocessor/PpContext.cpp` | `12ab9c297d77873bf3be8030125e6291261c57c2` |
| `glslang/MachineIndependent/preprocessor/PpContext.h` | `f164f0ee2e1277605dfc364487cb4d98d364d83f` |
| `glslang/MachineIndependent/preprocessor/PpScanner.cpp` | `a7243de98367c5dd08317262cd28a68fca4328dd` |
| `glslang/MachineIndependent/preprocessor/PpTokens.cpp` | `472df78f16fbd20b755372302832b963aa873bb5` |
| `glslang/MachineIndependent/preprocessor/PpTokens.h` | `486f69afc50bda68fd78a09433b8714b18b5da3e` |
| `glslang/MachineIndependent/propagateNoContraction.cpp` | `763c2395d8f549759e5590d3623e77fe954bb261` |
| `glslang/MachineIndependent/propagateNoContraction.h` | `d849be837f5a860664a8b0ffd1f7695bdfa8d697` |
| `glslang/MachineIndependent/reflection.cpp` | `3fcb72f8ab9d3b6f272c81b27d3d68922910fa0b` |
| `glslang/MachineIndependent/reflection.h` | `185f8010e83b595edad30e2aee40623329bb3f1e` |
| `glslang/MachineIndependent/span.h` | `bd705fead118358105563534b427716492ff55d5` |
| `glslang/OSDependent/Unix/ossource.cpp` | `fbb51f7bd113ff947cd63dd944ee7cecca7fff7f` |
| `glslang/OSDependent/Windows/ossource.cpp` | `d7f89f71b64b504f41ab5e8a4094736edf108991` |
| `glslang/OSDependent/osinclude.h` | `da1c4f69564a5cc9ff95b0622bd21c3cf42c2f9e` |
| `glslang/Public/ResourceLimits.h` | `8245e12d7a2c550595b99d4cb09974959155d3b5` |
| `glslang/Public/ShaderLang.h` | `dc682855b582c154dfc653ed40c70defc7e34f59` |
| `glslang/ResourceLimits/ResourceLimits.cpp` | `79a1b3a6d75b8efd91494f81556c8b6c566e0582` |
