// Derived from Azahar src/video_core/pica/pica_core.cpp, pica_core.h, shader_setup.cpp,
// geometry_pipeline.cpp, primitive_assembly.cpp (read to see what a draw hands over),
// vertex_loader.cpp (read to see where draws start) and src/video_core/gpu.cpp
// (GPU::Execute, GPU::WriteReg, GPU::SubmitCmdList) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of those files says "Licensed under GPLv2 or any later version"):
// the command-list loop and its decoding, every side effect of an internal register write, the
// immediate-mode vertex path, the draw triggers and the geometry-pipeline setup.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp,
//   pica_core.h, shader_setup.cpp, gpu.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (geometry_pipeline.cpp)
// Copyright 2014 Citra Emulator Project (primitive_assembly.cpp)
// Copyright 2023 Citra Emulator Project (vertex_loader.cpp)
#include "gpu/pica/command_processor.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "mh4n/log.h"

namespace mh4n::gpu::pica {

namespace {

constexpr const char* kLog = "PicaCore";

// The fields of a command header (pica_core.cpp, CommandHeader).
struct CommandHeader {
    u32 id;      // bits 0-15
    u32 mask;    // bits 16-19: the bytes of the register to write
    u32 count;   // bits 20-27: extra words that follow the header
    bool group;  // bit 31: the extra words go to consecutive registers

    explicit CommandHeader(u32 hex)
        : id(hex & 0xFFFF), mask((hex >> 16) & 0xF), count((hex >> 20) & 0xFF), group((hex >> 31) != 0) {}
};

bool AnyByteMatch(u32 a, u32 b) {
    return ((a & 0xFF) == (b & 0xFF)) || (((a >> 8) & 0xFF) == ((b >> 8) & 0xFF)) ||
           (((a >> 16) & 0xFF) == ((b >> 16) & 0xFF)) || (((a >> 24) & 0xFF) == ((b >> 24) & 0xFF));
}

u32 Load32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

float LoadFloat(const u8* p) {
    float v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace

CommandProcessor::CommandProcessor(gpu::Host& host, Backend& backend)
    : host_(host), backend_(backend), state_(std::make_unique<PicaState>()) {}

CommandProcessor::~CommandProcessor() = default;

// ---- INIT --------------------------------------------------------------------------------------

namespace {

// Sizes from mgpu-format.md section 4.
constexpr size_t kShaderSetupSize = 34368;
constexpr size_t kImmediateSize = 284;
constexpr size_t kGeometryPipelineSize = 288;

void LoadShaderSetup(ShaderSetup& setup, std::span<const u8> block) {
    const u8* p = block.data();
    setup.entry_point = Load32(p + 0);
    setup.biggest_program_size = std::min<u32>(Load32(p + 4), kMaxProgramWords);
    setup.biggest_swizzle_size = std::min<u32>(Load32(p + 8), kMaxSwizzleWords);
    std::memcpy(setup.program_code.data(), p + 12, kMaxProgramWords * sizeof(u32));
    std::memcpy(setup.swizzle_data.data(), p + 16396, kMaxSwizzleWords * sizeof(u32));
    for (u32 i = 0; i < kNumFloatUniforms; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            setup.uniform_f[i].c[c] = LoadFloat(p + 32780 + 16 * i + 4 * c);
        }
    }
    for (u32 i = 0; i < 16; ++i) {
        setup.uniform_b[i] = p[34316 + i] != 0;
    }
    for (u32 i = 0; i < 4; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            setup.uniform_i[i][c] = p[34332 + 4 * i + c];
        }
    }
    for (u32 i = 0; i < 4; ++i) {
        setup.uniform_queue.buffer[i] = Load32(p + 34348 + 4 * i);
    }
    setup.uniform_queue.index = std::min<u32>(Load32(p + 34364), 3);
}

void LoadVec4Array(std::array<Vec4f, 16>& out, const u8* p) {
    for (u32 i = 0; i < 16; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            out[i].c[c] = LoadFloat(p + 16 * i + 4 * c);
        }
    }
}

} // namespace

