// Derived from Azahar src/video_core/pica/pica_core.cpp, pica_core.h, shader_setup.cpp,
// packed_attribute.h and src/video_core/pica_types.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of those files says "Licensed under GPLv2 or any later version"):
// the expected values are worked out from their logic, and the interrupt delay formula and the
// float24 packing are written out again in this file.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp,
//   pica_core.h, shader_setup.cpp, packed_attribute.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
//
// mh4n_gpu_pica (N2-04): the PICA command processor.
//
// Every command list here is built by this file from synthetic words, and every expected value is
// worked out by hand from the logic of Azahar's pica_core.cpp, shader_setup.cpp and
// packed_attribute.h (pinned commit 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6) and the register
// layouts of regs_internal.h and its neighbours; the derivation is in the comment above each
// check. Nothing is computed by calling the code under test.
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gpu/pica/command_processor.h"
#include "gpu/pica/recording_backend.h"
#include "gpu_capture/capture_host.h"
#include "gpu_capture/mgpu.h"
#include "gpu_capture/player.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

#if defined(_WIN32)
#include <process.h>
#define MH4N_GETPID _getpid
#else
#include <unistd.h>
#define MH4N_GETPID getpid
#endif

using namespace mh4n;
using namespace mh4n::gpu;
using namespace mh4n::gpu::pica;
using mh4n::gpu_capture::CaptureHost;

namespace {

constexpr u32 kFcram = 0x20000000;

// A command header: the register in bits 0-15, the byte mask in 16-19, the number of extra words
// in 20-27, and bit 31 for "the extra words go to consecutive registers".
u32 Header(u32 id, u32 mask, u32 extra, bool group) {
    return id | (mask << 16) | (extra << 20) | (group ? 0x80000000u : 0u);
}

u32 F(float v) {
    return std::bit_cast<u32>(v);
}

// Builds a command list. A command is [value, header, extra words...]; the next one starts on a
// multiple of 8 bytes, so a pad word (which must be skipped) follows a command with an even
// number of extra words.
class List {
public:
    List& Write(u32 id, u32 value, u32 mask = 0xF) {
        return Multi(id, {value}, false, mask);
    }
    // values[0] is the command's value; the others are its extra words.
    List& Multi(u32 id, std::vector<u32> values, bool group = false, u32 mask = 0xF) {
        if (words_.size() % 2 != 0) {
            words_.push_back(0xDEADBEEF);
        }
        words_.push_back(values[0]);
        words_.push_back(Header(id, mask, static_cast<u32>(values.size() - 1), group));
        words_.insert(words_.end(), values.begin() + 1, values.end());
        return *this;
    }
    // A raw word, for the tests of the loop itself.
    List& Raw(u32 word) {
        words_.push_back(word);
        return *this;
    }
    std::vector<u32> Words() const {
        std::vector<u32> w = words_;
        if (w.size() % 2 != 0) {
            w.push_back(0);
        }
        return w;
    }

private:
    std::vector<u32> words_;
};

struct Rig {
    CaptureHost host;
    RecordingBackend backend;
    CommandProcessor cp{host, backend};

    Rig() {
        cp.SetRegisterWriteLog(true);
    }

    // Puts the list in guest memory and returns its size in bytes.
    u32 Place(const List& list, u32 addr) {
        const std::vector<u32> words = list.Words();
        std::vector<u8> bytes(words.size() * 4);
        std::memcpy(bytes.data(), words.data(), bytes.size());
        REQUIRE(host.WriteBoth(addr, bytes.data(), static_cast<u32>(bytes.size())));
        return static_cast<u32>(bytes.size());
    }
    void Run(const List& list, u32 addr = kFcram + 0x1000) {
        const u32 size = Place(list, addr);
        cp.ProcessCmdList(addr, size);
    }
    u32 Reg(u32 id) const {
        return cp.state().regs[id];
    }
    // A draw with whatever is in the registers (and nothing in the dirty flags afterwards).
    void Draw() {
        Run(List().Write(reg::kTriggerDraw, 0));
    }
    const RecordedDraw& LastDraw() const {
        REQUIRE(!backend.draws.empty());
        return backend.draws.back();
    }
};

std::vector<RegisterWrite> W(std::initializer_list<RegisterWrite> writes) {
    return writes;
}

// Three words that Azahar's float24 packing turns into (x, y, z, w) = (0.5, -1.0, 2.0, 1.0).
// PackedAttribute::AsFloat24: x = buffer[2] & 0xFFFFFF; y = ((buffer[1] & 0xFFFF) << 8) |
// (buffer[2] >> 24); z = ((buffer[0] & 0xFF) << 16) | (buffer[1] >> 16); w = buffer[0] >> 8.
// A float24 is sign (bit 23), exponent (bits 16-22, bias 63), mantissa: 0.5 = 62 << 16 =
// 0x3E0000, -1.0 = 0x800000 | 63 << 16 = 0xBF0000, 2.0 = 64 << 16 = 0x400000, 1.0 = 0x3F0000.
//   buffer[2] = x | (y & 0xFF) << 24         = 0x003E0000
//   buffer[1] = (y >> 8) | (z >> 16) << 16   = 0x0000BF00
//   buffer[0] = (z & 0xFF) | w << 8          = 0x3F000040
constexpr u32 kPacked[3] = {0x3F000040, 0x0000BF00, 0x003E0000};

void CheckVec(const Vec4f& v, float x, float y, float z, float w) {
    CHECK(F(v.c[0]) == F(x));
    CHECK(F(v.c[1]) == F(y));
    CHECK(F(v.c[2]) == F(z));
    CHECK(F(v.c[3]) == F(w));
}

} // namespace

// ================================================================================================
// Decoding
// ================================================================================================

TEST_CASE("a command writes the bytes its mask names") {
    // Register 0x120 holds 0xAABBCCDD after a full write. Mask 0b0101 expands to 0x00FF00FF
    // (ExpandBitsToBytes[5]): new = (old & ~0x00FF00FF) | (value & 0x00FF00FF)
    //   = 0xAA00CC00 | 0x00220044 = 0xAA22CC44.
    // Mask 0b1000 is 0xFF000000: new = 0x00220044 ... of the old 0xAA22CC44 -> 0x99_22CC44.
    // Mask 0 changes nothing.
    Rig rig;
    rig.Run(List()
                .Write(0x120, 0xAABBCCDD)
                .Write(0x120, 0x11223344, 0b0101)
                .Write(0x120, 0x99FFFFFF, 0b1000)
                .Write(0x120, 0x55555555, 0b0000));
    CHECK(rig.Reg(0x120) == 0x9922CC44);
    CHECK(rig.cp.TakeRegisterWrites() == W({{0x120, 0xF, 0xAABBCCDD},
                                            {0x120, 0x5, 0x11223344},
                                            {0x120, 0x8, 0x99FFFFFF},
                                            {0x120, 0x0, 0x55555555}}));
}

TEST_CASE("consecutive and repeated forms, and the alignment between commands") {
    // Command 1: group form, header register 0x120, value A; the three extra words go to
    // 0x121, 0x122, 0x123. 2 + 3 = 5 words, so the next command starts after a pad word (0xDEADBEEF
    // in this test, which must not be read as a command).
    // Command 2: register 0x130.
    // Command 3: not the group form: register 0x124 is written 1, 2, 3 in turn; it ends with 3.
    Rig rig;
    rig.Run(List()
                .Multi(0x120, {0xA, 0xB, 0xC, 0xD}, true)
                .Write(0x130, 0x77)
                .Multi(0x124, {1, 2, 3}, false));
    CHECK(rig.Reg(0x120) == 0xA);
    CHECK(rig.Reg(0x121) == 0xB);
    CHECK(rig.Reg(0x122) == 0xC);
    CHECK(rig.Reg(0x123) == 0xD);
    CHECK(rig.Reg(0x130) == 0x77);
    CHECK(rig.Reg(0x124) == 3);
    CHECK(rig.cp.TakeRegisterWrites() == W({{0x120, 0xF, 0xA},
                                            {0x121, 0xF, 0xB},
                                            {0x122, 0xF, 0xC},
                                            {0x123, 0xF, 0xD},
                                            {0x130, 0xF, 0x77},
                                            {0x124, 0xF, 1},
                                            {0x124, 0xF, 2},
                                            {0x124, 0xF, 3}}));
}

TEST_CASE("the mask applies to every word of a multi-word command") {
    // Group form with mask 0b0011 (0x0000FFFF): each register keeps its upper half.
    Rig rig;
    rig.Run(List().Write(0x120, 0xFFFF0000).Write(0x121, 0xEEEE0000));
    rig.Run(List().Multi(0x120, {0x1111, 0x2222}, true, 0b0011));
    CHECK(rig.Reg(0x120) == 0xFFFF1111);
    CHECK(rig.Reg(0x121) == 0xEEEE2222);
}

