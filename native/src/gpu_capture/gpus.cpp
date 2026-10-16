// The two Gpus that ship with the capture player, to test it (N2-02).
//
//   replay  writes what the capture says the GPU wrote (GPU_PAGES), signals the interrupts the
//           capture has and reports the register writes of the capture. It must pass everything.
//   none    does nothing. It must fail at the first draw (and at the first fill or copy).
#include <cstring>
#include <memory>

#include "gpu_capture/capture_host.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"

namespace mh4n::gpu_capture {
namespace {

class ReplayGpu final : public gpu::Gpu, public gpu::GpuDebug {
public:
    explicit ReplayGpu(CaptureHost& host) : host_(host) {}

    void Execute(const gpu::GxCommand&) override {
        Replay();
    }
    void WriteReg(u32, u32) override {
        Replay();
    }
    u32 ReadReg(u32) override {
        return 0;
    }
    void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {
        Replay();
    }
    void SetColorFill(u32) override {
        Replay();
    }
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}

    std::optional<std::vector<gpu::RegisterWrite>> TakeRegisterWrites() override {
        std::vector<gpu::RegisterWrite> taken;
        taken.swap(registers_);
        return taken;
    }

private:
    void Replay() {
        const EventReference* event = host_.CurrentEvent();
        if (event == nullptr) {
            return;
        }
        for (const PageData& page : event->pages) {
            u8* destination = host_.PhysicalPointer(page.address, kPageSize);
            if (destination != nullptr) {
                std::memcpy(destination, page.data.data(), kPageSize);
            }
        }
        registers_.insert(registers_.end(), event->registers.begin(), event->registers.end());
        for (const IrqData& irq : event->interrupts) {
            host_.SignalInterrupt(static_cast<gpu::Interrupt>(irq.id), irq.delay_ns);
        }
    }

    CaptureHost& host_;
    std::vector<gpu::RegisterWrite> registers_;
};

class NoneGpu final : public gpu::Gpu {
public:
    void Execute(const gpu::GxCommand&) override {}
    void WriteReg(u32, u32) override {}
    u32 ReadReg(u32) override {
        return 0;
    }
    void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}
};

} // namespace

MH4N_REGISTER(gpu::GpuFactory, "replay",
              [](gpu::Host& host, const Options&) -> std::unique_ptr<gpu::Gpu> {
                  auto* capture_host = dynamic_cast<CaptureHost*>(&host);
                  if (capture_host == nullptr) {
                      MH4N_LOG(Error, "CapturePlayer", "the replay Gpu needs the capture host");
                      return nullptr;
                  }
                  return std::make_unique<ReplayGpu>(*capture_host);
              });

MH4N_REGISTER(gpu::GpuFactory, "none", [](gpu::Host&, const Options&) -> std::unique_ptr<gpu::Gpu> {
    return std::make_unique<NoneGpu>();
});

} // namespace mh4n::gpu_capture