void CommandProcessor::LoadInitState(const gpu::InitState& init) {
    *state_ = PicaState();
    delay_.Reset();
    list_ = {};
    writes_.clear();

    // `block` is the block of that kind if it has the size the format gives it.
    const auto block = [&](gpu::InitBlock kind, size_t size, const char* name) {
        const std::span<const u8> data = init.Block(kind);
        if (data.size() != size) {
            if (!data.empty()) {
                MH4N_LOG(Error, kLog, "INIT block {} has {} bytes, expected {}; ignored", name,
                         data.size(), size);
            }
            return std::span<const u8>{};
        }
        return data;
    };

    PicaState& s = *state_;

    if (auto b = block(gpu::InitBlock::PicaRegs, kNumRegs * 4, "PICA_REGS"); !b.empty()) {
        for (u32 i = 0; i < kNumRegs; ++i) {
            s.regs[i] = Load32(b.data() + 4 * i);
        }
    }
    if (auto b = block(gpu::InitBlock::VsSetup, kShaderSetupSize, "VS_SETUP"); !b.empty()) {
        LoadShaderSetup(s.vs, b);
    }
    if (auto b = block(gpu::InitBlock::GsSetup, kShaderSetupSize, "GS_SETUP"); !b.empty()) {
        LoadShaderSetup(s.gs, b);
    }
    if (auto b = block(gpu::InitBlock::DefaultAttrs, 256, "DEFAULT_ATTRS"); !b.empty()) {
        LoadVec4Array(s.default_attributes, b.data());
    }
    if (auto b = block(gpu::InitBlock::LightingLut, Lighting::kTables * Lighting::kEntries * 4,
                       "LIGHTING_LUT");
        !b.empty()) {
        for (u32 t = 0; t < Lighting::kTables; ++t) {
            for (u32 i = 0; i < Lighting::kEntries; ++i) {
                s.lighting.luts[t][i] = Load32(b.data() + 4 * (t * Lighting::kEntries + i));
            }
        }
    }
    if (auto b = block(gpu::InitBlock::FogLut, Fog::kEntries * 4, "FOG_LUT"); !b.empty()) {
        for (u32 i = 0; i < Fog::kEntries; ++i) {
            s.fog.lut[i] = Load32(b.data() + 4 * i);
        }
    }
    if (auto b = block(gpu::InitBlock::ProcTex, 896 * 4, "PROCTEX"); !b.empty()) {
        // noise (128), color_map (128), alpha_map (128), color (256), color_diff (256)
        const u8* p = b.data();
        const auto fill = [&p](auto& table) {
            for (auto& entry : table) {
                entry = Load32(p);
                p += 4;
            }
        };
        fill(s.proctex.noise_table);
        fill(s.proctex.color_map_table);
        fill(s.proctex.alpha_map_table);
        fill(s.proctex.color_table);
        fill(s.proctex.color_diff_table);
    }
    if (auto b = block(gpu::InitBlock::Immediate, kImmediateSize, "IMMEDIATE"); !b.empty()) {
        LoadVec4Array(s.immediate.input_vertex, b.data());
        s.immediate.current_attribute = std::min<u32>(Load32(b.data() + 256), 15);
        s.immediate.reset_geometry_pipeline = Load32(b.data() + 260) != 0;
        for (u32 i = 0; i < 4; ++i) {
            s.immediate.queue.buffer[i] = Load32(b.data() + 264 + 4 * i);
        }
        s.immediate.queue.index = std::min<u32>(Load32(b.data() + 280), 3);
    }
    if (auto b = block(gpu::InitBlock::GeometryPipeline, kGeometryPipelineSize,
                       "GEOMETRY_PIPELINE");
        !b.empty()) {
        const u32 mode = Load32(b.data());
        s.geometry.mode = mode <= 3 ? static_cast<GeometryMode>(mode) : GeometryMode::None;
        s.geometry.vs_output_num = Load32(b.data() + 4);
        s.geometry.buffer_index = Load32(b.data() + 8);
        s.geometry.buffer_size = Load32(b.data() + 12);
        s.geometry.buffer_offset = Load32(b.data() + 16);
        s.geometry.need_index = Load32(b.data() + 20) != 0;
        s.geometry.main_vertex_num = Load32(b.data() + 24);
        s.geometry.total_vertex_num = Load32(b.data() + 28);
        LoadVec4Array(s.geometry.attribute_buffer, b.data() + 32);
    }

    s.dirty.SetAll();
}

// ---- command lists -----------------------------------------------------------------------------