TEST_CASE("registers past the end are ignored") {
    // 0x300 and 0xFFFF do not exist: no effect, and nothing is reported as written. The group
    // starting at 0x2FD writes 0x2FD, 0x2FE and 0x2FF; the fourth word would be register 0x300
    // and is dropped. The last command shows the list goes on.
    Rig rig;
    rig.Run(List()
                .Write(0x300, 1)
                .Write(0xFFFF, 2)
                .Multi(0x2FD, {1, 2, 3, 4}, true)
                .Multi(0x400, {5, 6, 7}, false)
                .Write(0x121, 9));
    CHECK(rig.Reg(0x2FD) == 1);
    CHECK(rig.Reg(0x2FE) == 2);
    CHECK(rig.Reg(0x2FF) == 3);
    CHECK(rig.Reg(0x121) == 9);
    CHECK(rig.cp.TakeRegisterWrites() == W({{0x2FD, 0xF, 1},
                                            {0x2FE, 0xF, 2},
                                            {0x2FF, 0xF, 3},
                                            {0x121, 0xF, 9}}));
}

TEST_CASE("a command list of an odd number of words, and one in unmapped memory") {
    Rig rig;
    // Three words: the value and header of a command, then a lone word. The loop starts a second
    // command at the next even index (2): its value is the lone word and the header would be word
    // 3, past the end, read from guest memory (zeros there: register 0, mask 0).
    std::vector<u32> words = {0x5, Header(0x120, 0xF, 0, false), 0x77};
    std::vector<u8> bytes(12);
    std::memcpy(bytes.data(), words.data(), 12);
    REQUIRE(rig.host.WriteBoth(kFcram + 0x2000, bytes.data(), 12));
    rig.cp.ProcessCmdList(kFcram + 0x2000, 12);
    CHECK(rig.Reg(0x120) == 5);
    CHECK(rig.cp.TakeRegisterWrites() == W({{0x120, 0xF, 5}, {0x000, 0x0, 0x77}}));

    // A list that is not in guest memory is skipped.
    rig.cp.ProcessCmdList(0x08000000, 64);
    CHECK(rig.cp.TakeRegisterWrites().empty());
    // A list of size 0 does nothing.
    rig.cp.ProcessCmdList(kFcram, 0);
    CHECK(rig.cp.TakeRegisterWrites().empty());
}

TEST_CASE("a list whose last command ends on an odd word is followed by one more command") {
    // PicaCore::ProcessCmdList tests `index < length` before it aligns the index to 8 bytes, so
    // after a command of an odd number of words (padded to 4 words in the list) it reads one more
    // pair from the 8 bytes after the list. Lists of real games end with the finalize write,
    // which stops the processor first; this is what Azahar does with one that does not.
    Rig rig;
    List list;
    list.Multi(0x120, {1, 2}, true); // value, header, one extra = 3 words, padded to 4
    const u32 size = rig.Place(list, kFcram + 0x3000);
    REQUIRE(size == 16);
    List after;
    after.Write(0x130, 0x42);
    rig.Place(after, kFcram + 0x3000 + size);
    rig.cp.ProcessCmdList(kFcram + 0x3000, size);
    CHECK(rig.Reg(0x120) == 1);
    CHECK(rig.Reg(0x121) == 2);
    CHECK(rig.Reg(0x130) == 0x42);
}

TEST_CASE("register defaults before any INIT") {
    // PicaCore::InitializeRegs: irq_autostop 1, irq_mask 0xFFFFFFF0, irq_compare 0x12345678, and
    // the geometry shader unit's input buffer config: max input attribute 1 (bits 0-3) and shader
    // mode VS = 0xA0 (bits 24-31).
    Rig rig;
    CHECK(rig.Reg(0x034) == 1);
    CHECK(rig.Reg(0x030) == 0xFFFFFFF0);
    CHECK(rig.Reg(0x020) == 0x12345678);
    CHECK(rig.Reg(0x289) == 0xA0000001);
    CHECK(rig.Reg(0x120) == 0);
}

TEST_CASE("batch writes of a special register act once per word") {
    // Three draws from one command: the draw register written three times.
    Rig rig;
    rig.Run(List().Multi(0x22E, {0, 0, 0}, false));
    CHECK(rig.backend.draws.size() == 3);
    CHECK(rig.cp.TakeRegisterWrites().size() == 3);
}

// ================================================================================================
// Shader programs and uniforms
// ================================================================================================

TEST_CASE("vertex shader program upload") {
    Rig rig;
    // vs.program.offset is register 0x2CB; the eight data registers are 0x2CC to 0x2D3 (any of
    // them does the same). Offset 4. A one-word write goes to program_code[4] (offset 4 -> 5); a
    // write to alias 0x2D3 goes to [5]; the command at 0x2CE has its own word, which goes to [6],
    // and two extra words, which are a batch and go to [7] and [8] (offset 9).
    rig.Run(List()
                .Write(0x2CB, 4)
                .Write(0x2CC, 0xA0)
                .Write(0x2D3, 0xA1)
                .Multi(0x2CE, {0xA2, 0xA3, 0xA4}, false));
    const auto& vs = rig.cp.state().vs;
    CHECK(vs.program_code[3] == 0);
    CHECK(vs.program_code[4] == 0xA0);
    CHECK(vs.program_code[5] == 0xA1);
    CHECK(vs.program_code[6] == 0xA2);
    CHECK(vs.program_code[7] == 0xA3);
    CHECK(vs.program_code[8] == 0xA4);
    CHECK(rig.Reg(0x2CB) == 9);
    // One more than the highest word changed.
    CHECK(vs.biggest_program_size == 9);
    // Unit 3 follows the vertex shader (gs_unit_exclusive_configuration 0, use_gs No).
    const auto& gs = rig.cp.state().gs;
    CHECK(gs.program_code[4] == 0xA0);
    CHECK(gs.program_code[8] == 0xA4);
    CHECK(gs.biggest_program_size == 9);
    // The offset register of the geometry unit did not move.
    CHECK(rig.Reg(0x29B) == 0);
}

TEST_CASE("program words are not masked, and a word equal to the old one changes nothing") {
    // The register keeps the masked value (only byte 0: 0x78) but UpdateProgramCode gets the whole
    // word.
    Rig rig;
    rig.Run(List().Write(0x2CB, 10).Write(0x2CC, 0x12345678, 0b0001));
    CHECK(rig.Reg(0x2CC) == 0x78);
    CHECK(rig.cp.state().vs.program_code[10] == 0x12345678);
    CHECK(rig.cp.state().vs.biggest_program_size == 11);
    // Writing 0 over the empty word 11 is not a change: the size stays 11, the offset advances.
    rig.Run(List().Write(0x2CC, 0));
    CHECK(rig.cp.state().vs.biggest_program_size == 11);
    CHECK(rig.Reg(0x2CB) == 12);
}

TEST_CASE("the vertex shader program is 512 words; the geometry shader's is 4096") {
    Rig rig;
    // The command's own word is a one-word write (offset 510 -> 511 here); its extra words are a
    // batch, which is refused as a whole when offset + count would pass 512: 511 + 2 > 512, so
    // the words 2 and 3 are dropped and the offset stays 511. A one-word write at 511 is fine
    // (-> 512); at 512 it is refused.
    rig.Run(List()
                .Write(0x2CB, 510)
                .Multi(0x2CC, {1, 2, 3}, false)
                .Write(0x2CB, 511)
                .Write(0x2CC, 7)
                .Write(0x2CC, 8));
    const auto& vs = rig.cp.state().vs;
    CHECK(vs.program_code[510] == 1);
    CHECK(vs.program_code[511] == 7);
    CHECK(vs.program_code[512] == 0);
    CHECK(rig.Reg(0x2CB) == 512);

    // Geometry shader, limit 4096. At 4094: the own word goes to [4094] (-> 4095), the batch of 3
    // is refused (4095 + 3 > 4096). At 4095: the own word goes to [4095] (-> 4096), the batch of
    // one is refused (4096 + 1 > 4096), and a one-word write at 4096 is refused.
    Rig rig2;
    rig2.Run(List()
                 .Write(0x29B, 4094)
                 .Multi(0x29C, {1, 2, 3, 4}, false)
                 .Write(0x29B, 4095)
                 .Multi(0x29D, {5, 6}, false)
                 .Write(0x29C, 9));
    const auto& gs = rig2.cp.state().gs;
    CHECK(gs.program_code[4094] == 1);
    CHECK(gs.program_code[4095] == 5);
    CHECK(rig2.Reg(0x29B) == 4096);
}

TEST_CASE("program uploads go to the geometry unit alone when it is configured on its own") {
    // gs_unit_exclusive_configuration (0x244) bit 0 set, or use_gs (0x229 bits 0-1) not 0.
    for (const u32 setting : {0u, 1u}) {
        Rig rig;
        if (setting == 0) {
            rig.Run(List().Write(0x244, 1));
        } else {
            rig.Run(List().Write(0x229, 2));
        }
        rig.Run(List()
                    .Write(0x2CB, 0)
                    .Write(0x2CC, 0x11)
                    .Write(0x2D5, 0)
                    .Write(0x2D6, 0x22)
                    .Write(0x2B0, 0xFFFF)
                    .Write(0x2B1, 0x04030201)
                    .Write(0x2BD, 0xFFFF)
                    .Write(0x2C0, 0)
                    .Multi(0x2C1, {kPacked[0], kPacked[1], kPacked[2]}, false));
        const auto& s = rig.cp.state();
        CHECK(s.vs.program_code[0] == 0x11);
        CHECK(s.vs.swizzle_data[0] == 0x22);
        CHECK(s.gs.program_code[0] == 0);
        CHECK(s.gs.swizzle_data[0] == 0);
        CHECK_FALSE(s.gs.uniform_b[0]);
        CHECK(s.gs.uniform_i[0][0] == 0);
        CHECK(rig.Reg(0x28D) == 0);
        CheckVec(s.vs.uniform_f[0], 0.5f, -1.f, 2.f, 1.f);
        CheckVec(s.gs.uniform_f[0], 0, 0, 0, 0);
    }
}

