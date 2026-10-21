// Derived from Azahar src/core/hle/service/cam/y2r_u.cpp and src/core/hle/service/cam/y2r_u.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the command table of y2r:u with its session limit, the standard coefficient sets, the
// checks and result codes of the line width, line count and coefficient commands, the replies of
// every command, the 50000-tick completion delay and the defaults DriverInitialize sets.
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (y2r_u.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (y2r_u.h)
//
// y2r:u (N1-19): the YUV to RGB converter. StartConversion converts the image at once (see
// y2r_convert.h) and signals the transfer-end event 50000 ticks later on the timer queue (Azahar's
// MinY2RDelay); IsBusyConversion is true until then, StopConversion cancels the event. The
// IsFinished* commands answer 1, as in Azahar. The state is shared by the sessions (the service
// allows one).
//
// Departures from Azahar: the source and destination of a conversion are read and written through the
// application's memory (the process handle the game passes is only logged, as in Azahar); the
// destination range is flushed from and invalidated in the GPU's caches before the conversion, as
// Azahar's StartConversion does through the rasterizer; GetPackageParameter returns the first 12 bytes
// of the configuration (three words after the result), which is what Azahar's four-word reply holds;
// the conversion itself departs as y2r_convert.h lists (a partial last output unit is padded with zero
// bytes instead of read past the pixel buffer, and the output is written at the descriptor's address,
// not at a host pointer that drifts from it).
#include <algorithm>
#include <cstring>
#include <memory>

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"
#include "y2r_convert.h"

namespace mh4n::y2r {

namespace {

constexpr const char* kLog = "Service.Y2R";

constexpr u32 kModuleCam = 20;
constexpr Result kResultOutOfRange = MakeResult(1021, kModuleCam, 7, 28);       // 0xE0E053FD
constexpr Result kResultInvalidEnumValue = MakeResult(1005, kModuleCam, 7, 28);  // 0xE0E053ED

constexpr std::array<CoefficientSet, 4> kStandardCoefficients = {{
    {{0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B}},  // ITU_Rec601
    {{0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51}},   // ITU_Rec709
    {{0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B}},  // ITU_Rec601_Scaling
    {{0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421}},   // ITU_Rec709_Scaling
}};

constexpr s64 kMinY2rDelayTicks = 50000;

struct Y2rState {
    ConversionConfiguration conversion;
    std::array<u16, 16> dithering_weight_params{};
    bool temporal_dithering_enabled = false;
    bool transfer_end_interrupt_enabled = false;
    bool spacial_dithering_enabled = false;
    bool is_busy_conversion = false;
    u64 completion_timer = 0;
    std::shared_ptr<Event> completion_event;
    bool created = false;

    Result SetInputLineWidth(u16 width) {
        if (width == 0 || width > 1024 || width % 8 != 0) return kResultOutOfRange;
        conversion.input_line_width = width;
        return kResultSuccess;
    }
    Result SetInputLines(u16 lines) {
        if (lines == 0 || lines > 1024) return kResultOutOfRange;
        // In what appears to be a bug the camera module does not set the register for 1024 lines, so
        // the conversion uses the last value that was set.
        if (lines != 1024) conversion.input_lines = lines;
        return kResultSuccess;
    }
    Result SetStandardCoefficient(u8 index) {
        if (index >= kStandardCoefficients.size()) return kResultInvalidEnumValue;
        conversion.coefficients = kStandardCoefficients[index];
        return kResultSuccess;
    }
};

class Y2rU final : public Service {
public:
    explicit Y2rU(ServiceContext& context) : Service(context, "y2r:u"), st_(context.State<Y2rState>("y2r:u")) {
        if (!st_->created) {
            st_->created = true;
            st_->completion_event = context.GetKernel().NewEvent(ResetType::OneShot, "Y2R:Completed");
        }
        Register(0x0001, "SetInputFormat", &Y2rU::SetInputFormat);
        Register(0x0002, "GetInputFormat", &Y2rU::GetInputFormat);
        Register(0x0003, "SetOutputFormat", &Y2rU::SetOutputFormat);
        Register(0x0004, "GetOutputFormat", &Y2rU::GetOutputFormat);
        Register(0x0005, "SetRotation", &Y2rU::SetRotation);
        Register(0x0006, "GetRotation", &Y2rU::GetRotation);
        Register(0x0007, "SetBlockAlignment", &Y2rU::SetBlockAlignment);
        Register(0x0008, "GetBlockAlignment", &Y2rU::GetBlockAlignment);
        Register(0x0009, "SetSpacialDithering", &Y2rU::SetSpacialDithering);
        Register(0x000A, "GetSpacialDithering", &Y2rU::GetSpacialDithering);
        Register(0x000B, "SetTemporalDithering", &Y2rU::SetTemporalDithering);
        Register(0x000C, "GetTemporalDithering", &Y2rU::GetTemporalDithering);
        Register(0x000D, "SetTransferEndInterrupt", &Y2rU::SetTransferEndInterrupt);
        Register(0x000E, "GetTransferEndInterrupt", &Y2rU::GetTransferEndInterrupt);
        Register(0x000F, "GetTransferEndEvent", &Y2rU::GetTransferEndEvent);
        Register(0x0010, "SetSendingY", &Y2rU::SetSendingY);
        Register(0x0011, "SetSendingU", &Y2rU::SetSendingU);
        Register(0x0012, "SetSendingV", &Y2rU::SetSendingV);
        Register(0x0013, "SetSendingYUYV", &Y2rU::SetSendingYUYV);
        Register(0x0014, "IsFinishedSendingYuv", &Y2rU::AlwaysFinished);
        Register(0x0015, "IsFinishedSendingY", &Y2rU::AlwaysFinished);
        Register(0x0016, "IsFinishedSendingU", &Y2rU::AlwaysFinished);
        Register(0x0017, "IsFinishedSendingV", &Y2rU::AlwaysFinished);
        Register(0x0018, "SetReceiving", &Y2rU::SetReceiving);
        Register(0x0019, "IsFinishedReceiving", &Y2rU::AlwaysFinished);
        Register(0x001A, "SetInputLineWidth", &Y2rU::SetInputLineWidth);
        Register(0x001B, "GetInputLineWidth", &Y2rU::GetInputLineWidth);
        Register(0x001C, "SetInputLines", &Y2rU::SetInputLines);
        Register(0x001D, "GetInputLines", &Y2rU::GetInputLines);
        Register(0x001E, "SetCoefficient", &Y2rU::SetCoefficient);
        Register(0x001F, "GetCoefficient", &Y2rU::GetCoefficient);
        Register(0x0020, "SetStandardCoefficient", &Y2rU::SetStandardCoefficient);
        Register(0x0021, "GetStandardCoefficient", &Y2rU::GetStandardCoefficient);
        Register(0x0022, "SetAlpha", &Y2rU::SetAlpha);
        Register(0x0023, "GetAlpha", &Y2rU::GetAlpha);
        Register(0x0024, "SetDitheringWeightParams", &Y2rU::SetDitheringWeightParams);
        Register(0x0025, "GetDitheringWeightParams", &Y2rU::GetDitheringWeightParams);
        Register(0x0026, "StartConversion", &Y2rU::StartConversion);
        Register(0x0027, "StopConversion", &Y2rU::StopConversion);
        Register(0x0028, "IsBusyConversion", &Y2rU::IsBusyConversion);
        Register(0x0029, "SetPackageParameter", &Y2rU::SetPackageParameter);
        Register(0x002A, "PingProcess", &Y2rU::PingProcess);
        Register(0x002B, "DriverInitialize", &Y2rU::DriverInitialize);
        Register(0x002C, "DriverFinalize", &Y2rU::DriverFinalize);
        Register(0x002D, "GetPackageParameter", &Y2rU::GetPackageParameter);
    }

private:
    ConversionConfiguration& Conv() { return st_->conversion; }

    static void Ok(Request& request) { request.MakeReply(1, 0).Push(kResultSuccess); }
    template <class T> static void OkWith(Request& request, T value) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(value);
    }

    void SetInputFormat(Request& r) { Conv().input_format = static_cast<InputFormat>(r.Pop<u8>()); Ok(r); }
    void GetInputFormat(Request& r) { OkWith(r, static_cast<u8>(Conv().input_format)); }
    void SetOutputFormat(Request& r) { Conv().output_format = static_cast<OutputFormat>(r.Pop<u8>()); Ok(r); }
    void GetOutputFormat(Request& r) { OkWith(r, static_cast<u8>(Conv().output_format)); }
    void SetRotation(Request& r) { Conv().rotation = static_cast<Rotation>(r.Pop<u8>()); Ok(r); }
    void GetRotation(Request& r) { OkWith(r, static_cast<u8>(Conv().rotation)); }
    void SetBlockAlignment(Request& r) { Conv().block_alignment = static_cast<BlockAlignment>(r.Pop<u8>()); Ok(r); }
    void GetBlockAlignment(Request& r) { OkWith(r, static_cast<u8>(Conv().block_alignment)); }
    void SetSpacialDithering(Request& r) { st_->spacial_dithering_enabled = r.Pop<bool>(); Ok(r); }
    void GetSpacialDithering(Request& r) { OkWith(r, st_->spacial_dithering_enabled); }
    void SetTemporalDithering(Request& r) { st_->temporal_dithering_enabled = r.Pop<bool>(); Ok(r); }
    void GetTemporalDithering(Request& r) { OkWith(r, st_->temporal_dithering_enabled); }
    void SetTransferEndInterrupt(Request& r) { st_->transfer_end_interrupt_enabled = r.Pop<bool>(); Ok(r); }
    void GetTransferEndInterrupt(Request& r) { OkWith(r, st_->transfer_end_interrupt_enabled); }

    void GetTransferEndEvent(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(st_->completion_event);
    }

    // SetSendingY/U/V/YUYV and SetReceiving: address, image size, transfer unit, gap, the process.
    void SetBuffer(Request& request, ConversionBuffer& buffer, const char* what) {
        buffer.address = request.Pop<u32>();
        buffer.image_size = request.Pop<u32>();
        buffer.transfer_unit = static_cast<u16>(request.Pop<u32>());
        buffer.gap = static_cast<u16>(request.Pop<u32>());
        const std::shared_ptr<Process> process = request.PopObject<Process>();
        MH4N_LOG(Debug, kLog, "{} called image_size=0x{:08X}, transfer_unit={}, transfer_stride={}, process_id={}",
                 what, buffer.image_size, buffer.transfer_unit, buffer.gap, process ? process->process_id : 0);
        Ok(request);
    }
    void SetSendingY(Request& r) { SetBuffer(r, Conv().src_Y, "SetSendingY"); }
    void SetSendingU(Request& r) { SetBuffer(r, Conv().src_U, "SetSendingU"); }
    void SetSendingV(Request& r) { SetBuffer(r, Conv().src_V, "SetSendingV"); }
    void SetSendingYUYV(Request& r) { SetBuffer(r, Conv().src_YUYV, "SetSendingYUYV"); }
    void SetReceiving(Request& r) { SetBuffer(r, Conv().dst, "SetReceiving"); }

    void AlwaysFinished(Request& request) { OkWith(request, static_cast<u8>(1)); }

    void SetInputLineWidth(Request& request) {
        const u32 width = request.Pop<u32>();
        request.MakeReply(1, 0).Push(st_->SetInputLineWidth(static_cast<u16>(width)));
    }
    void GetInputLineWidth(Request& r) { OkWith(r, static_cast<u32>(Conv().input_line_width)); }
    void SetInputLines(Request& request) {
        const u32 lines = request.Pop<u32>();
        request.MakeReply(1, 0).Push(st_->SetInputLines(static_cast<u16>(lines)));
    }
    void GetInputLines(Request& r) { OkWith(r, static_cast<u32>(Conv().input_lines)); }

    void SetCoefficient(Request& request) {
        const CoefficientSet set = request.PopRaw<CoefficientSet>();
        Conv().coefficients = set;
        Ok(request);
    }
    void GetCoefficient(Request& request) {
        Reply& reply = request.MakeReply(5, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(Conv().coefficients);
    }
    void SetStandardCoefficient(Request& request) {
        const u32 index = request.Pop<u32>();
        request.MakeReply(1, 0).Push(st_->SetStandardCoefficient(static_cast<u8>(index)));
    }
    void GetStandardCoefficient(Request& request) {
        const u32 index = request.Pop<u32>();
        if (index < kStandardCoefficients.size()) {
            Reply& reply = request.MakeReply(5, 0);
            reply.Push(kResultSuccess);
            reply.PushRaw(kStandardCoefficients[index]);
        } else {
            MH4N_LOG(Error, kLog, "called standard_coefficient={}  The argument is invalid!", index);
            request.MakeReply(1, 0).Push(kResultInvalidEnumValue);
        }
    }
    void SetAlpha(Request& request) {
        Conv().alpha = static_cast<u16>(request.Pop<u32>());
        Ok(request);
    }
    void GetAlpha(Request& r) { OkWith(r, static_cast<u32>(Conv().alpha)); }

    void SetDitheringWeightParams(Request& request) {
        st_->dithering_weight_params = request.PopRaw<std::array<u16, 16>>();
        Ok(request);
    }
    void GetDitheringWeightParams(Request& request) {
        Reply& reply = request.MakeReply(9, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(st_->dithering_weight_params);
    }

    // 0x00260000: converts at once; the completion event is signalled 50000 ticks later.
    void StartConversion(Request& request) {
        ConversionConfiguration& c = Conv();
        // dst_image_size would seem to be perfect for this, but it doesn't include the gap
        const u32 total_output_size = static_cast<u32>(c.input_lines) * (static_cast<u32>(c.dst.transfer_unit) + c.dst.gap);
        gpu::Gpu* gpu = Context().Interfaces().gpu;
        if (gpu != nullptr) {
            gpu->InvalidateRegion(c.dst.address, total_output_size);  // write back what the GPU holds
            gpu->FlushRegion(c.dst.address, total_output_size);       // and forget it: it is overwritten
        }
        PerformConversion(Context().Memory(), c);

        Kernel& kernel = Context().GetKernel();
        if (st_->is_busy_conversion && st_->completion_timer != 0) {
            kernel.Cancel(st_->completion_timer);
        }
        const std::shared_ptr<Y2rState> state = st_;
        st_->completion_timer = kernel.ScheduleAt(kernel.NowTicks() + kMinY2rDelayTicks, [state] {
            state->completion_timer = 0;
            state->completion_event->Signal();
            state->is_busy_conversion = false;
        });
        st_->is_busy_conversion = true;
        Ok(request);
    }
    void StopConversion(Request& request) {
        if (st_->is_busy_conversion) {
            st_->is_busy_conversion = false;
            if (st_->completion_timer != 0) {
                Context().GetKernel().Cancel(st_->completion_timer);
                st_->completion_timer = 0;
            }
        }
        Ok(request);
    }
    void IsBusyConversion(Request& r) { OkWith(r, st_->is_busy_conversion); }

    // 0x00290102: the 12 bytes of ConversionParameters.
    void SetPackageParameter(Request& request) {
        struct Parameters {
            u8 input_format, output_format, rotation, block_alignment;
            u16 input_line_width, input_lines;
            u8 standard_coefficient, padding;
            u16 alpha;
        };
        static_assert(sizeof(Parameters) == 12);
        const Parameters params = request.PopRaw<Parameters>();
        Conv().input_format = static_cast<InputFormat>(params.input_format);
        Conv().output_format = static_cast<OutputFormat>(params.output_format);
        Conv().rotation = static_cast<Rotation>(params.rotation);
        Conv().block_alignment = static_cast<BlockAlignment>(params.block_alignment);
        Result result = st_->SetInputLineWidth(params.input_line_width);
        if (result == kResultSuccess) {
            result = st_->SetInputLines(params.input_lines);
        }
        if (result == kResultSuccess) {
            result = st_->SetStandardCoefficient(params.standard_coefficient);
        }
        if (result == kResultSuccess) {
            Conv().padding = params.padding;
            Conv().alpha = params.alpha;
        }
        request.MakeReply(1, 0).Push(result);
    }

    void PingProcess(Request& request) { OkWith(request, static_cast<u8>(0)); }

    void DriverInitialize(Request& request) {
        ConversionConfiguration& c = Conv();
        c.input_format = InputFormat::YUV422_Indiv8;
        c.output_format = OutputFormat::RGBA8;
        c.rotation = Rotation::None;
        c.block_alignment = BlockAlignment::Linear;
        c.coefficients.fill(0);
        st_->SetInputLineWidth(1024);
        st_->SetInputLines(1024);
        c.alpha = 0;
        const ConversionBuffer zero{};
        c.src_Y = zero;
        c.src_U = zero;
        c.src_V = zero;
        c.dst = zero;
        st_->completion_event->Clear();
        Ok(request);
    }
    void DriverFinalize(Request& request) { Ok(request); }

    // The first 12 bytes of the configuration (Azahar pushes the whole structure into a reply that
    // holds three words after the result).
    void GetPackageParameter(Request& request) {
        const ConversionConfiguration& c = Conv();
        Reply& reply = request.MakeReply(4, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(static_cast<u32>(c.input_format) | static_cast<u32>(c.output_format) << 8 |
                        static_cast<u32>(c.rotation) << 16 | static_cast<u32>(c.block_alignment) << 24);
        reply.Push<u32>(static_cast<u32>(c.input_line_width) | static_cast<u32>(c.input_lines) << 16);
        reply.Push<u32>(static_cast<u16>(c.coefficients[0]) | static_cast<u32>(static_cast<u16>(c.coefficients[1])) << 16);
    }

    std::shared_ptr<Y2rState> st_;
};

MH4N_REGISTER(ServiceFactory, "y2r:u",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<Y2rU>(context); }, 1));

}  // namespace

}  // namespace mh4n::y2r