void CommandProcessor::ResetList(u32 paddr, u32 size) {
    list_.paddr = paddr;
    list_.length = size / sizeof(u32);
    list_.index = 0;
    list_.head = size == 0 ? nullptr : host_.PhysicalPointer(paddr, size);
    if (list_.head == nullptr && size != 0 && host_.PhysicalPointer(paddr, 4) == nullptr) {
        // Azahar would read through a null pointer here.
        MH4N_LOG(Error, kLog, "command list at {:#010x} ({} bytes) is not in guest memory; skipped",
                 paddr, size);
        list_.length = 0;
    }
}

u32 CommandProcessor::ListWord(u32 index) const {
    if (list_.head != nullptr && index < list_.length) {
        return Load32(list_.head + 4 * static_cast<size_t>(index));
    }
    const u8* p = host_.PhysicalPointer(list_.paddr + 4 * index, 4);
    return p != nullptr ? Load32(p) : 0;
}

void CommandProcessor::ProcessCmdList(u32 paddr, u32 size) {
    ++lists_;
    ResetList(paddr, size);

    bool stop_requested = false;
    while (list_.index < list_.length) {
        if (stop_requested) {
            break;
        }
        // Commands start on a multiple of 8 bytes.
        if (list_.index % 2 != 0) {
            list_.index++;
        }

        // The value to write comes first, then the header.
        const u32 value = ListWord(list_.index++);
        const CommandHeader header(ListWord(list_.index++));

        WriteInternalReg(header.id, value, header.mask, stop_requested);

        // Any extra parameters. They are read from the list as it is after the first write: when
        // that write was a jump, they come from the new list (as in Azahar), and a jump written
        // among them does not change which words are left to write.
        const u32 count = header.count;
        if (count == 0) {
            continue;
        }
        std::array<u32, 255> extra;
        for (u32 i = 0; i < count; ++i) {
            extra[i] = ListWord(list_.index + i);
        }
        list_.index += count;

        const std::span<const u32> values(extra.data(), count);
        if (!header.group) {
            // The same register written count more times (program and swizzle uploads, tables).
            WriteInternalRegBatch(header.id, values, header.mask, stop_requested);
        } else {
            // The registers from id + 1 on.
            WriteInternalRegSequential(header.id + 1, values, header.mask, stop_requested);
        }
    }
}

void CommandProcessor::SubmitCmdList(u32 index) {
    PicaState& s = *state_;
    if (s.regs[reg::kCommandBufferTrigger + index] == 0) {
        return;
    }
    const u32 addr = 8 * BitsOf(s.regs[reg::kCommandBufferAddress + index], 0, 28);
    const u32 size = 8 * BitsOf(s.regs[reg::kCommandBufferSize + index], 0, 20);
    ProcessCmdList(addr, size);
    s.regs[reg::kCommandBufferTrigger + index] = 0;
}

void CommandProcessor::SubmitGxCmdList(u32 paddr, u32 size) {
    PicaState& s = *state_;
    u32& addr = s.regs[reg::kCommandBufferAddress];
    u32& sz = s.regs[reg::kCommandBufferSize];
    addr = WithBits(addr, 0, 28, paddr >> 3);
    sz = WithBits(sz, 0, 20, size >> 3);
    s.regs[reg::kCommandBufferTrigger] = 1;
    SubmitCmdList(0);
}

void CommandProcessor::WriteHardwareRegister(u32 id, u32 value) {
    if (id >= kNumRegs) {
        return;
    }
    state_->regs[id] = value;
    state_->dirty.Set(id);
    if (id == reg::kCommandBufferTrigger) {
        SubmitCmdList(0);
    } else if (id == reg::kCommandBufferTrigger + 1) {
        SubmitCmdList(1);
    }
}

void CommandProcessor::SetRegisterWriteLog(bool enabled) {
    log_writes_ = enabled;
    if (!enabled) {
        writes_.clear();
    }
}

std::vector<gpu::RegisterWrite> CommandProcessor::TakeRegisterWrites() {
    std::vector<gpu::RegisterWrite> taken;
    taken.swap(writes_);
    return taken;
}

void CommandProcessor::LogWrite(u32 id, u32 mask, u32 value) {
    if (log_writes_) {
        writes_.push_back({static_cast<u16>(id), static_cast<u8>(mask), value});
    }
}

