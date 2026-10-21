// Derived from Azahar src/core/hle/service/cam/cam.cpp, src/core/hle/service/cam/cam.h,
// src/core/hle/service/cam/cam_params.h, src/core/hle/service/cam/cam_u.cpp and
// src/core/frontend/camera/blank_camera.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: each of them says "GPLv2 or any later version"): the command table of cam:u with its
// session limit, the enumerations and structures (Effect, Flip, Size, FrameRate, OutputFormat, the
// package parameters, the stereo calibration data), the preset resolutions and frame latencies, the
// port and camera state machine (activation, capture, receiving, trimming, contexts), the checks and
// result codes of every command, the vsync and completion events, and the blank camera.
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (cam.cpp)
// Copyright Citra Emulator Project / Lime3DS Emulator Project (cam.h, cam_params.h)
// Copyright 2015 Citra Emulator Project (cam_u.cpp)
// Copyright 2016 Citra Emulator Project (blank_camera.cpp)
//
// cam:u (N1-19): the camera service, with the camera Azahar uses when none is configured: a blank one.
// Every frame it delivers is black (YUV 0x8000 per pixel, or 0 for RGB565) in the size and format the
// game selected. The ports work as in Azahar:
//  - Activate starts a port's vsync chain: the vsync event is signalled every frame period (67, 100,
//    118, 200, 50 or 33 ms by the frame rate, in exact ticks of the timer queue: the next signal is the
//    previous one's scheduled tick plus the period), and the latest five times (microseconds of game
//    time) are kept for GetLatestVsyncTiming. A second Activate starts a second chain, as in Azahar, and
//    Activate of the inner camera (0x02) calls ActivatePort(0, 1) twice, as Azahar does, so it starts two
//    chains at once and each period adds two equal entries to the times.
//  - SetReceiving on a port that is capturing delivers a frame one frame period later: the pixels are
//    written to the destination (trimmed if SetTrimming asked) and the completion event, which is
//    sticky and cleared by the next SetReceiving, is signalled. On a port that is not capturing the
//    receive waits for StartCapture.
//  - The commands that are nullptr entries in Azahar's table (SetExposure, SetWhiteBalance,
//    SetSharpness, SetAutoExposure, SetAutoWhiteBalance, SetPhotoMode, SetContrast, SetLensCorrection,
//    SetAutoExposureWindow, SetAutoWhiteBalanceWindow, GetActivatedCamera, GetSleepCamera,
//    SetSleepCamera and the others) are answered as unknown commands.
//
// Departures from Azahar: the frame is made when the receive starts and written at the end (Azahar
// makes it on a worker thread), and a command that uses a camera whose driver has not been
// initialised creates the blank camera instead of dereferencing a null pointer; SetSize with a size
// that is not one of the eight presets is an invalid enumeration value (Azahar indexes past its
// table); the Process handle SetReceiving gets is not used, the application's memory is written;
// SetPackageParameterWithContext with a size that is not a preset is an invalid enumeration value; GetMaxLines
// when its first guess of lines is 0 (width 0 or above 2560, or height 0) and GetMaxBytes when the
// byte count reaches 0 stop instead of dividing by zero; a
// command that changes one field of the camera's current context re-applies the context's format and
// resolution to the blank camera as well (ApplyContext, and LoadCameraImplementation for the first
// use), where Azahar's commands apply only the field they change. The blank camera's output depends on
// the format and resolution alone, so this shows only if the camera's own format or resolution lagged
// the current context's, which Azahar's per-field commands allow. The error replies whose declared word
// Azahar leaves unwritten (rb.Skip(1, false): IsBusy, IsFinishedReceiving, GetTransferBytes, IsTrimming,
// GetTrimmingParams (its four coordinate words) and the GetMaxLines and GetMaxBytes error replies) answer
// the request's word of that position when it lies inside the request, else 0; Azahar leaves the guest's
// own command-buffer content there (Stale()). LatencyMs, and the vsync chain's period, use the table's
// entry 0 (67 ms) for a frame rate above 12, where Azahar indexes past its table.
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n::cam {

namespace {

constexpr const char* kLog = "Service.CAM";

constexpr u32 kModuleCam = 20;
constexpr Result kResultInvalidEnumValue = MakeResult(1005, kModuleCam, 7, 28);
constexpr Result kResultOutOfRange = MakeResult(1021, kModuleCam, 7, 28);

constexpr int kNumCameras = 3;
constexpr std::size_t kMaxVsyncTimings = 5;

struct Resolution {
    u16 width, height, crop_x0, crop_y0, crop_x1, crop_y1;
};

// built-in resolution parameters
constexpr std::array<Resolution, 8> kPresetResolution = {{
    {640, 480, 0, 0, 639, 479},   // VGA
    {320, 240, 0, 0, 639, 479},   // QVGA
    {160, 120, 0, 0, 639, 479},   // QQVGA
    {352, 288, 26, 0, 613, 479},  // CIF
    {176, 144, 26, 0, 613, 479},  // QCIF
    {256, 192, 0, 0, 639, 479},   // DS_LCD
    {512, 384, 0, 0, 639, 479},   // DS_LCDx4
    {400, 240, 0, 48, 639, 431},  // CTR_TOP_LCD
}};

// latency in ms for each frame rate option
constexpr std::array<int, 13> kLatencyByFrameRate = {{67, 67, 67, 100, 118, 200, 50, 50, 33, 33, 67, 50, 33}};

// msToCycles(int ms): the clock rate times the milliseconds, divided by 1000 (integer arithmetic).
constexpr u64 MsToTicks(int ms) {
    return kTicksPerSecond * static_cast<u64>(ms) / 1000;
}

// A set of ports, cameras or contexts in a command parameter (Azahar's CommandParamBitSet).
template <int max_index> struct ParamSet {
    u8 value;
    bool IsValid() const { return value < (1 << max_index); }
    int Count() const { return std::popcount(value); }
    bool IsSingle() const { return IsValid() && Count() == 1; }
    bool operator[](int i) const { return (value >> i) & 1; }
    std::vector<int> Members() const {
        std::vector<int> out;
        for (int i = 0; i < 8; ++i) {
            if (operator[](i)) out.push_back(i);
        }
        return out;
    }
};
using PortSet = ParamSet<2>;
using ContextSet = ParamSet<2>;
using CameraSet = ParamSet<3>;

struct ContextConfig {
    u8 flip = 0;    // NoFlip
    u8 effect = 0;  // NoEffect
    u8 format = 0;  // YUV422
    Resolution resolution{0, 0, 0, 0, 0, 0};
};

// The blank camera: it keeps the size and the format it was told, and every frame it makes is black.
struct BlankCamera {
    bool exists = false;
    int width = 0;
    int height = 0;
    bool output_rgb = false;
    std::vector<u16> ReceiveFrame() const {
        // 0x80008000 stands for two black pixels in YUV422
        return std::vector<u16>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), output_rgb ? 0 : 0x8000);
    }
};