TEST_CASE("swizzle pattern upload") {
    // vs.swizzle_patterns.offset 0x2D5, data 0x2D6 to 0x2DD; 4096 words.
    Rig rig;
    rig.Run(List()
                .Write(0x2D5, 2)
                .Write(0x2D6, 0xB0)
                .Multi(0x2DD, {0xB1, 0xB2}, false));
    const auto& s = rig.cp.state();
    CHECK(s.vs.swizzle_data[2] == 0xB0);
    CHECK(s.vs.swizzle_data[3] == 0xB1);
    CHECK(s.vs.swizzle_data[4] == 0xB2);
    CHECK(s.vs.biggest_swizzle_size == 5);
    CHECK(s.gs.swizzle_data[4] == 0xB2);
    CHECK(rig.Reg(0x2D5) == 5);

    // The limit is 4096. At 4094 the command's own word goes to [4094] (-> 4095) and its two
    // extras are refused (4095 + 2 > 4096); a one-word write then fills [4095] (-> 4096); the
    // next ones are refused.
    Rig rig2;
    rig2.Run(List()
                 .Write(0x2D5, 4094)
                 .Multi(0x2D6, {1, 2, 3}, false)
                 .Write(0x2D6, 8)
                 .Write(0x2D6, 9)
                 .Write(0x2D6, 10));
    CHECK(rig2.cp.state().vs.swizzle_data[4094] == 1);
    CHECK(rig2.cp.state().vs.swizzle_data[4095] == 8);
    CHECK(rig2.Reg(0x2D5) == 4096);
}

TEST_CASE("float uniforms in three-word mode") {
    // vs.uniform_setup.index is register 0x2C0 (bits 0-6; bit 31 = float32 mode); the data
    // registers are 0x2C1 to 0x2C8. Index 5, three words: one vector (0.5, -1, 2, 1) at [5],
    // the index becomes 6, and unit 3 gets a copy.
    Rig rig;
    rig.Run(List().Write(0x2C0, 5).Write(0x2C1, kPacked[0]).Write(0x2C8, kPacked[1]).Write(0x2C4, kPacked[2]));
    const auto& s = rig.cp.state();
    CheckVec(s.vs.uniform_f[5], 0.5f, -1.f, 2.f, 1.f);
    CheckVec(s.gs.uniform_f[5], 0.5f, -1.f, 2.f, 1.f);
    CHECK(rig.Reg(0x2C0) == 6);
    CHECK(s.vs.uniform_f[4].c[0] == 0.f);
    CHECK(s.vs.uniform_f[6].c[3] == 0.f);
    CHECK(s.vs.uniforms_dirty);
}

TEST_CASE("float uniforms in float32 mode") {
    // Bit 31 of the index register: four words per vector, component (3 - i) is buffer[i], and
    // the value is the float as it is (not rounded to 24 bits): 1.0000001f = 0x3F800001 survives.
    // Index 7, words 1.0, 2.0, 3.0, 0x3F800001: x = word 3, y = 3.0, z = 2.0, w = 1.0.
    // The format bit stays set when the index advances (-> 8).
    Rig rig;
    rig.Run(List()
                .Write(0x2C0, 0x80000007)
                .Multi(0x2C1, {F(1.f), F(2.f), F(3.f), 0x3F800001}, false));
    const auto& v = rig.cp.state().vs.uniform_f[7];
    CHECK(F(v.c[3]) == F(1.f));
    CHECK(F(v.c[2]) == F(2.f));
    CHECK(F(v.c[1]) == F(3.f));
    CHECK(F(v.c[0]) == 0x3F800001);
    CHECK(rig.Reg(0x2C0) == 0x80000008);
}

TEST_CASE("float uniform batches, and indices past the last uniform") {
    // Six words in one command (value + 5 extras) fill two vectors, [5] and [6]; the second is
    // all zero words = +0.
    Rig rig;
    rig.Run(List()
                .Write(0x2C0, 5)
                .Multi(0x2C1, {kPacked[0], kPacked[1], kPacked[2], 0, 0, 0}, false));
    const auto& s = rig.cp.state();
    CheckVec(s.vs.uniform_f[5], 0.5f, -1.f, 2.f, 1.f);
    CheckVec(s.vs.uniform_f[6], 0, 0, 0, 0);
    CHECK(rig.Reg(0x2C0) == 7);

    // Index 96 is outside the 96 uniforms: ignored (real hardware ignores it too), the index does
    // not move, and the queue is emptied so the next vector starts clean. Index 127 likewise.
    Rig rig2;
    rig2.Run(List()
                 .Write(0x2C0, 96)
                 .Multi(0x2C1, {kPacked[0], kPacked[1], kPacked[2]}, false)
                 .Write(0x2C0, 127)
                 .Write(0x2C1, 1)
                 .Write(0x2C1, 2)
                 .Write(0x2C1, 3));
    for (u32 i = 0; i < kNumFloatUniforms; ++i) {
        CHECK(rig2.cp.state().vs.uniform_f[i].c[0] == 0.f);
    }
    CHECK(rig2.Reg(0x2C0) == 127);

    // A batch gives up at the first out-of-range index: from 95, six words store [95] and drop
    // the second vector; the queue is empty afterwards, so a following one-word-at-a-time vector
    // is [96]-bound and ignored too.
    Rig rig3;
    rig3.Run(List()
                 .Write(0x2C0, 95)
                 .Multi(0x2C1, {kPacked[0], kPacked[1], kPacked[2], 1, 2, 3}, false));
    CheckVec(rig3.cp.state().vs.uniform_f[95], 0.5f, -1.f, 2.f, 1.f);
    CHECK(rig3.Reg(0x2C0) == 96);
}

TEST_CASE("float uniforms written as a consecutive group") {
    // The group form starting at 0x2C0: the first word is the index register itself (index 2),
    // the three extras go to 0x2C1, 0x2C2, 0x2C3 and complete one vector.
    Rig rig;
    rig.Run(List().Multi(0x2C0, {2, kPacked[0], kPacked[1], kPacked[2]}, true));
    CheckVec(rig.cp.state().vs.uniform_f[2], 0.5f, -1.f, 2.f, 1.f);
    CHECK(rig.Reg(0x2C0) == 3);
}

TEST_CASE("a denormal float24 is converted exactly") {
    // Float24ToFloat: exponent field 0, mantissa 1 = 2^-62 * 2^-16 = 2^-78 (bias 63: smallest
    // normal exponent 1 is 2^-62; mantissa has 16 bits). Raw 0x000001 -> 2^-78.
    // Zero with the sign bit set is -0.0; exponent field 127 is infinity (mantissa 0) or NaN.
    CHECK(F(Float24ToFloat(0x000001)) == F(std::ldexp(1.0f, -78)));
    CHECK(F(Float24ToFloat(0x800000)) == 0x80000000);
    CHECK(F(Float24ToFloat(0x000000)) == 0);
    CHECK(F(Float24ToFloat(0x7F0000)) == 0x7F800000);
    CHECK(std::isnan(Float24ToFloat(0x7F0001)));
    CHECK(F(Float24ToFloat(0xBF0000)) == F(-1.f));
}

TEST_CASE("boolean and integer uniforms") {
    // vs.bool_uniforms (0x2B0) keeps bits 0-15 as uniform_b: 0xA5C3 = 1010 0101 1100 0011 has
    // bits 0, 1, 6, 7, 8, 10, 13 and 15. The upper half of the written word is not used.
    // vs.int_uniforms[1] (0x2B2) = 0x04030201: x = 1, y = 2, z = 3, w = 4 (bits 0-7, 8-15, ...).
    // Unit 3 follows (default configuration).
    Rig rig;
    rig.Run(List().Write(0x2B0, 0xDEADA5C3).Write(0x2B2, 0x04030201));
    const auto& s = rig.cp.state();
    const bool expected[16] = {1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 0, 1};
    for (u32 i = 0; i < 16; ++i) {
        CHECK(s.vs.uniform_b[i] == static_cast<bool>(expected[i]));
        CHECK(s.gs.uniform_b[i] == static_cast<bool>(expected[i]));
    }
    CHECK(s.vs.uniform_i[1] == std::array<u8, 4>{1, 2, 3, 4});
    CHECK(s.gs.uniform_i[1] == std::array<u8, 4>{1, 2, 3, 4});
    CHECK(s.vs.uniform_i[0] == std::array<u8, 4>{0, 0, 0, 0});

    // The geometry shader's own registers (0x280, 0x281 to 0x284) touch only unit 3.
    rig.Run(List().Write(0x280, 0x1).Write(0x284, 0x08070605));
    CHECK(rig.cp.state().gs.uniform_b[0]);
    CHECK(rig.cp.state().gs.uniform_i[3] == std::array<u8, 4>{5, 6, 7, 8});
    CHECK(rig.cp.state().vs.uniform_i[3] == std::array<u8, 4>{0, 0, 0, 0});
    // 0x280 replaced the whole mirrored bool set of unit 3 with bit 0.
    CHECK_FALSE(rig.cp.state().gs.uniform_b[1]);
    CHECK(rig.cp.state().vs.uniform_b[1]);
}