// ---- register writes ---------------------------------------------------------------------------

void CommandProcessor::WriteInternalReg(u32 id, u32 value, u32 mask, bool& stop_requested) {
    if (id >= kNumRegs) {
        // Writes to registers that do not exist are ignored.
        return;
    }
    PicaState& s = *state_;

    delay_.AddCommands(1);

    // Azahar: "Figure out how register masking acts on e.g. vs.uniform_setup.set_value". The
    // register gets the masked value; the handlers below get the whole word.
    const u32 write_mask = ExpandMask(mask);
    s.regs[id] = (s.regs[id] & ~write_mask) | (value & write_mask);
    LogWrite(id, mask, value);

    if (IsSpecialRegister(id)) {
        HandleSpecialReg(id, value, stop_requested);
    }
    s.dirty.Set(id);
}

void CommandProcessor::WriteInternalRegBatch(u32 id, std::span<const u32> values, u32 mask,
                                             bool& stop_requested) {
    if (id >= kNumRegs) {
        return;
    }
    PicaState& s = *state_;

    delay_.AddCommands(values.size());
    const u32 write_mask = ExpandMask(mask);
    // Only the last value reaches the register array; the batch handlers know.
    s.regs[id] = (s.regs[id] & ~write_mask) | (values.back() & write_mask);
    for (const u32 value : values) {
        LogWrite(id, mask, value);
    }
    s.dirty.Set(id);

    if (IsBatchRegister(id)) {
        HandleSpecialRegBatch(id, values);
    } else if (IsSpecialRegister(id)) {
        for (size_t i = 0; i < values.size() && !stop_requested; ++i) {
            HandleSpecialReg(id, values[i], stop_requested);
        }
    }
}

void CommandProcessor::WriteInternalRegSequential(u32 id, std::span<const u32> values, u32 mask,
                                                  bool& stop_requested) {
    u32 count = static_cast<u32>(values.size());
    if (id + count > kNumRegs) {
        // The part that is past the last register is ignored.
        if (id >= kNumRegs) {
            return;
        }
        count = kNumRegs - id;
    }
    // Azahar copies the runs of ordinary registers in one go and handles each special register
    // on its own; the result is that of writing the registers one by one.
    for (u32 i = 0; i < count && !stop_requested; ++i) {
        WriteInternalReg(id + i, values[i], mask, stop_requested);
    }
}

bool CommandProcessor::GsMirrorsVs() const {
    // Unit 3 shares the vertex shader's configuration unless it is configured on its own or the
    // geometry shader is in use.
    const auto& regs = state_->regs;
    return BitsOf(regs[reg::kGsUnitExclusiveConfig], 0, 1) == 0 &&
           BitsOf(regs[reg::kGeometryStage], 0, 2) == kUseGsNo;
}