struct CameraConfig {
    BlankCamera impl;
    std::array<ContextConfig, 2> contexts;
    int current_context = 0;
    u8 frame_rate = 0;  // Rate_15
};

struct PortConfig {
    int camera_id = 0;
    bool is_active = false;
    bool is_pending_receiving = false;
    bool is_busy = false;
    bool is_receiving = false;
    bool is_trimming = false;
    u16 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    u16 transfer_bytes = 256;
    std::shared_ptr<Event> completion_event;
    std::shared_ptr<Event> buffer_error_interrupt_event;
    std::shared_ptr<Event> vsync_interrupt_event;
    std::deque<s64> vsync_timings;
    std::vector<u16> capture_result;
    u32 dest = 0;
    u32 dest_size = 0;
    u64 completion_timer = 0;
    std::set<u64> vsync_timers;
};

struct CamModule {
    bool created = false;
    bool initialized = false;
    std::array<CameraConfig, kNumCameras> cameras;
    std::array<PortConfig, 2> ports;
};

void ClearPort(PortConfig& port) {
    port.completion_event->Clear();
    port.buffer_error_interrupt_event->Clear();
    port.vsync_interrupt_event->Clear();
    port.vsync_timings.clear();
    port.is_receiving = false;
    port.is_active = false;
    port.is_pending_receiving = false;
    port.is_busy = false;
    port.is_trimming = false;
    port.x0 = port.y0 = port.x1 = port.y1 = 0;
    port.transfer_bytes = 256;
}

// The hardware's stereo calibration data (values from Azahar: "valid data is required here or games
// using the calibration get stuck in an infinite CPU loop").
std::array<u32, 16> CalibrationWords() {
    const auto bits = [](float v) { return std::bit_cast<u32>(v); };
    return {0u,                          // isValidRotationXY and padding
            bits(1.001776f),             // scale
            bits(0.008322907f),          // rotationZ
            bits(-87.70484f),            // translationX
            bits(-7.640977f),            // translationY
            bits(0.0f),                  // rotationX
            bits(0.0f),                  // rotationY
            bits(64.66875f),             // angleOfViewRight
            bits(64.76067f),             // angleOfViewLeft
            bits(250.0f),                // distanceToChart
            bits(35.0f),                 // distanceCameras
            640u | (480u << 16),         // imageWidth, imageHeight
            0u, 0u, 0u, 0u};             // padding
}