TEST_CASE("the vertex shader output mask is copied to unit 3") {
    // vs.output_mask (0x2BD) -> gs.output_mask (0x28D), 16 bits of the written word.
    Rig rig;
    rig.Run(List().Write(0x2BD, 0xABCD1234));
    CHECK(rig.Reg(0x28D) == 0x1234);
}

// ================================================================================================
// Attributes and immediate mode
// ================================================================================================

TEST_CASE("default attributes") {
    // 0x232 is the index. Index 2, then three words through the three data aliases (0x233,
    // 0x235, 0x234 -- the first goes through the single path, the others through a batch):
    // attribute [2] = (0.5, -1, 2, 1), and the index becomes 3. A second attribute goes to [3].
    Rig rig;
    rig.Run(List()
                .Write(0x232, 2)
                .Multi(0x233, {kPacked[0], kPacked[1], kPacked[2]}, false)
                .Write(0x234, kPacked[0])
                .Write(0x235, kPacked[1])
                .Write(0x233, kPacked[2]));
    const auto& s = rig.cp.state();
    CheckVec(s.default_attributes[2], 0.5f, -1.f, 2.f, 1.f);
    CheckVec(s.default_attributes[3], 0.5f, -1.f, 2.f, 1.f);
    CHECK(rig.Reg(0x232) == 4);
    CHECK(rig.backend.draws.empty());
}

TEST_CASE("immediate mode draws one vertex when the last attribute arrives") {
    // Index 0xF is immediate mode; vs.max_input_attrib_index (0x242) = 1 means two attributes per
    // vertex. 6 words = 2 attributes = one vertex: a draw. The second vertex is another draw.
    // The first draw reconfigures the geometry pipeline (writing the index reset it), the second
    // does not. Writing the index again re-arms it.
    Rig rig;
    rig.Run(List().Write(0x232, 0xF).Write(0x242, 1));
    rig.Run(List().Multi(0x233, {kPacked[0], kPacked[1], kPacked[2], 0, 0, 0}, false));
    REQUIRE(rig.backend.draws.size() == 1);
    {
        const RecordedDraw& d = rig.backend.draws[0];
        CHECK(d.call.kind == DrawKind::Immediate);
        CHECK(d.call.num_vertices == 1);
        CHECK(d.call.geometry_reconfigured);
        CHECK(d.call.sequence == 0);
        CheckVec(d.state->immediate.input_vertex[0], 0.5f, -1.f, 2.f, 1.f);
        CheckVec(d.state->immediate.input_vertex[1], 0, 0, 0, 0);
    }
    CHECK(rig.cp.state().immediate.current_attribute == 0);
    CHECK(rig.Reg(0x232) == 0xF); // not advanced in immediate mode

    rig.Run(List().Multi(0x233, {0, 0, 0, kPacked[0], kPacked[1], kPacked[2]}, false));
    REQUIRE(rig.backend.draws.size() == 2);
    CHECK_FALSE(rig.backend.draws[1].call.geometry_reconfigured);
    CHECK(rig.backend.draws[1].call.sequence == 1);
    CheckVec(rig.backend.draws[1].state->immediate.input_vertex[1], 0.5f, -1.f, 2.f, 1.f);

    rig.Run(List().Write(0x232, 0xF).Multi(0x233, {0, 0, 0, 0, 0, 0}, false));
    REQUIRE(rig.backend.draws.size() == 3);
    CHECK(rig.backend.draws[2].call.geometry_reconfigured);
}

TEST_CASE("an attribute index past 0xF stores nothing and leaves the queue usable") {
    Rig rig;
    rig.Run(List().Write(0x232, 0x10).Multi(0x233, {1, 2, 3, 4, 5, 6, 7}, false));
    for (const auto& a : rig.cp.state().default_attributes) {
        CHECK(a.c[0] == 0.f);
    }
    CHECK(rig.backend.draws.empty());
    rig.Run(List().Write(0x232, 1).Multi(0x233, {kPacked[0], kPacked[1], kPacked[2]}, false));
    CheckVec(rig.cp.state().default_attributes[1], 0.5f, -1.f, 2.f, 1.f);
}

// ================================================================================================
// Look-up tables
// ================================================================================================

TEST_CASE("lighting look-up table uploads") {
    // lut_config (0x1C5): index bits 0-7, type bits 8-12; data registers 0x1C8 to 0x1CF.
    // Type 5, index 250. The first word goes through the single path (250 -> 251), the seven
    // extras through the batch, which wraps the index modulo 256: 251..255, 0, 1. Afterwards the
    // index is (251 + 7) & 0xFF = 2 and the type is untouched.
    Rig rig;
    rig.Draw(); // clears the dirty flags
    rig.Run(List()
                .Write(0x1C5, (5u << 8) | 250)
                .Multi(0x1C8, {0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107}, false));
    const auto& l = rig.cp.state().lighting;
    for (u32 i = 0; i < 6; ++i) {
        CHECK(l.luts[5][250 + i] == 0x100 + i);
    }
    CHECK(l.luts[5][0] == 0x106);
    CHECK(l.luts[5][1] == 0x107);
    CHECK(l.luts[4][250] == 0);
    CHECK(rig.Reg(0x1C5) == ((5u << 8) | 2));
    rig.Draw();
    // Only table 5 changed since the last draw.
    CHECK(rig.LastDraw().state->lighting.lut_dirty == (1u << 5));
    // The same values again: nothing differs, nothing is dirty.
    rig.Run(List()
                .Write(0x1C5, (5u << 8) | 250)
                .Multi(0x1CF, {0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107}, false));
    rig.Draw();
    CHECK(rig.LastDraw().state->lighting.lut_dirty == 0);

    // Table 24 and up do not exist: the words are dropped, the index still advances.
    Rig rig2;
    rig2.Run(List().Write(0x1C5, (24u << 8) | 3).Write(0x1C8, 7).Multi(0x1C8, {1, 2}, false));
    CHECK(rig2.Reg(0x1C5) == ((24u << 8) | 6));
}

TEST_CASE("fog look-up table uploads") {
    // fog_lut_offset (0x0E6, 16 bits); data 0x0E8 to 0x0EF; 128 entries, indexed modulo 128.
    // Offset 126: the first word -> [126] (offset 127); the four extras -> 127, 0, 1, 2
    // (offset 131: the register is 16 bits wide and does not wrap at 128).
    Rig rig;
    rig.Draw();
    rig.Run(List().Write(0x0E6, 126).Multi(0x0E8, {1, 2, 3, 4, 5}, false));
    const auto& f = rig.cp.state().fog;
    CHECK(f.lut[126] == 1);
    CHECK(f.lut[127] == 2);
    CHECK(f.lut[0] == 3);
    CHECK(f.lut[1] == 4);
    CHECK(f.lut[2] == 5);
    CHECK(rig.Reg(0x0E6) == 131);
    rig.Draw();
    CHECK(rig.LastDraw().state->fog.lut_dirty);
    rig.Run(List().Write(0x0E6, 126).Multi(0x0EF, {1, 2, 3, 4, 5}, false));
    rig.Draw();
    CHECK_FALSE(rig.LastDraw().state->fog.lut_dirty);
}