void CommandProcessor::HandleSpecialReg(u32 id, u32 value, bool& stop_requested) {
    PicaState& s = *state_;
    auto& regs = s.regs;

    switch (id) {
    case reg::kIrqRequest:
        // Azahar: "This logic is not fully accurate, but close enough".
        if (AnyByteMatch(regs[id], regs[reg::kIrqCompare])) {
            host_.SignalInterrupt(gpu::Interrupt::P3D, delay_.CalculateAndResetDelay());
            if (regs[reg::kIrqAutostop] != 0) {
                stop_requested = true;
            }
        }
        return;

    case reg::kTriangleTopology:
    case reg::kRestartPrimitive:
        // Azahar reconfigures or resets its software primitive assembler here. A draw call
        // carries no assembler state: the topology is in the registers, and every draw starts a
        // new primitive sequence (what Azahar's hardware renderers do).
        return;

    case reg::kFixedAttribIndex:
        s.immediate.Reset();
        return;

    case reg::kTriggerDraw:
    case reg::kTriggerDrawIndexed:
        DrawArrays(id == reg::kTriggerDrawIndexed);
        return;

    case reg::kCommandBufferTrigger:
    case reg::kCommandBufferTrigger + 1: {
        // A jump: the rest of the current list is not executed.
        const u32 index = id - reg::kCommandBufferTrigger;
        const u32 addr = 8 * BitsOf(regs[reg::kCommandBufferAddress + index], 0, 28);
        const u32 size = 8 * BitsOf(regs[reg::kCommandBufferSize + index], 0, 20);
        ResetList(addr, size);
        return;
    }
    default:
        break;
    }

    if (InRange(id, reg::kFixedAttribData, 3)) {
        SubmitImmediate(value);
        return;
    }

    if (InRange(id, reg::kLightingLutData, 8)) {
        u32& config = regs[reg::kLightingLutConfig];
        const u32 type = BitsOf(config, 8, 5);
        const u32 index = BitsOf(config, 0, 8);
        if (type < Lighting::kTables) {
            const u32 prev = std::exchange(s.lighting.luts[type][index], value);
            s.lighting.lut_dirty |= static_cast<u32>(prev != value) << type;
        } else {
            // Azahar indexes outside its table array here.
            MH4N_LOG(Error, kLog, "lighting LUT type {} is out of range", type);
        }
        config = WithBits(config, 0, 8, index + 1);
        return;
    }

    if (InRange(id, reg::kFogLutData, 8)) {
        u32& offset_reg = regs[reg::kFogLutOffset];
        const u32 offset = BitsOf(offset_reg, 0, 16);
        const u32 prev = std::exchange(s.fog.lut[offset % Fog::kEntries], value);
        s.fog.lut_dirty |= prev != value;
        offset_reg = WithBits(offset_reg, 0, 16, offset + 1);
        return;
    }

    if (InRange(id, reg::kProcTexLutData, 8)) {
        u32& config = regs[reg::kProcTexLutConfig];
        const u32 index = BitsOf(config, 0, 8);
        const u32 table = BitsOf(config, 8, 4);
        const auto sync = [&](auto& entries) {
            const u32 prev = std::exchange(entries[index % entries.size()], value);
            s.proctex.table_dirty |= static_cast<u8>(static_cast<u32>(prev != value) << table);
        };
        switch (table) {
        case kProcTexNoise:
            sync(s.proctex.noise_table);
            break;
        case kProcTexColorMap:
            sync(s.proctex.color_map_table);
            break;
        case kProcTexAlphaMap:
            sync(s.proctex.alpha_map_table);
            break;
        case kProcTexColor:
            sync(s.proctex.color_table);
            break;
        case kProcTexColorDiff:
            sync(s.proctex.color_diff_table);
            break;
        default:
            break; // not a table: the value goes nowhere, the index still advances
        }
        config = WithBits(config, 0, 8, index + 1);
        return;
    }

    // The two shader units.
    const bool is_gs = InRange(id, reg::kGs, 0x30);
    const bool is_vs = InRange(id, reg::kVs, 0x30);
    if (!is_gs && !is_vs) {
        return;
    }
    const u32 base = is_vs ? reg::kVs : reg::kGs;
    const u32 rel = id - base;
    ShaderSetup& unit = is_vs ? s.vs : s.gs;
    const bool mirror = is_vs && GsMirrorsVs();

    if (rel == reg::kShaderBoolUniforms) {
        const u32 bits = BitsOf(regs[id], 0, 16);
        unit.WriteUniformBoolReg(bits);
        if (mirror) {
            s.gs.WriteUniformBoolReg(bits);
        }
    } else if (InRange(rel, reg::kShaderIntUniforms, 4)) {
        const u32 index = rel - reg::kShaderIntUniforms;
        const u32 word = regs[id];
        const std::array<u8, 4> bytes = {static_cast<u8>(word), static_cast<u8>(word >> 8),
                                         static_cast<u8>(word >> 16), static_cast<u8>(word >> 24)};
        unit.WriteUniformIntReg(index, bytes);
        if (mirror) {
            s.gs.WriteUniformIntReg(index, bytes);
        }
    } else if (rel == reg::kShaderOutputMask) {
        // Only the vertex shader's is special.
        if (mirror) {
            u32& gs_mask = regs[reg::kGs + reg::kShaderOutputMask];
            gs_mask = WithBits(gs_mask, 0, 16, value);
        }
    } else if (InRange(rel, reg::kShaderUniformData, 8)) {
        const auto index = unit.WriteUniformFloatReg(regs[base + reg::kShaderUniformIndex], value);
        if (mirror && index) {
            s.gs.uniform_f[*index] = s.vs.uniform_f[*index];
        }
    } else if (InRange(rel, reg::kShaderProgramData, 8)) {
        u32& offset = regs[base + reg::kShaderProgramOffset];
        const u32 limit = is_vs ? 512 : kMaxProgramWords;
        if (offset >= limit) {
            MH4N_LOG(Error, kLog, "invalid {} program offset {}", is_vs ? "VS" : "GS", offset);
        } else {
            unit.UpdateProgramCode(offset, value);
            if (mirror) {
                s.gs.UpdateProgramCode(offset, value);
            }
            offset++;
        }
    } else if (InRange(rel, reg::kShaderSwizzleData, 8)) {
        u32& offset = regs[base + reg::kShaderSwizzleOffset];
        if (offset >= kMaxSwizzleWords) {
            MH4N_LOG(Error, kLog, "invalid {} swizzle pattern offset {}", is_vs ? "VS" : "GS",
                     offset);
        } else {
            unit.UpdateSwizzleData(offset, value);
            if (mirror) {
                s.gs.UpdateSwizzleData(offset, value);
            }
            offset++;
        }
    }
}