class CamU final : public Service {
public:
    explicit CamU(ServiceContext& context) : Service(context, "cam:u"), m_(context.State<CamModule>("cam:u")) {
        if (!m_->created) {
            m_->created = true;
            Kernel& kernel = context.GetKernel();
            for (PortConfig& port : m_->ports) {
                port.completion_event = kernel.NewEvent(ResetType::Sticky, "CAM::completion_event");
                port.buffer_error_interrupt_event = kernel.NewEvent(ResetType::OneShot, "CAM::buffer_error_interrupt_event");
                port.vsync_interrupt_event = kernel.NewEvent(ResetType::OneShot, "CAM::vsync_interrupt_event");
            }
        }
        Register(0x0001, "StartCapture", &CamU::StartCapture);
        Register(0x0002, "StopCapture", &CamU::StopCapture);
        Register(0x0003, "IsBusy", &CamU::IsBusy);
        Register(0x0004, "ClearBuffer", &CamU::ClearBuffer);
        Register(0x0005, "GetVsyncInterruptEvent", &CamU::GetVsyncInterruptEvent);
        Register(0x0006, "GetBufferErrorInterruptEvent", &CamU::GetBufferErrorInterruptEvent);
        Register(0x0007, "SetReceiving", &CamU::SetReceiving);
        Register(0x0008, "IsFinishedReceiving", &CamU::IsFinishedReceiving);
        Register(0x0009, "SetTransferLines", &CamU::SetTransferLines);
        Register(0x000A, "GetMaxLines", &CamU::GetMaxLines);
        Register(0x000B, "SetTransferBytes", &CamU::SetTransferBytes);
        Register(0x000C, "GetTransferBytes", &CamU::GetTransferBytes);
        Register(0x000D, "GetMaxBytes", &CamU::GetMaxBytes);
        Register(0x000E, "SetTrimming", &CamU::SetTrimming);
        Register(0x000F, "IsTrimming", &CamU::IsTrimming);
        Register(0x0010, "SetTrimmingParams", &CamU::SetTrimmingParams);
        Register(0x0011, "GetTrimmingParams", &CamU::GetTrimmingParams);
        Register(0x0012, "SetTrimmingParamsCenter", &CamU::SetTrimmingParamsCenter);
        Register(0x0013, "Activate", &CamU::Activate);
        Register(0x0014, "SwitchContext", &CamU::SwitchContext);
        Register(0x0015, "SetExposure");
        Register(0x0016, "SetWhiteBalance");
        Register(0x0017, "SetWhiteBalanceWithoutBaseUp");
        Register(0x0018, "SetSharpness");
        Register(0x0019, "SetAutoExposure");
        Register(0x001A, "IsAutoExposure");
        Register(0x001B, "SetAutoWhiteBalance");
        Register(0x001C, "IsAutoWhiteBalance");
        Register(0x001D, "FlipImage", &CamU::FlipImage);
        Register(0x001E, "SetDetailSize", &CamU::SetDetailSize);
        Register(0x001F, "SetSize", &CamU::SetSize);
        Register(0x0020, "SetFrameRate", &CamU::SetFrameRate);
        Register(0x0021, "SetPhotoMode");
        Register(0x0022, "SetEffect", &CamU::SetEffect);
        Register(0x0023, "SetContrast");
        Register(0x0024, "SetLensCorrection");
        Register(0x0025, "SetOutputFormat", &CamU::SetOutputFormat);
        Register(0x0026, "SetAutoExposureWindow");
        Register(0x0027, "SetAutoWhiteBalanceWindow");
        Register(0x0028, "SetNoiseFilter");
        Register(0x0029, "SynchronizeVsyncTiming", &CamU::SynchronizeVsyncTiming);
        Register(0x002A, "GetLatestVsyncTiming", &CamU::GetLatestVsyncTiming);
        Register(0x002B, "GetStereoCameraCalibrationData", &CamU::GetStereoCameraCalibrationData);
        Register(0x002C, "SetStereoCameraCalibrationData");
        Register(0x002D, "WriteRegisterI2c");
        Register(0x002E, "WriteMcuVariableI2c");
        Register(0x002F, "ReadRegisterI2cExclusive");
        Register(0x0030, "ReadMcuVariableI2cExclusive");
        Register(0x0031, "SetImageQualityCalibrationData");
        Register(0x0032, "GetImageQualityCalibrationData");
        Register(0x0033, "SetPackageParameterWithoutContext", &CamU::SetPackageParameterWithoutContext);
        Register(0x0034, "SetPackageParameterWithContext", &CamU::SetPackageParameterWithContext);
        Register(0x0035, "SetPackageParameterWithContextDetail", &CamU::SetPackageParameterWithContextDetail);
        Register(0x0036, "GetSuitableY2rStandardCoefficient", &CamU::GetSuitableY2rStandardCoefficient);
        Register(0x0037, "PlayShutterSoundWithWave");
        Register(0x0038, "PlayShutterSound", &CamU::PlayShutterSound);
        Register(0x0039, "DriverInitialize", &CamU::DriverInitialize);
        Register(0x003A, "DriverFinalize", &CamU::DriverFinalize);
        Register(0x003B, "GetActivatedCamera");
        Register(0x003C, "GetSleepCamera");
        Register(0x003D, "SetSleepCamera");
        Register(0x003E, "SetBrightnessSynchronization");
    }

private:
    Kernel& GetKernel() { return Context().GetKernel(); }
    PortConfig& Port(int i) { return m_->ports[static_cast<std::size_t>(i)]; }
    CameraConfig& Camera(int i) { return m_->cameras[static_cast<std::size_t>(i)]; }

    static u32 Stale(const Request& request, std::size_t index) {
        return index < request.Words().size() ? request.Words()[index] : 0u;
    }
    static void ResultOnly(Request& request, Result result) { request.MakeReply(1, 0).Push(result); }

    // ---- the camera implementation (the blank camera) ----

    void LoadCameraImplementation(int camera_id) {
        CameraConfig& camera = Camera(camera_id);
        camera.impl.exists = true;
        // flip and effect do nothing to a blank camera
        camera.impl.output_rgb = camera.contexts[0].format == 1;
        camera.impl.width = camera.contexts[0].resolution.width;
        camera.impl.height = camera.contexts[0].resolution.height;
    }
    BlankCamera& Impl(int camera_id) {
        if (!Camera(camera_id).impl.exists) LoadCameraImplementation(camera_id);
        return Camera(camera_id).impl;
    }
    void ApplyContext(int camera_id, const ContextConfig& context) {
        BlankCamera& impl = Impl(camera_id);
        impl.output_rgb = context.format == 1;
        impl.width = context.resolution.width;
        impl.height = context.resolution.height;
    }

    // ---- timing ----