TEST_CASE("procedural texture look-up table uploads") {
    // proctex_lut_config (0x0AF): index bits 0-7, table bits 8-11; data 0x0B0 to 0x0B7.
    // Tables: 0 noise (128 entries), 2 colour map (128), 3 alpha map (128), 4 colour (256), 5
    // colour difference (256); table n marks bit n of table_dirty. Others are not tables: the
    // words are dropped and the index advances.
    Rig rig;
    rig.Draw();
    // Table 2, index 126: [126] = 10 (single), then the batch of three: 127, 0, 1.
    rig.Run(List().Write(0x0AF, (2u << 8) | 126).Multi(0x0B0, {10, 11, 12, 13}, false));
    const auto& p = rig.cp.state().proctex;
    CHECK(p.color_map_table[126] == 10);
    CHECK(p.color_map_table[127] == 11);
    CHECK(p.color_map_table[0] == 12);
    CHECK(p.color_map_table[1] == 13);
    CHECK(rig.Reg(0x0AF) == ((2u << 8) | 130));
    rig.Draw();
    CHECK(rig.LastDraw().state->proctex.table_dirty == (1u << 2));

    // Table 4, index 254: [254] = 20, then 255 and 256 % 256 = 0. The index register ends at
    // (255 + 2) & 0xFF = 1.
    rig.Run(List().Write(0x0AF, (4u << 8) | 254).Multi(0x0B1, {20, 21, 22}, false));
    CHECK(p.color_table[254] == 20);
    CHECK(p.color_table[255] == 21);
    CHECK(p.color_table[0] == 22);
    CHECK(rig.Reg(0x0AF) == ((4u << 8) | 1));
    // Tables 3, 5 and 0.
    rig.Run(List()
                .Write(0x0AF, (3u << 8) | 5)
                .Write(0x0B2, 30)
                .Write(0x0AF, (5u << 8) | 255)
                .Write(0x0B3, 40)
                .Write(0x0AF, (0u << 8) | 0)
                .Write(0x0B4, 50));
    CHECK(p.alpha_map_table[5] == 30);
    CHECK(p.color_diff_table[255] == 40);
    CHECK(p.noise_table[0] == 50);
    rig.Draw();
    CHECK(rig.LastDraw().state->proctex.table_dirty == ((1u << 4) | (1u << 3) | (1u << 5) | (1u << 0)));

    // Table 1 is not a table.
    rig.Run(List().Write(0x0AF, (1u << 8) | 5).Write(0x0B0, 99));
    CHECK(rig.Reg(0x0AF) == ((1u << 8) | 6));
    rig.Draw();
    CHECK(rig.LastDraw().state->proctex.table_dirty == 0);
}

// ================================================================================================
// Command-list jumps
// ================================================================================================

TEST_CASE("a write to a command buffer trigger jumps to that list") {
    // Channel 0: size 0x238, address 0x23A, trigger 0x23C (units of 8 bytes). List B sets 0x120
    // and 0x121; list A sets 0x120, points channel 0 at B, triggers, and has a command after the
    // trigger that is never executed.
    Rig rig;
    const u32 b_addr = kFcram + 0x4000;
    const u32 b_size = rig.Place(List().Write(0x120, 0xB1).Write(0x121, 0xB2), b_addr);
    REQUIRE(b_size == 16);
    rig.Run(List()
                .Write(0x120, 0xA1)
                .Write(0x23A, b_addr >> 3)
                .Write(0x238, b_size >> 3)
                .Write(0x23C, 1)
                .Write(0x122, 0xA2));
    CHECK(rig.Reg(0x120) == 0xB1);
    CHECK(rig.Reg(0x121) == 0xB2);
    CHECK(rig.Reg(0x122) == 0);
    CHECK(rig.Reg(0x23C) == 1); // the jump trigger is not cleared (only a submitted channel is)
    CHECK(rig.cp.TakeRegisterWrites() == W({{0x120, 0xF, 0xA1},
                                            {0x23A, 0xF, b_addr >> 3},
                                            {0x238, 0xF, b_size >> 3},
                                            {0x23C, 0xF, 1},
                                            {0x120, 0xF, 0xB1},
                                            {0x121, 0xF, 0xB2}}));

    // Channel 1 (0x239, 0x23B, 0x23D) does the same.
    Rig rig2;
    const u32 c_addr = kFcram + 0x4000;
    const u32 c_size = rig2.Place(List().Write(0x120, 0xC1), c_addr);
    rig2.Run(List()
                 .Write(0x23B, c_addr >> 3)
                 .Write(0x239, c_size >> 3)
                 .Write(0x23D, 1)
                 .Write(0x122, 0xA2));
    CHECK(rig2.Reg(0x120) == 0xC1);
    CHECK(rig2.Reg(0x122) == 0);

    // A jump to an address outside guest memory ends the list (Azahar would read through a null
    // pointer); the next list runs normally.
    Rig rig3;
    rig3.Run(List().Write(0x23A, 0x01000000 >> 3).Write(0x238, 4).Write(0x23C, 1).Write(0x120, 1));
    CHECK(rig3.Reg(0x120) == 0);
    rig3.Run(List().Write(0x120, 2));
    CHECK(rig3.Reg(0x120) == 2);
}

TEST_CASE("submitting a command list through the command buffer registers") {
    // GPU::SubmitCmdList: the trigger must be set; the list is read from address * 8 for size *
    // 8 bytes; the trigger is cleared afterwards. The GX command writes channel 0's registers
    // first: address >> 3 (28 bits), size >> 3 (20 bits), trigger 1.
    Rig rig;
    const u32 addr = kFcram + 0x6000;
    const u32 size = rig.Place(List().Write(0x120, 0x55), addr);
    rig.cp.SubmitCmdList(0); // trigger not set: nothing
    CHECK(rig.Reg(0x120) == 0);
    rig.cp.SubmitGxCmdList(addr, size);
    CHECK(rig.Reg(0x120) == 0x55);
    CHECK(rig.Reg(0x23A) == (addr >> 3));
    CHECK(rig.Reg(0x238) == (size >> 3));
    CHECK(rig.Reg(0x23C) == 0);
    CHECK(rig.cp.command_lists_processed() == 1);

    // GPU::WriteReg of the trigger register submits the list the registers name.
    rig.cp.WriteHardwareRegister(0x120, 0);
    rig.cp.WriteHardwareRegister(0x23C, 1);
    CHECK(rig.Reg(0x120) == 0x55);
    CHECK(rig.Reg(0x23C) == 0);
    // Channel 1.
    rig.cp.WriteHardwareRegister(0x23B, addr >> 3);
    rig.cp.WriteHardwareRegister(0x239, size >> 3);
    rig.cp.WriteHardwareRegister(0x120, 0);
    rig.cp.WriteHardwareRegister(0x23D, 1);
    CHECK(rig.Reg(0x120) == 0x55);
    CHECK(rig.Reg(0x23D) == 0);
    CHECK(rig.cp.command_lists_processed() == 3);
}

// ================================================================================================
// Interrupts
// ================================================================================================

TEST_CASE("the finalize register signals P3D when a byte matches, and stops the list") {
    // irq_request (0x010) is compared byte by byte with irq_compare (0x020 = 0x12345678 after
    // reset). Writing 0x78: byte 0 equals 0x78, so P3D is signalled and, with irq_autostop (0x034)
    // 1, the rest of the list is dropped. Two commands had run when the handler ran (the first
    // write and this one): the delay is 2 * 22.4 ns = 44.8 -> 44.
    Rig rig;
    rig.Run(List().Write(0x120, 1).Write(0x010, 0x78).Write(0x121, 2));
    CHECK(rig.Reg(0x121) == 0);
    auto irqs = rig.host.TakeSignalled();
    REQUIRE(irqs.size() == 1);
    CHECK(irqs[0].id == Interrupt::P3D);
    CHECK(irqs[0].delay_ns == 44);
    CHECK(rig.cp.TakeRegisterWrites() == W({{0x120, 0xF, 1}, {0x010, 0xF, 0x78}}));

    // No byte matches (01 01 01 01 against 78 56 34 12): nothing signalled, the list goes on.
    rig.Run(List().Write(0x010, 0x01010101).Write(0x121, 2));
    CHECK(rig.Reg(0x121) == 2);
    CHECK(rig.host.TakeSignalled().empty());
}

TEST_CASE("without autostop the list continues; the delay counts commands since the last interrupt") {
    // autostop off (the first command). Then a match (delay: 2 commands so far = 44), a write,
    // and another match through byte 1 (0x5600 against ..56..): 2 commands since = 44.
    Rig rig;
    rig.Run(List()
                .Write(0x034, 0)
                .Write(0x010, 0x78)
                .Write(0x121, 2)
                .Write(0x010, 0x5600)
                .Write(0x122, 3));
    CHECK(rig.Reg(0x121) == 2);
    CHECK(rig.Reg(0x122) == 3);
    auto irqs = rig.host.TakeSignalled();
    REQUIRE(irqs.size() == 2);
    CHECK(irqs[0].delay_ns == 44);
    CHECK(irqs[1].delay_ns == 44);
}

TEST_CASE("the interrupt delay counts triangles by topology") {
    // pica_core.h: 22.4 ns per command, 400 ns per triangle with 35% of the triangles costing
    // 20% (re-written here from that formula). Commands: 4 (6 vertices, list topology, a draw, the
    // finalize). A list of 6 vertices is 2 triangles.
    Rig rig;
    rig.Run(List().Write(0x228, 6).Write(0x25E, 0).Write(0x22E, 0).Write(0x010, 0x78));
    float expected = 4 * 22.4f;
    expected += (1.f - 0.35f) * 2 * 400.f;
    expected += 0.35f * 2 * (400.f * 0.20f);
    auto irqs = rig.host.TakeSignalled();
    REQUIRE(irqs.size() == 1);
    CHECK(irqs[0].delay_ns == static_cast<u64>(expected));

    // A strip of 5 vertices is 5 - 2 = 3 triangles; a fan of 2 vertices counts 1 (fewer than 3).
    Rig rig2;
    rig2.Run(List().Write(0x228, 5).Write(0x25E, 1u << 8).Write(0x22E, 0).Write(0x010, 0x78));
    float strip = 4 * 22.4f;
    strip += (1.f - 0.35f) * 3 * 400.f;
    strip += 0.35f * 3 * (400.f * 0.20f);
    CHECK(rig2.host.TakeSignalled().at(0).delay_ns == static_cast<u64>(strip));
    Rig rig3;
    rig3.Run(List().Write(0x228, 2).Write(0x25E, 2u << 8).Write(0x22E, 0).Write(0x010, 0x78));
    float fan = 4 * 22.4f;
    fan += (1.f - 0.35f) * 1 * 400.f;
    fan += 0.35f * 1 * (400.f * 0.20f);
    CHECK(rig3.host.TakeSignalled().at(0).delay_ns == static_cast<u64>(fan));
}

