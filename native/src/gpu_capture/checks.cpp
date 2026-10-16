// The check that ships with the capture player: --check registers (N2-02).
#include <algorithm>
#include <memory>

#include <fmt/format.h>

#include "gpu_capture/check.h"

namespace mh4n::gpu_capture {
namespace {

u32 ExpandMask(u32 mask) {
    u32 m = 0;
    for (u32 n = 0; n < 4; ++n) {
        if (mask & (1u << n)) {
            m |= 0xFFu << (8 * n);
        }
    }
    return m;
}

// The register writes the Gpu decoded from a command list must be the ones the capture recorded
// for it (PICA_REG): the same registers, in the same order, with the same byte masks and the same
// bytes under the masks.
class RegistersCheck final : public CaptureCheck {
public:
    bool Start(CheckContext& context, std::string* error) override {
        debug_ = context.debug;
        if (debug_ == nullptr) {
            *error = "this Gpu does not implement GpuDebug, so it cannot report register writes";
            return false;
        }
        return true;
    }

    std::vector<std::string> Columns() const override {
        return {"reg_writes"};
    }

    std::string OnRecord(size_t, const Record& record) override {
        if (record.type != kPicaReg) {
            return {};
        }
        std::vector<gpu::RegisterWrite> expected;
        std::string error;
        if (!ParsePicaReg(record, expected, &error)) {
            return error;
        }
        const auto decoded = debug_->TakeRegisterWrites();
        if (!decoded) {
            return "the Gpu does not decode register writes";
        }
        checked_ += expected.size();
        const size_t common = std::min(expected.size(), decoded->size());
        for (size_t i = 0; i < common; ++i) {
            const gpu::RegisterWrite& want = expected[i];
            const gpu::RegisterWrite& got = (*decoded)[i];
            if (want.reg != got.reg || want.mask != got.mask ||
                ((want.value ^ got.value) & ExpandMask(want.mask)) != 0) {
                return fmt::format("write {} of {} differs: the capture has register 0x{:03x} mask "
                                   "0x{:x} value 0x{:08x}, the Gpu 0x{:03x} mask 0x{:x} value 0x{:08x}",
                                   i, expected.size(), want.reg, want.mask, want.value, got.reg,
                                   got.mask, got.value);
            }
        }
        if (expected.size() != decoded->size()) {
            return fmt::format("the capture has {} register writes for this command list, the Gpu "
                               "decoded {}",
                               expected.size(), decoded->size());
        }
        return {};
    }

    std::string OnFrame(const FrameInfo&, FrameColumns& columns) override {
        columns.Set("reg_writes", std::to_string(checked_));
        checked_ = 0;
        return {};
    }

    std::string Finish() override {
        const auto left = debug_->TakeRegisterWrites();
        if (left && !left->empty()) {
            return fmt::format("the Gpu decoded {} register writes the capture does not have",
                               left->size());
        }
        return {};
    }

private:
    gpu::GpuDebug* debug_ = nullptr;
    size_t checked_ = 0;
};

} // namespace

MH4N_REGISTER(CheckFactory, "registers",
              [](const Options&) -> std::unique_ptr<CaptureCheck> {
                  return std::make_unique<RegistersCheck>();
              });

} // namespace mh4n::gpu_capture