void CommandProcessor::HandleSpecialRegBatch(u32 id, std::span<const u32> values) {
    PicaState& s = *state_;
    auto& regs = s.regs;
    const u32 count = static_cast<u32>(values.size());

    if (InRange(id, reg::kFixedAttribData, 3)) {
        for (const u32 value : values) {
            SubmitImmediate(value);
        }
        return;
    }

    if (InRange(id, reg::kLightingLutData, 8)) {
        u32& config = regs[reg::kLightingLutConfig];
        const u32 type = BitsOf(config, 8, 5);
        const u32 index = BitsOf(config, 0, 8);
        if (type < Lighting::kTables) {
            auto& table = s.lighting.luts[type];
            for (u32 i = 0; i < count; ++i) {
                const u32 prev = std::exchange(table[(index + i) % table.size()], values[i]);
                s.lighting.lut_dirty |= static_cast<u32>(prev != values[i]) << type;
            }
        } else {
            MH4N_LOG(Error, kLog, "lighting LUT type {} is out of range", type);
        }
        config = WithBits(config, 0, 8, index + count);
        return;
    }

    if (InRange(id, reg::kFogLutData, 8)) {
        u32& offset_reg = regs[reg::kFogLutOffset];
        const u32 offset = BitsOf(offset_reg, 0, 16);
        for (u32 i = 0; i < count; ++i) {
            const u32 prev = std::exchange(s.fog.lut[(offset + i) % Fog::kEntries], values[i]);
            s.fog.lut_dirty |= prev != values[i];
        }
        offset_reg = WithBits(offset_reg, 0, 16, offset + count);
        return;
    }

    if (InRange(id, reg::kProcTexLutData, 8)) {
        u32& config = regs[reg::kProcTexLutConfig];
        const u32 index = BitsOf(config, 0, 8);
        const u32 table = BitsOf(config, 8, 4);
        const auto sync = [&](auto& entries) {
            for (u32 i = 0; i < count; ++i) {
                const u32 prev = std::exchange(entries[(index + i) % entries.size()], values[i]);
                s.proctex.table_dirty |= static_cast<u8>(static_cast<u32>(prev != values[i]) << table);
            }
        };
        switch (table) {
        case kProcTexNoise:
            sync(s.proctex.noise_table);
            break;
        case kProcTexColorMap:
            sync(s.proctex.color_map_table);
            break;
        case kProcTexAlphaMap:
            sync(s.proctex.alpha_map_table);
            break;
        case kProcTexColor:
            sync(s.proctex.color_table);
            break;
        case kProcTexColorDiff:
            sync(s.proctex.color_diff_table);
            break;
        default:
            break;
        }
        config = WithBits(config, 0, 8, index + count);
        return;
    }

    const bool is_gs = InRange(id, reg::kGs, 0x30);
    const bool is_vs = InRange(id, reg::kVs, 0x30);
    if (!is_gs && !is_vs) {
        return;
    }
    const u32 base = is_vs ? reg::kVs : reg::kGs;
    const u32 rel = id - base;
    ShaderSetup& unit = is_vs ? s.vs : s.gs;
    const bool mirror = is_vs && GsMirrorsVs();

    if (InRange(rel, reg::kShaderUniformData, 8)) {
        const auto range = unit.WriteUniformFloatRegRange(regs[base + reg::kShaderUniformIndex], values);
        if (mirror && range) {
            for (u32 i = 0; i < range->count; ++i) {
                const u32 idx = range->first_index + i;
                s.gs.uniform_f[idx] = s.vs.uniform_f[idx];
            }
        }
    } else if (InRange(rel, reg::kShaderProgramData, 8)) {
        u32& offset = regs[base + reg::kShaderProgramOffset];
        const u32 limit = is_vs ? 512 : kMaxProgramWords;
        if (offset + count > limit) {
            // The whole batch is refused.
            MH4N_LOG(Error, kLog, "invalid {} program offset {} count {}", is_vs ? "VS" : "GS",
                     offset, count);
        } else {
            unit.UpdateProgramCodeRange(offset, values);
            if (mirror) {
                s.gs.UpdateProgramCodeRange(offset, values);
            }
            offset += count;
        }
    } else if (InRange(rel, reg::kShaderSwizzleData, 8)) {
        u32& offset = regs[base + reg::kShaderSwizzleOffset];
        if (offset + count > kMaxSwizzleWords) {
            MH4N_LOG(Error, kLog, "invalid {} swizzle pattern offset {} count {}",
                     is_vs ? "VS" : "GS", offset, count);
        } else {
            unit.UpdateSwizzleDataRange(offset, values);
            if (mirror) {
                s.gs.UpdateSwizzleDataRange(offset, values);
            }
            offset += count;
        }
    }
}