// ================================================================================================
// Draws
// ================================================================================================

TEST_CASE("draw triggers produce draw calls") {
    // vertex_attributes.base_address is bits 1-28 of register 0x200, in units of 16 bytes:
    // 0x20100000 / 16 = 0x2010000, stored shifted left by one = 0x4020000. 0x227 is the index
    // array: offset in bits 0-27, bit 31 = 16-bit indices. 0x228 is the vertex count, 0x22A the
    // first vertex.
    Rig rig;
    rig.Run(List()
                .Write(0x200, 0x4020000)
                .Write(0x228, 6)
                .Write(0x22A, 2)
                .Write(0x2BA, 0x1234ABCD)
                .Write(0x22E, 0));
    REQUIRE(rig.backend.draws.size() == 1);
    {
        const DrawCall& c = rig.backend.draws[0].call;
        CHECK(c.kind == DrawKind::Arrays);
        CHECK(c.sequence == 0);
        CHECK(c.vertex_base_address == 0x20100000);
        CHECK(c.num_vertices == 6);
        CHECK(c.vertex_offset == 2);
        CHECK(c.geometry_reconfigured);
        // The vertex shader starts at main_offset (bits 0-15 of 0x2BA).
        CHECK(rig.backend.draws[0].state->vs.entry_point == 0xABCD);
        CHECK(rig.backend.draws[0].state->regs[0x228] == 6);
        // The first draw sees everything dirty.
        CHECK(rig.backend.draws[0].state->dirty.Test(0x200));
        CHECK(rig.backend.draws[0].state->dirty.Test(0x100));
        CHECK(rig.backend.draws[0].state->vs.program_dirty);
        CHECK(rig.backend.draws[0].state->lighting.lut_dirty == 0xFFFFFF);
        CHECK(rig.backend.draws[0].state->proctex.table_dirty == 0xFF);
        CHECK(rig.backend.draws[0].state->fog.lut_dirty);
    }
    rig.Run(List().Write(0x227, 0x80000040).Write(0x22F, 0));
    REQUIRE(rig.backend.draws.size() == 2);
    {
        const DrawCall& c = rig.backend.draws[1].call;
        CHECK(c.kind == DrawKind::Indexed);
        CHECK(c.sequence == 1);
        CHECK(c.vertex_base_address == 0x20100000);
        CHECK(c.num_vertices == 6);
        CHECK(c.index_offset == 0x40);
        CHECK(c.index_u16);
        // Since the first draw returned only 0x22E (written after the draw) and 0x227 changed;
        // the trigger itself is marked after its handler.
        const auto& d = rig.backend.draws[1].state->dirty;
        CHECK(d.Test(0x227));
        CHECK(d.Test(0x22E));
        CHECK_FALSE(d.Test(0x228));
        CHECK_FALSE(d.Test(0x200));
        CHECK_FALSE(rig.backend.draws[1].state->vs.program_dirty);
        CHECK(rig.backend.draws[1].state->lighting.lut_dirty == 0);
    }
    rig.Run(List().Write(0x227, 0x00000008).Write(0x22F, 0));
    CHECK_FALSE(rig.backend.draws[2].call.index_u16);
    CHECK(rig.backend.draws[2].call.index_offset == 8);
}

TEST_CASE("a trigger inside a consecutive group draws once") {
    // The group form starting at 0x22C writes 0x22C, 0x22D and 0x22E (the draw register).
    Rig rig;
    rig.Run(List().Multi(0x22C, {9, 1, 2}, true));
    CHECK(rig.backend.draws.size() == 1);
    CHECK(rig.cp.TakeRegisterWrites() == W({{0x22C, 0xF, 9}, {0x22D, 0xF, 1}, {0x22E, 0xF, 2}}));
}

TEST_CASE("the state a backend keeps is a copy") {
    Rig rig;
    rig.Run(List().Write(0x120, 1).Write(0x22E, 0).Write(0x120, 2).Write(0x22E, 0));
    REQUIRE(rig.backend.draws.size() == 2);
    CHECK(rig.backend.draws[0].state->regs[0x120] == 1);
    CHECK(rig.backend.draws[1].state->regs[0x120] == 2);
}

// ================================================================================================
// Geometry pipeline
// ================================================================================================

TEST_CASE("geometry pipeline setup") {
    // use_gs: bits 0-1 of 0x229 (2 = yes), variable_primitive: bit 31. The geometry unit's input
    // buffer config (0x289): max input attribute in bits 0-3, input_to_uniform in bits 8-15,
    // shader mode in bits 24-31 (0x08 = GS). The vertex output count is bits 0-3 of 0x24A (and
    // 0x251) plus one. gs_config (0x252): mode bits 0-7, fixed vertex count - 1 bits 8-11,
    // stride - 1 bits 12-15, start index bits 16-23.
    {
        // Geometry shader off.
        Rig rig;
        rig.Draw();
        CHECK(rig.LastDraw().state->geometry.mode == GeometryMode::None);
    }
    {
        // Point mode: the geometry shader takes 3 + 1 = 4 inputs, the vertex shader gives 2.
        Rig rig;
        rig.Run(List()
                    .Write(0x229, 2)
                    .Write(0x289, (0x08u << 24) | 3)
                    .Write(0x24A, 1)
                    .Write(0x251, 1)
                    .Write(0x252, 0)
                    .Write(0x28A, 0x00FF0042));
        rig.Draw();
        const GeometryPipelineState& g = rig.LastDraw().state->geometry;
        CHECK(g.mode == GeometryMode::Point);
        CHECK(g.vs_output_num == 2);
        CHECK(g.buffer_index == 0);
        CHECK(g.buffer_size == 4);
        CHECK_FALSE(g.need_index);
        CHECK(rig.LastDraw().state->gs.entry_point == 0x42);
    }
    {
        // Variable primitive mode: it waits for the vertex count (need_index).
        Rig rig;
        rig.Run(List()
                    .Write(0x229, 0x80000002)
                    .Write(0x289, (0x08u << 24) | (1u << 8) | 3)
                    .Write(0x24A, 1)
                    .Write(0x251, 1)
                    .Write(0x252, 1));
        rig.Draw();
        const GeometryPipelineState& g = rig.LastDraw().state->geometry;
        CHECK(g.mode == GeometryMode::VariablePrimitive);
        CHECK(g.vs_output_num == 2);
        CHECK(g.need_index);
    }
    {
        // Fixed primitive mode: 2 + 1 = 3 vertices per invocation, 2 attributes each (stride 1 + 1),
        // buffered in the float uniforms from index 10: [10, 10 + 2 * 3).
        Rig rig;
        rig.Run(List()
                    .Write(0x229, 2)
                    .Write(0x289, (0x08u << 24) | (1u << 8) | 3)
                    .Write(0x24A, 1)
                    .Write(0x251, 1)
                    .Write(0x252, 2u | (2u << 8) | (1u << 12) | (10u << 16)));
        rig.Draw();
        const GeometryPipelineState& g = rig.LastDraw().state->geometry;
        CHECK(g.mode == GeometryMode::FixedPrimitive);
        CHECK(g.vs_output_num == 2);
        CHECK(g.buffer_offset == 10);
        CHECK(g.buffer_index == 10);
        CHECK(g.buffer_size == 16);
    }
    {
        // A mode that does not exist (3) is ignored instead of crashing.
        Rig rig;
        rig.Run(List().Write(0x229, 2).Write(0x252, 3));
        rig.Draw();
        CHECK(rig.LastDraw().state->geometry.mode == GeometryMode::None);
    }
}

// ================================================================================================
// INIT
// ================================================================================================

namespace {

using Bytes = std::vector<u8>;

void Set32(Bytes& b, size_t offset, u32 value) {
    REQUIRE(offset + 4 <= b.size());
    std::memcpy(b.data() + offset, &value, 4);
}

// Offsets inside a shader setup block (mgpu-format.md section 4).
constexpr size_t kSetupProgram = 12;
constexpr size_t kSetupSwizzle = 16396;
constexpr size_t kSetupFloat = 32780;
constexpr size_t kSetupBool = 34316;
constexpr size_t kSetupInt = 34332;
constexpr size_t kSetupQueue = 34348;

struct InitBlocks {
    std::array<Bytes, gpu::kInitBlockCount + 1> data;

    InitBlocks() {
        const size_t sizes[] = {0, 4296, 4096, 3072, 34368, 34368, 256, 24576, 512, 3584, 284, 288};
        for (u32 kind = 1; kind <= gpu::kInitBlockCount; ++kind) {
            data[kind].assign(sizes[kind], 0);
        }
    }
    Bytes& operator[](InitBlock kind) {
        return data[static_cast<u32>(kind)];
    }
    gpu::InitState State() const {
        gpu::InitState state;
        for (u32 kind = 1; kind <= gpu::kInitBlockCount; ++kind) {
            state.blocks[kind] = std::span<const u8>(data[kind]);
        }
        return state;
    }
};

} // namespace

