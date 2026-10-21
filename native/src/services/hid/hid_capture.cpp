// --capture-hid FILE (N1-16): the HID capture of N0-06 in the MHID format of tools/native/capfmt.py
// (its docstring defines the records), written as the reference runner (port/src/runner.cpp,
// WriteHidCapture) writes it, so that `capfmt.py diff-hid` compares the two runs:
//  - on every change of the game's frame counter (Runtime::OnFrame, the first reading included), one
//    record of type 1: the first 0x2b0 bytes of the HID shared memory, or zeros until the game has
//    mapped it (the reference reads the memory through the game's mapping);
//  - after it, when a game has handed ir:USER a shared memory block, one record of type 2: the first
//    bytes of the first block it handed in, at most 0x200, if it holds at least the 16-byte header;
//  - the record's frame is the counter, its ticks the game clock's since the capture started;
//  - FILE.json is the manifest: tool, format, version, game (the title id), frames (the type 1
//    records) and the record counts.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "hid_state.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "services/ir/ir_state.h"

namespace mh4n::hid {

namespace {

constexpr const char* kLog = "HID.Capture";
constexpr u32 kRecordHid = 1;
constexpr u32 kRecordIr = 2;
constexpr u32 kIrMaxBytes = 0x200;

MH4N_OPTION("runner", "--capture-hid", "FILE",
            "write the HID shared memory (and ir:USER's, once a game sets it up) on every game frame "
            "(MHID, tools/native/capfmt.py)");

class Capture {
public:
    Capture(std::string path, u64 start_tick, u64 game, std::shared_ptr<Shared> hid,
            std::shared_ptr<ir::Shared> ir)
        : path_(std::move(path)),
          start_tick_(start_tick),
          game_(game),
          hid_(std::move(hid)),
          ir_(std::move(ir)) {
        file_ = std::fopen(path_.c_str(), "wb");
        if (file_ != nullptr) {
            const u32 version = 1;
            std::fwrite("MHID", 1, 4, file_);
            std::fwrite(&version, 4, 1, file_);
        }
    }
    ~Capture() { Close(); }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    bool ok() const { return file_ != nullptr; }

    void OnFrame(u32 frame, u64 tick) {
        if (file_ == nullptr) {
            return;
        }
        const u64 ticks = tick - start_tick_;
        std::array<u8, kReadableSize> block{};  // zeros until the game has mapped the memory
        if (hid_->Memory() && hid_->Memory()->IsMapped()) {
            std::memcpy(block.data(), hid_->Memory()->GetPointer(), block.size());
        }
        Record(kRecordHid, frame, ticks, block.data(), static_cast<u32>(block.size()));
        if (ir_->capture_memory) {
            const u32 size = std::min<u32>(ir_->capture_memory->Size(), kIrMaxBytes);
            if (size >= sizeof(ir::SharedMemoryHeader)) {
                Record(kRecordIr, frame, ticks, ir_->capture_memory->GetPointer(), size);
            }
        }
        ++frames_;
    }

    void Close() {
        if (file_ == nullptr) {
            return;
        }
        if (std::fclose(file_) != 0) {
            MH4N_LOG(Error, kLog, "cannot finish {}", path_);
        }
        file_ = nullptr;
        if (std::FILE* manifest = std::fopen((path_ + ".json").c_str(), "w")) {
            std::fprintf(manifest,
                         "{\n  \"tool\": \"mh4u_native_runner\",\n  \"format\": \"MHID\",\n"
                         "  \"version\": 1,\n  \"game\": \"0x%016llx\",\n  \"frames\": %u,\n"
                         "  \"records\": {\"1\": %u, \"2\": %u, \"3\": %u}\n}\n",
                         static_cast<unsigned long long>(game_), frames_, counts_[1], counts_[2],
                         counts_[3]);
            std::fclose(manifest);
        } else {
            MH4N_LOG(Error, kLog, "cannot write {}.json", path_);
        }
    }

private:
    void Record(u32 type, u32 frame, u64 ticks, const void* data, u32 size) {
        std::fwrite(&type, 4, 1, file_);
        std::fwrite(&frame, 4, 1, file_);
        std::fwrite(&ticks, 8, 1, file_);
        std::fwrite(&size, 4, 1, file_);
        if (size != 0) {
            std::fwrite(data, 1, size, file_);
        }
        ++counts_[type];
    }

    std::string path_;
    u64 start_tick_;
    u64 game_;
    std::shared_ptr<Shared> hid_;
    std::shared_ptr<ir::Shared> ir_;
    std::FILE* file_ = nullptr;
    u32 frames_ = 0;
    std::array<u32, 4> counts_{};
};

void StartHidCapture(Runtime& rt) {
    const auto path = rt.GetOptions().Get("--capture-hid");
    if (!path) {
        return;
    }
    ServiceManager* manager = FindServiceManager(rt);
    if (manager == nullptr) {
        MH4N_LOG(Error, kLog, "--capture-hid needs the services phase");
        rt.RequestExit(2, "--capture-hid needs the services phase");
        return;
    }
    const ProcessImage* image = rt.Image();
    auto capture = std::make_shared<Capture>(*path, rt.Clock().Now(), image ? image->program_id : 0,
                                             GetShared(manager->Context()),
                                             ir::GetShared(manager->Context()));
    if (!capture->ok()) {
        MH4N_LOG(Error, kLog, "cannot write {}", *path);
        rt.RequestExit(2, "cannot write " + *path);
        return;
    }
    std::weak_ptr<Capture> weak = capture;
    rt.OnFrame([weak](u32 frame, u64 tick) {
        if (const auto self = weak.lock()) {
            self->OnFrame(frame, tick);
        }
    });
    rt.OnFinish([weak] {
        if (const auto self = weak.lock()) {
            self->Close();
        }
    });
    rt.Own(capture);
}

MH4N_REGISTER(BootStep, "tracing.hid", BootStep{Phase::Tracing, StartHidCapture});

}  // namespace

}  // namespace mh4n::hid