// ---- vertices and draws ------------------------------------------------------------------------

void CommandProcessor::SubmitImmediate(u32 word) {
    PicaState& s = *state_;
    auto& regs = s.regs;

    // The queue says when a full attribute has been formed.
    if (!s.immediate.queue.Push(word)) {
        return;
    }

    constexpr u32 kImmediateModeIndex = 0xF;

    const u32 setup_index = regs[reg::kFixedAttribIndex];
    if (setup_index > kImmediateModeIndex) {
        MH4N_LOG(Error, kLog, "invalid VS default attribute index {}", setup_index);
        // Azahar leaves the queue full here, so its next word would land outside the queue.
        s.immediate.queue.Reset();
        return;
    }

    const Vec4f attribute = s.immediate.queue.Get();
    if (setup_index < kImmediateModeIndex) {
        s.default_attributes[setup_index] = attribute;
        regs[reg::kFixedAttribIndex] = setup_index + 1;
        return;
    }

    // With the index at 0xF the attribute is part of a vertex to draw at once.
    s.immediate.input_vertex[s.immediate.current_attribute] = attribute;
    if (s.immediate.current_attribute < BitsOf(regs[reg::kMaxInputAttribIndex], 0, 4)) {
        s.immediate.current_attribute++;
        return;
    }

    // A whole vertex has been formed.
    DrawImmediate();
}

void CommandProcessor::DrawImmediate() {
    PicaState& s = *state_;

    DrawCall call;
    call.kind = DrawKind::Immediate;
    call.vertex_base_address = 0;
    call.num_vertices = 1;
    if (s.immediate.reset_geometry_pipeline) {
        ReconfigureGeometryPipeline();
        s.immediate.reset_geometry_pipeline = false;
        call.geometry_reconfigured = true;
    }
    IssueDraw(call);
    s.immediate.current_attribute = 0;
}

void CommandProcessor::DrawArrays(bool is_indexed) {
    PicaState& s = *state_;
    const auto& regs = s.regs;

    delay_.AddVertices(regs[reg::kNumVertices], BitsOf(regs[reg::kTriangleTopology], 8, 2));

    DrawCall call;
    call.kind = is_indexed ? DrawKind::Indexed : DrawKind::Arrays;
    call.vertex_base_address = 16 * BitsOf(regs[reg::kVertexAttributesBase], 1, 28);
    call.num_vertices = regs[reg::kNumVertices];
    if (is_indexed) {
        call.index_offset = BitsOf(regs[reg::kIndexArray], 0, 28);
        call.index_u16 = BitsOf(regs[reg::kIndexArray], 31, 1) != 0;
    } else {
        call.vertex_offset = regs[reg::kVertexOffset];
    }
    // PicaCore::LoadVertices reconfigures the geometry pipeline at every draw.
    ReconfigureGeometryPipeline();
    call.geometry_reconfigured = true;
    IssueDraw(call);
}

