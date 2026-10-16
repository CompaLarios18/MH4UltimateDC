// The check interface of the capture player (N2-02). A WP adds a check from its own module:
//
//     class MyCheck : public gpu_capture::CaptureCheck { ... };
//     MH4N_REGISTER(gpu_capture::CheckFactory, "my-check",
//                   [](const Options&) { return std::make_unique<MyCheck>(); });
//
// and the user turns it on with `--check my-check`. The module lists `mh4n_gpu_capture` in its
// DEPENDS. Options of its own are registered in the "capture-player" scope.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu_capture/capture_host.h"
#include "gpu_capture/mgpu.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

namespace mh4n::gpu_capture {

// What a check can reach. It lives for the whole run.
struct CheckContext {
    gpu::Gpu& gpu;           // the outermost Gpu (after the wrappers)
    gpu::GpuDebug* debug;    // nullptr when the Gpu does not implement it
    CaptureHost& host;       // memory of the Gpu, the capture's reference memory
    const Options& options;
};

struct FrameInfo {
    u32 frame;           // the FRAME record's counter
    size_t record_index;
    bool gas;            // the command lists since the last FRAME set fog mode 7 (Azahar draws no gas)
};

// The cells a check adds to a frame's row of the table, one per column it declared.
class FrameColumns {
public:
    void Set(const std::string& column, std::string text) {
        for (auto& cell : cells_) {
            if (cell.first == column) {
                cell.second = std::move(text);
                return;
            }
        }
        cells_.emplace_back(column, std::move(text));
    }
    const std::vector<std::pair<std::string, std::string>>& cells() const {
        return cells_;
    }

private:
    std::vector<std::pair<std::string, std::string>> cells_;
};

// A failure is a non-empty message; it is reported against the record it came from, as
// "[check NAME] message". Returning an empty string passes.
class CaptureCheck {
public:
    virtual ~CaptureCheck() = default;

    // Before the first record. False, with *error, when the Gpu lacks what the check needs (the
    // run then stops with status 2).
    virtual bool Start(CheckContext&, std::string* error) {
        (void)error;
        return true;
    }

    // The names of the columns this check adds to the frame table.
    virtual std::vector<std::string> Columns() const {
        return {};
    }

    // Every record, in file order, after the player has handled it. A command group (a GX or MMIO
    // record and the PICA_REG, GPU_PAGES and IRQ records that follow it) has been executed by the
    // Gpu and judged by the time its first record arrives here, so the Gpu's state is the one
    // after the command.
    virtual std::string OnRecord(size_t index, const Record& record) {
        (void)index;
        (void)record;
        return {};
    }

    // Every FRAME record, after the player judged the screens.
    virtual std::string OnFrame(const FrameInfo& frame, FrameColumns& columns) {
        (void)frame;
        (void)columns;
        return {};
    }

    // After the last record.
    virtual std::string Finish() {
        return {};
    }
};

using CheckFactory = std::function<std::unique_ptr<CaptureCheck>(const Options&)>;

} // namespace mh4n::gpu_capture