    int LatencyMs(int camera_id) {
        const u8 rate = Camera(camera_id).frame_rate;
        return kLatencyByFrameRate[rate < kLatencyByFrameRate.size() ? rate : 0];
    }

    static s64 GlobalTimeUs(Kernel& kernel) {
        return static_cast<s64>(TicksToNs(kernel.NowTicks()) / 1000);
    }

    // A link of a port's vsync chain: signals the event and starts the next link, one period later.
    static void ScheduleVsync(const std::shared_ptr<CamModule>& module, Kernel* kernel, int port_id, u64 when) {
        const auto slot = std::make_shared<u64>(0);
        *slot = kernel->ScheduleAt(when, [module, kernel, port_id, when, slot] {
            PortConfig& port = module->ports[static_cast<std::size_t>(port_id)];
            port.vsync_timers.erase(*slot);  // this link has run
            if (!port.is_active) {
                return;
            }
            port.vsync_timings.push_front(GlobalTimeUs(*kernel));
            if (port.vsync_timings.size() > kMaxVsyncTimings) port.vsync_timings.pop_back();
            port.vsync_interrupt_event->Signal();
            const CameraConfig& camera = module->cameras[static_cast<std::size_t>(port.camera_id)];
            const u8 rate = camera.frame_rate;
            const int latency = kLatencyByFrameRate[rate < kLatencyByFrameRate.size() ? rate : 0];
            ScheduleVsync(module, kernel, port_id, when + MsToTicks(latency));
        });
        module->ports[static_cast<std::size_t>(port_id)].vsync_timers.insert(*slot);
    }

    void UnscheduleVsync(int port_id) {
        PortConfig& port = Port(port_id);
        for (const u64 id : port.vsync_timers) GetKernel().Cancel(id);
        port.vsync_timers.clear();
    }

    // The completion of a receive: the frame goes to the destination and the event is signalled.
    static void Complete(const std::shared_ptr<CamModule>& module, ServiceContext* context, int port_id) {
        PortConfig& port = module->ports[static_cast<std::size_t>(port_id)];
        port.completion_timer = 0;
        const CameraConfig& camera = module->cameras[static_cast<std::size_t>(port.camera_id)];
        const std::vector<u16>& buffer = port.capture_result;
        GuestMemory& memory = context->Memory();
        u32 written_from = port.dest, written_size = 0;
        if (port.is_trimming) {
            const Resolution& resolution = camera.contexts[static_cast<std::size_t>(camera.current_context)].resolution;
            const int original_width = resolution.width;
            const int original_height = resolution.height;
            u32 trim_width, trim_height;
            if (port.x1 <= port.x0 || port.y1 <= port.y0 || port.x1 > original_width || port.y1 > original_height) {
                MH4N_LOG(Error, kLog, "Invalid trimming coordinates x0={}, y0={}, x1={}, y1={}", port.x0, port.y0, port.x1, port.y1);
                trim_width = 0;
                trim_height = 0;
            } else {
                trim_width = static_cast<u32>(port.x1 - port.x0);
                trim_height = static_cast<u32>(port.y1 - port.y0);
            }
            const u32 trim_size = static_cast<u32>(port.x1 - port.x0) * static_cast<u32>(port.y1 - port.y0) * 2;
            if (port.dest_size != trim_size) {
                MH4N_LOG(Error, kLog, "The destination size ({}) doesn't match the source ({})!", port.dest_size, trim_size);
            }
            const u32 src_offset = static_cast<u32>(port.y0) * static_cast<u32>(original_width) + port.x0;
            // src_size_left is an int because it can be negative if the buffer size doesn't match.
            int src_size_left = static_cast<int>((static_cast<std::int64_t>(buffer.size()) - src_offset) * 2);
            u32 dest_ptr = port.dest;
            int dest_size_left = static_cast<int>(port.dest_size);
            const int line_bytes = static_cast<int>(trim_width * sizeof(u16));
            std::size_t src_index = src_offset;
            for (u32 y = 0; y < trim_height; ++y) {
                const int copy_length = std::min({line_bytes, dest_size_left, src_size_left});
                if (copy_length <= 0) break;
                memory.WriteBlock(dest_ptr, buffer.data() + src_index, static_cast<u32>(copy_length));
                dest_ptr += static_cast<u32>(copy_length);
                written_size += static_cast<u32>(copy_length);
                dest_size_left -= copy_length;
                src_index += static_cast<std::size_t>(original_width);
                src_size_left -= original_width * static_cast<int>(sizeof(u16));
            }
        } else {
            const std::size_t buffer_size = buffer.size() * sizeof(u16);
            if (port.dest_size != buffer_size) {
                MH4N_LOG(Error, kLog, "The destination size ({}) doesn't match the source ({})!", port.dest_size, buffer_size);
            }
            written_size = static_cast<u32>(std::min<std::size_t>(port.dest_size, buffer_size));
            memory.WriteBlock(port.dest, buffer.data(), written_size);
        }
        if (gpu::Gpu* gpu = context->Interfaces().gpu; gpu != nullptr && written_size != 0) {
            gpu->FlushRegion(written_from, written_size);  // the CPU wrote this range
        }
        port.is_receiving = false;
        port.completion_event->Signal();
    }