void CommandProcessor::ReconfigureGeometryPipeline() {
    PicaState& s = *state_;
    const auto& regs = s.regs;
    GeometryPipelineState& g = s.geometry;
    g = GeometryPipelineState();

    if (BitsOf(regs[reg::kGeometryStage], 0, 2) == kUseGsNo) {
        return;
    }

    // GeometryPipeline::Reconfigure asserts what it relies on; here a mismatch is only logged.
    const u32 gs_mode_reg = regs[reg::kGsConfig];
    const u32 shader_mode = BitsOf(regs[reg::kGs + reg::kShaderInputBufferConfig], 24, 8);
    if (shader_mode != kShaderModeGs) {
        MH4N_LOG(Warning, kLog, "geometry shader in use but unit 3 has shader mode {:#x}",
                 shader_mode);
    }
    if (BitsOf(regs[reg::kVsOutmapTotalA], 0, 4) != BitsOf(regs[reg::kVsOutmapTotalB], 0, 4)) {
        MH4N_LOG(Warning, kLog, "the two vertex shader output counts differ");
    }
    const u32 vs_output_num = BitsOf(regs[reg::kVsOutmapTotalA], 0, 4) + 1;
    const bool variable = BitsOf(regs[reg::kGeometryStage], 31, 1) != 0;
    const bool to_uniform = BitsOf(regs[reg::kGs + reg::kShaderInputBufferConfig], 8, 8) != 0;

    switch (BitsOf(gs_mode_reg, 0, 8)) {
    case kGsModePoint: {
        // Vertex outputs fill the geometry shader's input registers; the shader runs when they
        // are full.
        const u32 gs_input_num = BitsOf(regs[reg::kGs + reg::kShaderInputBufferConfig], 0, 4) + 1;
        if (variable || to_uniform || gs_input_num % vs_output_num != 0) {
            MH4N_LOG(Warning, kLog, "inconsistent geometry pipeline (point mode)");
        }
        g.mode = GeometryMode::Point;
        g.vs_output_num = vs_output_num;
        g.buffer_index = 0;
        g.buffer_size = gs_input_num;
        break;
    }
    case kGsModeVariablePrimitive:
        // Vertex outputs go to the geometry shader's float uniforms, the vertex count first.
        if (!variable || !to_uniform) {
            MH4N_LOG(Warning, kLog, "inconsistent geometry pipeline (variable primitive mode)");
        }
        g.mode = GeometryMode::VariablePrimitive;
        g.vs_output_num = vs_output_num;
        g.need_index = true;
        break;
    case kGsModeFixedPrimitive: {
        // A fixed number of vertices per invocation, buffered in the float uniforms from
        // start_index on.
        const u32 vertex_num = BitsOf(gs_mode_reg, 8, 4) + 1;
        const u32 start_index = BitsOf(gs_mode_reg, 16, 8);
        if (variable || !to_uniform || vs_output_num != BitsOf(gs_mode_reg, 12, 4) + 1) {
            MH4N_LOG(Warning, kLog, "inconsistent geometry pipeline (fixed primitive mode)");
        }
        g.mode = GeometryMode::FixedPrimitive;
        g.vs_output_num = vs_output_num;
        g.buffer_offset = start_index;
        g.buffer_index = start_index;
        g.buffer_size = start_index + vs_output_num * vertex_num;
        break;
    }
    default:
        // Azahar treats this as unreachable.
        MH4N_LOG(Error, kLog, "geometry shader mode {} does not exist; geometry shader ignored",
                 BitsOf(gs_mode_reg, 0, 8));
        break;
    }
}

void CommandProcessor::IssueDraw(DrawCall& call) {
    PicaState& s = *state_;
    call.sequence = sequence_++;

    // ShaderEngine::SetupBatch gives the shader its entry point.
    s.vs.entry_point = BitsOf(s.regs[reg::kVs + reg::kShaderMainOffset], 0, 16);
    if (s.geometry.mode != GeometryMode::None) {
        s.gs.entry_point = BitsOf(s.regs[reg::kGs + reg::kShaderMainOffset], 0, 16);
    }

    call.state = state_.get();
    backend_.Draw(call);
    s.ClearDirty();
}

} // namespace mh4n::gpu::pica