TEST_CASE("LoadInitState fills the register file, the shader setups, the tables and the attributes") {
    InitBlocks init;
    // PICA_REGS: word n is register n.
    Set32(init[InitBlock::PicaRegs], 4 * 0x120, 0xCAFE);
    Set32(init[InitBlock::PicaRegs], 4 * 0x2CB, 7);            // vs program offset
    Set32(init[InitBlock::PicaRegs], 4 * 0x2C0, 0x80000003);   // vs uniforms: float32 mode, index 3
    Set32(init[InitBlock::PicaRegs], 4 * 0x034, 0);
    // VS_SETUP.
    Bytes& vs = init[InitBlock::VsSetup];
    Set32(vs, 0, 9);  // entry_point
    Set32(vs, 4, 3);  // biggest_program_size
    Set32(vs, 8, 2);  // biggest_swizzle_size
    Set32(vs, kSetupProgram + 4 * 0, 0x111);
    Set32(vs, kSetupProgram + 4 * 2, 0x333);
    Set32(vs, kSetupSwizzle + 4 * 1, 0x444);
    Set32(vs, kSetupFloat + 16 * 2 + 0, F(1.f));
    Set32(vs, kSetupFloat + 16 * 2 + 4, F(2.f));
    Set32(vs, kSetupFloat + 16 * 2 + 8, F(3.f));
    Set32(vs, kSetupFloat + 16 * 2 + 12, F(4.f));
    vs[kSetupBool + 3] = 1;
    vs[kSetupBool + 4] = 2; // any non-zero byte is true
    vs[kSetupInt + 4 * 1 + 0] = 5;
    vs[kSetupInt + 4 * 1 + 3] = 8;
    // The uniform queue holds two of the four words of a float32 vector: 1.0 and 2.0.
    Set32(vs, kSetupQueue + 0, F(1.f));
    Set32(vs, kSetupQueue + 4, F(2.f));
    Set32(vs, kSetupQueue + 16, 2);
    // GS_SETUP: only a marker.
    Set32(init[InitBlock::GsSetup], 0, 4);
    Set32(init[InitBlock::GsSetup], kSetupProgram, 0x999);
    // DEFAULT_ATTRS: attribute 5.
    Set32(init[InitBlock::DefaultAttrs], 16 * 5 + 0, F(9.f));
    Set32(init[InitBlock::DefaultAttrs], 16 * 5 + 12, F(6.f));
    // LIGHTING_LUT: 24 tables of 256, table-major.
    Set32(init[InitBlock::LightingLut], 4 * (23 * 256 + 255), 0xABC);
    Set32(init[InitBlock::LightingLut], 0, 1);
    // FOG_LUT.
    Set32(init[InitBlock::FogLut], 4 * 127, 0x777);
    // PROCTEX: 896 words: noise 0-127, colour map 128-255, alpha map 256-383, colour 384-639,
    // colour difference 640-895.
    Bytes& pt = init[InitBlock::ProcTex];
    Set32(pt, 4 * 0, 1);
    Set32(pt, 4 * 128, 2);
    Set32(pt, 4 * 256, 3);
    Set32(pt, 4 * 384, 4);
    Set32(pt, 4 * 640, 5);
    Set32(pt, 4 * 895, 6);
    // IMMEDIATE: 16 vertices of 4 floats, current attribute, reset flag, queue (4 words + index).
    Bytes& im = init[InitBlock::Immediate];
    Set32(im, 16 * 3 + 4, F(1.5f));
    Set32(im, 256, 2);
    Set32(im, 260, 0);
    Set32(im, 264, 0x1234);
    Set32(im, 280, 1);
    // GEOMETRY_PIPELINE.
    Bytes& gp = init[InitBlock::GeometryPipeline];
    Set32(gp, 0, 3);
    Set32(gp, 4, 4);
    Set32(gp, 8, 5);
    Set32(gp, 12, 6);
    Set32(gp, 16, 7);
    Set32(gp, 20, 1);
    Set32(gp, 24, 8);
    Set32(gp, 28, 9);
    Set32(gp, 32 + 16 * 15 + 8, F(2.5f));

    Rig rig;
    rig.cp.LoadInitState(init.State());
    const PicaState& s = rig.cp.state();
    CHECK(s.regs[0x120] == 0xCAFE);
    CHECK(s.regs[0x034] == 0);
    CHECK(s.vs.entry_point == 9);
    CHECK(s.vs.biggest_program_size == 3);
    CHECK(s.vs.biggest_swizzle_size == 2);
    CHECK(s.vs.program_code[0] == 0x111);
    CHECK(s.vs.program_code[1] == 0);
    CHECK(s.vs.program_code[2] == 0x333);
    CHECK(s.vs.swizzle_data[1] == 0x444);
    CheckVec(s.vs.uniform_f[2], 1.f, 2.f, 3.f, 4.f);
    CHECK(s.vs.uniform_b[3]);
    CHECK(s.vs.uniform_b[4]);
    CHECK_FALSE(s.vs.uniform_b[5]);
    CHECK(s.vs.uniform_i[1] == std::array<u8, 4>{5, 0, 0, 8});
    CHECK(s.vs.uniform_queue.index == 2);
    CHECK(s.gs.entry_point == 4);
    CHECK(s.gs.program_code[0] == 0x999);
    CHECK(s.vs.program_code[0] != s.gs.program_code[0]);
    CheckVec(s.default_attributes[5], 9.f, 0.f, 0.f, 6.f);
    CHECK(s.lighting.luts[23][255] == 0xABC);
    CHECK(s.lighting.luts[0][0] == 1);
    CHECK(s.fog.lut[127] == 0x777);
    CHECK(s.proctex.noise_table[0] == 1);
    CHECK(s.proctex.color_map_table[0] == 2);
    CHECK(s.proctex.alpha_map_table[0] == 3);
    CHECK(s.proctex.color_table[0] == 4);
    CHECK(s.proctex.color_diff_table[0] == 5);
    CHECK(s.proctex.color_diff_table[255] == 6);
    CheckVec(s.immediate.input_vertex[3], 0, 1.5f, 0, 0);
    CHECK(s.immediate.current_attribute == 2);
    CHECK_FALSE(s.immediate.reset_geometry_pipeline);
    CHECK(s.immediate.queue.buffer[0] == 0x1234);
    CHECK(s.immediate.queue.index == 1);
    CHECK(s.geometry.mode == GeometryMode::FixedPrimitive);
    CHECK(s.geometry.vs_output_num == 4);
    CHECK(s.geometry.buffer_index == 5);
    CHECK(s.geometry.buffer_size == 6);
    CHECK(s.geometry.buffer_offset == 7);
    CHECK(s.geometry.need_index);
    CHECK(s.geometry.main_vertex_num == 8);
    CHECK(s.geometry.total_vertex_num == 9);
    CheckVec(s.geometry.attribute_buffer[15], 0, 0, 2.5f, 0);
    CHECK(s.dirty.Test(0x120));

    // Work continues from that state: the program offset is 7 and the biggest size 3 (-> 8); the
    // uniform setup is in float32 mode at index 3 with two of four words already queued, so two
    // more words complete the vector (1.0, 2.0, 3.0, 4.0): component 3 - i is word i.
    rig.Run(List()
                .Write(0x2CC, 0x77)
                .Write(0x2C1, F(3.f))
                .Write(0x2C1, F(4.f)));
    CHECK(s.vs.program_code[7] == 0x77);
    CHECK(s.vs.biggest_program_size == 8);
    CHECK(rig.Reg(0x2CB) == 8);
    CheckVec(s.vs.uniform_f[3], 4.f, 3.f, 2.f, 1.f);
    CHECK(rig.Reg(0x2C0) == 0x80000004);
    // The first draw sees the loaded tables as new.
    rig.Draw();
    CHECK(rig.LastDraw().state->lighting.lut_dirty == 0xFFFFFF);
}

TEST_CASE("LoadInitState ignores a block of the wrong size") {
    InitBlocks init;
    init[InitBlock::FogLut].assign(100, 0xFF);
    Set32(init[InitBlock::PicaRegs], 4 * 0x120, 5);
    Rig rig;
    rig.cp.LoadInitState(init.State());
    CHECK(rig.cp.state().fog.lut[0] == 0);
    CHECK(rig.cp.state().regs[0x120] == 5);
}

TEST_CASE("an INIT without blocks keeps the defaults") {
    Rig rig;
    rig.Run(List().Write(0x120, 7));
    rig.cp.LoadInitState(gpu::InitState{});
    CHECK(rig.Reg(0x120) == 0);
    CHECK(rig.Reg(0x034) == 1);
    CHECK(rig.Reg(0x289) == 0xA0000001);
}

// ================================================================================================
// --gpu pica-only
// ================================================================================================