    void StartReceiving(int port_id) {
        PortConfig& port = Port(port_id);
        port.is_receiving = true;
        // the frame the camera makes now; it is delivered when the frame period has passed
        port.capture_result = Impl(port.camera_id).ReceiveFrame();
        Kernel& kernel = GetKernel();
        const std::shared_ptr<CamModule> module = m_;
        ServiceContext* context = &Context();
        port.completion_timer = kernel.ScheduleAt(kernel.NowTicks() + MsToTicks(LatencyMs(port.camera_id)),
                                                  [module, context, port_id] { Complete(module, context, port_id); });
    }

    void CancelReceiving(int port_id) {
        PortConfig& port = Port(port_id);
        if (!port.is_receiving) return;
        MH4N_LOG(Warning, kLog, "tries to cancel an ongoing receiving process.");
        if (port.completion_timer != 0) {
            GetKernel().Cancel(port.completion_timer);
            port.completion_timer = 0;
        }
        port.is_receiving = false;
    }

    void ActivatePort(int port_id, int camera_id) {
        PortConfig& port = Port(port_id);
        if (port.is_busy && port.camera_id != camera_id) {
            CancelReceiving(port_id);
            Impl(port.camera_id);  // StopCapture of the blank camera does nothing
            port.is_busy = false;
        }
        port.is_active = true;
        port.camera_id = camera_id;
        Kernel& kernel = GetKernel();
        ScheduleVsync(m_, &kernel, port_id, kernel.NowTicks() + MsToTicks(LatencyMs(camera_id)));
    }

    // ---- commands ----

    // 0x00010040: the ports.
    void StartCapture(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        if (!port_select.IsValid()) {
            MH4N_LOG(Error, kLog, "invalid port_select={}", port_select.value);
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        for (const int i : port_select.Members()) {
            PortConfig& port = Port(i);
            if (port.is_busy) {
                MH4N_LOG(Warning, kLog, "port {} already started", i);
                continue;
            }
            if (!port.is_active) {
                // This doesn't return an error, but seems to put the camera in an undefined state
                MH4N_LOG(Error, kLog, "port {} hasn't been activated", i);
                continue;
            }
            Impl(port.camera_id);
            port.is_busy = true;
            if (port.is_pending_receiving) {
                port.is_pending_receiving = false;
                StartReceiving(i);
            }
        }
        ResultOnly(request, kResultSuccess);
    }

    void StopCapture(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        if (!port_select.IsValid()) {
            MH4N_LOG(Error, kLog, "invalid port_select={}", port_select.value);
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        for (const int i : port_select.Members()) {
            if (Port(i).is_busy) {
                CancelReceiving(i);
                Port(i).is_busy = false;
            } else {
                MH4N_LOG(Warning, kLog, "port {} already stopped", i);
            }
        }
        ResultOnly(request, kResultSuccess);
    }

    void IsBusy(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        Reply& reply = request.MakeReply(2, 0);
        if (!port_select.IsValid()) {
            reply.Push(kResultInvalidEnumValue);
            reply.Push(Stale(request, 2));
            return;
        }
        bool is_busy = true;
        // Note: the behaviour on no or both ports selected are verified against real 3DS.
        for (const int i : port_select.Members()) is_busy &= Port(i).is_busy;
        reply.Push(kResultSuccess);
        reply.Push(is_busy);
    }

    void ClearBuffer(Request& request) {
        request.Pop<u8>();
        ResultOnly(request, kResultSuccess);
    }

    void GetEvent(Request& request, bool vsync) {
        const PortSet port_select{request.Pop<u8>()};
        Reply& reply = request.MakeReply(1, 2);
        if (port_select.IsSingle()) {
            PortConfig& port = Port(port_select.Members().front());
            reply.Push(kResultSuccess);
            reply.PushCopyObjects(vsync ? port.vsync_interrupt_event : port.buffer_error_interrupt_event);
        } else {
            MH4N_LOG(Error, kLog, "invalid port_select={}", port_select.value);
            reply.Push(kResultInvalidEnumValue);
            reply.PushCopyObjects(std::shared_ptr<Object>());
        }
    }
    void GetVsyncInterruptEvent(Request& request) { GetEvent(request, true); }
    void GetBufferErrorInterruptEvent(Request& request) { GetEvent(request, false); }

    // 0x00070102: the destination, the port, the size, the transfer unit (u16), the process.
    void SetReceiving(Request& request) {
        const u32 dest = request.Pop<u32>();
        const PortSet port_select{request.Pop<u8>()};
        const u32 image_size = request.Pop<u32>();
        const u16 trans_unit = request.Pop<u16>();
        request.PopObject<Process>();
        Reply& reply = request.MakeReply(1, 2);
        if (port_select.IsSingle()) {
            const int port_id = port_select.Members().front();
            PortConfig& port = Port(port_id);
            CancelReceiving(port_id);
            port.completion_event->Clear();
            port.dest = dest;
            port.dest_size = image_size;
            if (port.is_busy) {
                StartReceiving(port_id);
            } else {
                port.is_pending_receiving = true;
            }
            reply.Push(kResultSuccess);
            reply.PushCopyObjects(port.completion_event);
        } else {
            MH4N_LOG(Error, kLog, "invalid port_select={}", port_select.value);
            reply.Push(kResultInvalidEnumValue);
            reply.PushCopyObjects(std::shared_ptr<Object>());
        }
        MH4N_LOG(Debug, kLog, "called, addr=0x{:X}, port_select={}, image_size={}, trans_unit={}", dest,
                 port_select.value, image_size, trans_unit);
    }

    void IsFinishedReceiving(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        Reply& reply = request.MakeReply(2, 0);
        if (port_select.IsSingle()) {
            const PortConfig& port = Port(port_select.Members().front());
            reply.Push(kResultSuccess);
            reply.Push(!(port.is_receiving || port.is_pending_receiving));
        } else {
            reply.Push(kResultInvalidEnumValue);
            reply.Push(Stale(request, 2));
        }
    }

    void SetTransferLines(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        const u16 transfer_lines = request.Pop<u16>();
        const u16 width = request.Pop<u16>();
        request.Pop<u16>();  // height
        if (!port_select.IsValid()) return ResultOnly(request, kResultInvalidEnumValue);
        for (const int i : port_select.Members()) {
            Port(i).transfer_bytes = static_cast<u16>(static_cast<u32>(transfer_lines) * width * 2);
        }
        ResultOnly(request, kResultSuccess);
    }

    void GetMaxLines(Request& request) {
        const u32 width = request.Pop<u16>();
        const u32 height = request.Pop<u16>();
        Reply& reply = request.MakeReply(2, 0);
        // Note: the result of the algorithm below are hwtested with width < 640 and with height < 480
        constexpr u32 kMinTransferUnit = 256;
        constexpr u32 kMaxBufferSize = 2560;
        if (width * height * 2 % kMinTransferUnit != 0) {
            reply.Push(kResultOutOfRange);
            reply.Push(Stale(request, 2));
            return;
        }
        u32 lines = width == 0 ? 0 : kMaxBufferSize / width;
        if (lines > height) lines = height;
        Result result = kResultSuccess;
        while (lines == 0 || height % lines != 0 || (lines * width * 2 % kMinTransferUnit != 0)) {
            --lines;
            if (lines == 0 || lines == 0xFFFFFFFF) {
                lines = 0;
                result = kResultOutOfRange;
                break;
            }
        }
        reply.Push(result);
        reply.Push(lines);
    }

    void SetTransferBytes(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        const u16 transfer_bytes = request.Pop<u16>();
        request.Pop<u16>();
        request.Pop<u16>();
        if (!port_select.IsValid()) return ResultOnly(request, kResultInvalidEnumValue);
        for (const int i : port_select.Members()) Port(i).transfer_bytes = transfer_bytes;
        ResultOnly(request, kResultSuccess);
    }

    void GetTransferBytes(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        Reply& reply = request.MakeReply(2, 0);
        if (port_select.IsSingle()) {
            reply.Push(kResultSuccess);
            reply.Push(Port(port_select.Members().front()).transfer_bytes);
        } else {
            reply.Push(kResultInvalidEnumValue);
            reply.Push(Stale(request, 2));
        }
    }

    void GetMaxBytes(Request& request) {
        const u32 width = request.Pop<u16>();
        const u32 height = request.Pop<u16>();
        Reply& reply = request.MakeReply(2, 0);
        constexpr u32 kMinTransferUnit = 256;
        constexpr u32 kMaxBufferSize = 2560;
        if (width * height * 2 % kMinTransferUnit != 0) {
            reply.Push(kResultOutOfRange);
            reply.Push(Stale(request, 2));
            return;
        }
        u32 bytes = kMaxBufferSize;
        while (bytes != 0 && width * height * 2 % bytes != 0) bytes -= kMinTransferUnit;
        reply.Push(kResultSuccess);
        reply.Push(bytes);
    }

    void SetTrimming(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        const bool trim = request.Pop<bool>();
        if (!port_select.IsValid()) return ResultOnly(request, kResultInvalidEnumValue);
        for (const int i : port_select.Members()) Port(i).is_trimming = trim;
        ResultOnly(request, kResultSuccess);
    }

    void IsTrimming(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        Reply& reply = request.MakeReply(2, 0);
        if (port_select.IsSingle()) {
            reply.Push(kResultSuccess);
            reply.Push(Port(port_select.Members().front()).is_trimming);
        } else {
            reply.Push(kResultInvalidEnumValue);
            reply.Push(Stale(request, 2));
        }
    }

    void SetTrimmingParams(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        const u16 x0 = request.Pop<u16>(), y0 = request.Pop<u16>(), x1 = request.Pop<u16>(), y1 = request.Pop<u16>();
        if (!port_select.IsValid()) return ResultOnly(request, kResultInvalidEnumValue);
        for (const int i : port_select.Members()) {
            Port(i).x0 = x0, Port(i).y0 = y0, Port(i).x1 = x1, Port(i).y1 = y1;
        }
        ResultOnly(request, kResultSuccess);
    }

    void GetTrimmingParams(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        Reply& reply = request.MakeReply(5, 0);
        if (port_select.IsSingle()) {
            const PortConfig& port = Port(port_select.Members().front());
            reply.Push(kResultSuccess);
            reply.Push(port.x0);
            reply.Push(port.y0);
            reply.Push(port.x1);
            reply.Push(port.y1);
        } else {
            reply.Push(kResultInvalidEnumValue);
            for (std::size_t i = 2; i < 6; ++i) reply.Push(Stale(request, i));
        }
    }

    void SetTrimmingParamsCenter(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        const u16 trim_w = request.Pop<u16>(), trim_h = request.Pop<u16>(), cam_w = request.Pop<u16>(),
                  cam_h = request.Pop<u16>();
        if (!port_select.IsValid()) return ResultOnly(request, kResultInvalidEnumValue);
        for (const int i : port_select.Members()) {
            PortConfig& port = Port(i);
            // the arithmetic is on ints and the result stored in 16 bits
            port.x0 = static_cast<u16>((cam_w - trim_w) / 2);
            port.y0 = static_cast<u16>((cam_h - trim_h) / 2);
            port.x1 = static_cast<u16>(port.x0 + trim_w);
            port.y1 = static_cast<u16>(port.y0 + trim_h);
        }
        ResultOnly(request, kResultSuccess);
    }

    void Activate(Request& request) {
        const CameraSet camera_select{request.Pop<u8>()};
        if (!camera_select.IsValid()) {
            MH4N_LOG(Error, kLog, "invalid camera_select={}", camera_select.value);
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        if (camera_select.value == 0) {  // deactivate all
            for (int i = 0; i < 2; ++i) {
                if (Port(i).is_busy) {
                    CancelReceiving(i);
                    Port(i).is_busy = false;
                }
                Port(i).is_active = false;
                UnscheduleVsync(i);
            }
            return ResultOnly(request, kResultSuccess);
        }
        if (camera_select[0] && camera_select[1]) {
            MH4N_LOG(Error, kLog, "camera 0 and 1 can't be both activated");
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        if (camera_select[0]) {
            ActivatePort(0, 0);
        } else if (camera_select[1]) {
            // Azahar calls this twice for the inner camera: two vsync chains
            ActivatePort(0, 1);
            ActivatePort(0, 1);
        }
        if (camera_select[2]) {
            ActivatePort(1, 2);
        }
        ResultOnly(request, kResultSuccess);
    }

    void SwitchContext(Request& request) {
        const CameraSet camera_select{request.Pop<u8>()};
        const ContextSet context_select{request.Pop<u8>()};
        if (!camera_select.IsValid() || !context_select.IsSingle()) {
            MH4N_LOG(Error, kLog, "invalid camera_select={}, context_select={}", camera_select.value, context_select.value);
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        const int context = context_select.Members().front();
        for (const int camera : camera_select.Members()) {
            Camera(camera).current_context = context;
            ApplyContext(camera, Camera(camera).contexts[static_cast<std::size_t>(context)]);
        }
        ResultOnly(request, kResultSuccess);
    }

    // The commands that set one field of a context: the camera, [the value], the context.
    template <class Setter> void SetContextField(Request& request, const CameraSet& camera_select,
                                                 const ContextSet& context_select, Setter setter) {
        if (!camera_select.IsValid() || !context_select.IsValid()) {
            MH4N_LOG(Error, kLog, "invalid camera_select={}, context_select={}", camera_select.value, context_select.value);
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        for (const int index : camera_select.Members()) {
            CameraConfig& camera = Camera(index);
            for (const int context : context_select.Members()) {
                setter(camera.contexts[static_cast<std::size_t>(context)]);
                if (camera.current_context != context) continue;
                ApplyContext(index, camera.contexts[static_cast<std::size_t>(context)]);
            }
        }
        ResultOnly(request, kResultSuccess);
    }

    void FlipImage(Request& request) {
        const CameraSet camera_select{request.Pop<u8>()};
        const u8 flip = request.Pop<u8>();
        const ContextSet context_select{request.Pop<u8>()};
        SetContextField(request, camera_select, context_select, [flip](ContextConfig& c) { c.flip = flip; });
    }

    void SetDetailSize(Request& request) {
        const CameraSet camera_select{request.Pop<u8>()};
        Resolution resolution;
        resolution.width = request.Pop<u16>();
        resolution.height = request.Pop<u16>();
        resolution.crop_x0 = request.Pop<u16>();
        resolution.crop_y0 = request.Pop<u16>();
        resolution.crop_x1 = request.Pop<u16>();
        resolution.crop_y1 = request.Pop<u16>();
        const ContextSet context_select{request.Pop<u8>()};
        SetContextField(request, camera_select, context_select, [resolution](ContextConfig& c) { c.resolution = resolution; });
    }

    void SetSize(Request& request) {
        const CameraSet camera_select{request.Pop<u8>()};
        const u8 size = request.Pop<u8>();
        const ContextSet context_select{request.Pop<u8>()};
        if (size >= kPresetResolution.size()) {
            MH4N_LOG(Error, kLog, "invalid size={}", size);
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        const Resolution resolution = kPresetResolution[size];
        SetContextField(request, camera_select, context_select, [resolution](ContextConfig& c) { c.resolution = resolution; });
    }

    void SetFrameRate(Request& request) {
        const CameraSet camera_select{request.Pop<u8>()};
        const u8 frame_rate = request.Pop<u8>();
        if (!camera_select.IsValid()) {
            MH4N_LOG(Error, kLog, "invalid camera_select={}", camera_select.value);
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        for (const int index : camera_select.Members()) {
            Camera(index).frame_rate = frame_rate;
            Impl(index);
        }
        MH4N_LOG(Warning, kLog, "(STUBBED) called, camera_select={}, frame_rate={}", camera_select.value, frame_rate);
        ResultOnly(request, kResultSuccess);
    }

    void SetEffect(Request& request) {
        const CameraSet camera_select{request.Pop<u8>()};
        const u8 effect = request.Pop<u8>();
        const ContextSet context_select{request.Pop<u8>()};
        SetContextField(request, camera_select, context_select, [effect](ContextConfig& c) { c.effect = effect; });
    }

    void SetOutputFormat(Request& request) {
        const CameraSet camera_select{request.Pop<u8>()};
        const u8 format = request.Pop<u8>();
        const ContextSet context_select{request.Pop<u8>()};
        SetContextField(request, camera_select, context_select, [format](ContextConfig& c) { c.format = format; });
    }

    void SynchronizeVsyncTiming(Request& request) {
        request.Pop<u8>();
        request.Pop<u8>();
        ResultOnly(request, kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    // 0x002A0080: the port, the count. Reply: result, the times (static buffer 0).
    void GetLatestVsyncTiming(Request& request) {
        const PortSet port_select{request.Pop<u8>()};
        const u32 count = request.Pop<u32>();
        Reply& reply = request.MakeReply(1, 2);
        if (!port_select.IsSingle() || count > kMaxVsyncTimings) {
            reply.Push(kResultOutOfRange);
            reply.PushStaticBuffer({}, 0);
            return;
        }
        reply.Push(kResultSuccess);
        const std::size_t port_id = port_select.value == 1 ? 0 : 1;
        std::vector<u8> out(static_cast<std::size_t>(count) * sizeof(s64), 0);
        std::size_t offset = 0;
        for (const s64 timing : m_->ports[port_id].vsync_timings) {
            std::memcpy(out.data() + offset * sizeof(timing), &timing, sizeof(timing));
            ++offset;
            if (offset >= count) break;
        }
        reply.PushStaticBuffer(std::move(out), 0);
    }

    void GetStereoCameraCalibrationData(Request& request) {
        Reply& reply = request.MakeReply(17, 0);
        reply.Push(kResultSuccess);
        for (const u32 word : CalibrationWords()) reply.Push(word);
    }

    // The package parameters: raw structures of 44, 20 and 28 bytes.
    void SetPackageParameterWithoutContext(Request& request) {
        struct Package {
            u8 bytes[44];
        };
        request.PopRaw<Package>();
        ResultOnly(request, kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    Result ApplyPackage(u8 camera_select_value, u8 context_select_value, u8 flip, u8 effect, const Resolution& resolution) {
        const CameraSet camera_select{camera_select_value};
        const ContextSet context_select{context_select_value};
        if (!camera_select.IsValid() || !context_select.IsValid()) {
            MH4N_LOG(Error, kLog, "invalid camera_select={}, context_select={}", camera_select_value, context_select_value);
            return kResultInvalidEnumValue;
        }
        for (const int camera_id : camera_select.Members()) {
            CameraConfig& camera = Camera(camera_id);
            for (const int context_id : context_select.Members()) {
                ContextConfig& context = camera.contexts[static_cast<std::size_t>(context_id)];
                context.effect = effect;
                context.flip = flip;
                context.resolution = resolution;
                if (context_id == camera.current_context) ApplyContext(camera_id, context);
            }
        }
        return kResultSuccess;
    }

    void SetPackageParameterWithContext(Request& request) {
        struct Package {
            u8 camera_select, context_select, flip, effect, size, padding[3];
            u32 padding_words[3];
        };
        static_assert(sizeof(Package) == 20);
        const Package package = request.PopRaw<Package>();
        if (package.size >= kPresetResolution.size()) {
            return ResultOnly(request, kResultInvalidEnumValue);
        }
        ResultOnly(request, ApplyPackage(package.camera_select, package.context_select, package.flip, package.effect,
                                         kPresetResolution[package.size]));
    }

    void SetPackageParameterWithContextDetail(Request& request) {
        struct Package {
            u8 camera_select, context_select, flip, effect;
            Resolution resolution;
            u32 padding_words[3];
        };
        static_assert(sizeof(Package) == 28);
        const Package package = request.PopRaw<Package>();
        ResultOnly(request, ApplyPackage(package.camera_select, package.context_select, package.flip, package.effect,
                                         package.resolution));
    }

    void GetSuitableY2rStandardCoefficient(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void PlayShutterSound(Request& request) {
        const u8 sound_id = request.Pop<u8>();
        ResultOnly(request, kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, sound_id={}", sound_id);
    }

    void DriverInitialize(Request& request) {
        for (int camera_id = 0; camera_id < kNumCameras; ++camera_id) {
            CameraConfig& camera = Camera(camera_id);
            camera.current_context = 0;
            for (int context_id = 0; context_id < 2; ++context_id) {
                // Note: the following default values are verified against real 3DS
                ContextConfig& context = camera.contexts[static_cast<std::size_t>(context_id)];
                context.flip = camera_id == 1 ? 1 : 0;  // Horizontal for the inner camera
                context.effect = 0;
                context.format = 0;  // YUV422
                context.resolution = context_id == 0 ? kPresetResolution[5] : kPresetResolution[0];  // DS_LCD, VGA
            }
            LoadCameraImplementation(camera_id);
        }
        for (PortConfig& port : m_->ports) ClearPort(port);
        m_->initialized = true;
        ResultOnly(request, kResultSuccess);
    }

    void DriverFinalize(Request& request) {
        CancelReceiving(0);
        CancelReceiving(1);
        for (CameraConfig& camera : m_->cameras) camera.impl.exists = false;
        m_->initialized = false;
        ResultOnly(request, kResultSuccess);
    }

    std::shared_ptr<CamModule> m_;
};

MH4N_REGISTER(ServiceFactory, "cam:u",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<CamU>(context); }, 1));

}  // namespace

}  // namespace mh4n::cam