namespace {

std::unique_ptr<gpu::Gpu> MakePicaOnly(gpu::Host& host) {
    const gpu::GpuFactory* factory = Registry<gpu::GpuFactory>::Instance().Find("pica-only");
    REQUIRE(factory != nullptr);
    const Options none = ParseOptions("capture-player", std::vector<std::string>{});
    return (*factory)(host, none);
}

} // namespace

TEST_CASE("pica-only decodes the command lists of GX commands and reports the register writes") {
    CaptureHost host;
    auto gpu = MakePicaOnly(host);
    auto* debug = dynamic_cast<GpuDebug*>(gpu.get());
    REQUIRE(debug != nullptr);

    // A list in the linear heap: virtual 0x14001000 is physical 0x20001000.
    List list;
    list.Write(0x120, 5).Write(0x121, 6, 0b0011).Multi(0x122, {7, 8, 9}, true);
    const std::vector<u32> words = list.Words();
    REQUIRE(host.WriteBoth(kFcram + 0x1000, reinterpret_cast<const u8*>(words.data()),
                           static_cast<u32>(words.size() * 4)));
    GxCommand command{};
    command.words[0] = 1; // SubmitCmdList
    command.words[1] = 0x14001000;
    command.words[2] = static_cast<u32>(words.size() * 4);
    gpu->Execute(command);

    const auto writes = debug->TakeRegisterWrites();
    REQUIRE(writes.has_value());
    CHECK(*writes == W({{0x120, 0xF, 5}, {0x121, 0x3, 6}, {0x122, 0xF, 7}, {0x123, 0xF, 8}, {0x124, 0xF, 9}}));
    CHECK(debug->TakeRegisterWrites()->empty()); // taken

    // The internal registers are at 0x1EF00000 + 4 * (0x432 + n).
    CHECK(gpu->ReadReg(0x1EF00000 + 4 * (0x432 + 0x120)) == 5);
    CHECK(gpu->ReadReg(0x1EF00000 + 4 * (0x432 + 0x23C)) == 0); // the trigger was cleared

    // Other GX commands are not its business.
    GxCommand fill{};
    fill.words[0] = 2;
    gpu->Execute(fill);
    CHECK(debug->TakeRegisterWrites()->empty());

    // The same list through the MMIO trigger: channel 0's registers, then the trigger.
    gpu->WriteReg(0x1EF00000 + 4 * (0x432 + 0x23A), (kFcram + 0x1000) >> 3);
    gpu->WriteReg(0x1EF00000 + 4 * (0x432 + 0x238), static_cast<u32>(words.size() * 4) >> 3);
    gpu->WriteReg(0x1EF00000 + 4 * (0x432 + 0x23C), 1);
    CHECK(debug->TakeRegisterWrites()->size() == 5);

    // The counters of the frame: one draw is none here; the command lists were two.
    const auto counters = debug->TakeFrameCounters();
    bool found = false;
    for (const auto& c : counters) {
        if (c.name == "pica_cmdlists") {
            CHECK(c.value == 2);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("pica-only starts from the INIT state") {
    CaptureHost host;
    auto gpu = MakePicaOnly(host);
    auto* debug = dynamic_cast<GpuDebug*>(gpu.get());
    REQUIRE(debug != nullptr);
    InitBlocks init;
    Set32(init[InitBlock::PicaRegs], 4 * 0x120, 0xCAFE);
    Set32(init[InitBlock::GpuRegs], 4 * 0x10, 0x1111);
    Set32(init[InitBlock::LcdRegs], 4 * 0x4, 0x2222);
    debug->LoadInitState(init.State());
    CHECK(gpu->ReadReg(0x1EF00000 + 4 * (0x432 + 0x120)) == 0xCAFE);
    CHECK(gpu->ReadReg(0x1EF00000 + 4 * 0x10) == 0x1111);
    CHECK(gpu->ReadReg(0x1ED02000 + 4 * 0x4) == 0x2222);
}

// ----- end to end through the capture player ------------------------------------------------------

namespace {

void Put16(Bytes& b, u32 v) {
    b.push_back(static_cast<u8>(v));
    b.push_back(static_cast<u8>(v >> 8));
}
void Put32(Bytes& b, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

// A capture with one command list and its PICA_REG record, written from scratch.
struct CaptureFile {
    Bytes out;
    u32 frame = 0;
    u64 ticks = 0;

    CaptureFile() {
        out = {'M', 'G', 'P', 'U'};
        Put32(out, 1);
    }
    void Record(u32 type, const Bytes& payload) {
        Put32(out, type);
        Put32(out, frame);
        Put32(out, static_cast<u32>(ticks += 1000));
        Put32(out, static_cast<u32>(ticks >> 32));
        Put32(out, static_cast<u32>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
    }
    void Init() {
        Bytes payload;
        Put32(payload, 11);
        for (u32 kind = 1; kind <= 11; ++kind) {
            const u32 size = gpu_capture::InitBlockSize(kind);
            Put32(payload, kind);
            Put32(payload, size);
            payload.resize(payload.size() + size, 0);
        }
        Record(gpu_capture::kInit, payload);
    }
    void Mem(u32 addr, const Bytes& data) {
        Bytes payload;
        Put32(payload, addr);
        Put32(payload, static_cast<u32>(data.size()));
        payload.insert(payload.end(), data.begin(), data.end());
        Record(gpu_capture::kMem, payload);
    }
    void SubmitCmdList(u32 vaddr, u32 size) {
        Bytes payload;
        for (u32 w : {1u, vaddr, size, 0u, 0u, 0u, 0u, 0u}) {
            Put32(payload, w);
        }
        Record(gpu_capture::kGx, payload);
    }
    void PicaReg(const std::vector<RegisterWrite>& writes) {
        Bytes payload;
        Put32(payload, static_cast<u32>(writes.size()));
        for (const RegisterWrite& w : writes) {
            Put16(payload, w.reg);
            payload.push_back(w.mask);
            payload.push_back(0);
            Put32(payload, w.value);
        }
        Record(gpu_capture::kPicaReg, payload);
    }
    void Frame(u32 n) {
        frame = n;
        Bytes payload;
        Put32(payload, n);
        Record(gpu_capture::kFrame, payload);
    }
    std::string Save(const std::string& name) const {
        const auto path = std::filesystem::temp_directory_path() /
                          ("mh4n_gpu_pica_test_" + std::to_string(MH4N_GETPID()) + "_" + name + ".mgpu");
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return path.string();
    }
};

gpu_capture::PlayerResult RunPlayerOn(const std::string& capture, std::vector<std::string> args) {
    args.push_back(capture);
    const Options options = ParseOptions("capture-player", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    gpu_capture::PlayerConfig config;
    std::string error;
    REQUIRE_MESSAGE(gpu_capture::ConfigFromOptions(options, config, &error), error);
    return gpu_capture::RunPlayer(config, options);
}

std::string CaptureWithList(const std::string& name, std::vector<RegisterWrite> expected) {
    List list;
    list.Write(0x120, 5).Write(0x121, 6, 0b0011).Multi(0x122, {7, 8, 9}, true);
    const std::vector<u32> words = list.Words();
    Bytes list_bytes(words.size() * 4);
    std::memcpy(list_bytes.data(), words.data(), list_bytes.size());

    CaptureFile capture;
    capture.Init();
    capture.Mem(kFcram + 0x1000, list_bytes);
    capture.SubmitCmdList(0x14001000, static_cast<u32>(list_bytes.size()));
    capture.PicaReg(expected);
    capture.Frame(1);
    return capture.Save(name);
}

} // namespace

TEST_CASE("the capture player checks the register writes of pica-only") {
    // The list above writes 0x120 = 5, 0x121 = 6 (bytes 0-1), then the consecutive form 0x122,
    // 0x123, 0x124 = 7, 8, 9: five PICA_REG entries.
    const std::vector<RegisterWrite> right = {
        {0x120, 0xF, 5}, {0x121, 0x3, 6}, {0x122, 0xF, 7}, {0x123, 0xF, 8}, {0x124, 0xF, 9}};
    const std::string good = CaptureWithList("good", right);
    auto result = RunPlayerOn(good, {"--gpu", "pica-only", "--isolated", "--judge", "none",
                                     "--check", "registers"});
    CHECK_MESSAGE(result.ExitStatus() == 0, gpu_capture::FormatReport(result));
    std::filesystem::remove(good);

    // A wrong value, a missing write and an extra one each fail at the command list's record.
    std::vector<RegisterWrite> wrong_value = right;
    wrong_value[3].value = 0x80;
    const std::string bad1 = CaptureWithList("badvalue", wrong_value);
    result = RunPlayerOn(bad1, {"--gpu", "pica-only", "--judge", "none", "--check", "registers"});
    CHECK(result.ExitStatus() == 1);
    CHECK(!result.failures.empty());
    std::filesystem::remove(bad1);

    std::vector<RegisterWrite> missing = right;
    missing.pop_back();
    const std::string bad2 = CaptureWithList("missing", missing);
    result = RunPlayerOn(bad2, {"--gpu", "pica-only", "--judge", "none", "--check", "registers"});
    CHECK(result.ExitStatus() == 1);
    std::filesystem::remove(bad2);
}
