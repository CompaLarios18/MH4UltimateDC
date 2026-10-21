#include "audio/mdsp_writer.h"

#include <cinttypes>
#include <utility>

#include "mh4n/log.h"

namespace mh4n::audio {

namespace {
constexpr const char* kLog = "Audio.DSP";
}

bool MdspWriter::Open(const std::string& path, u64 program_id, std::string tool) {
    Close();
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) {
        MH4N_LOG(Error, kLog, "cannot write {}", path);
        return false;
    }
    path_ = path;
    tool_ = std::move(tool);
    program_id_ = program_id;
    frames_ = 0;
    failed_ = false;
    // The format is little-endian; so is every host this runs on (ipc.h asserts it).
    const u32 version = kMdspVersion;
    if (std::fwrite("MDSP", 1, 4, file_) != 4 || std::fwrite(&version, 4, 1, file_) != 1) {
        MH4N_LOG(Error, kLog, "cannot write {}", path);
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool MdspWriter::AddRegions(u32 frame, u64 ticks, const u8* region0, const u8* region1,
                            u32 region_size) {
    if (file_ == nullptr) {
        return false;
    }
    bool ok = std::fwrite(&kMdspRegions, 4, 1, file_) == 1 && std::fwrite(&frame, 4, 1, file_) == 1 &&
              std::fwrite(&ticks, 8, 1, file_) == 1;
    const u32 size = 2 * region_size;
    ok = ok && std::fwrite(&size, 4, 1, file_) == 1 &&
         std::fwrite(region0, 1, region_size, file_) == region_size &&
         std::fwrite(region1, 1, region_size, file_) == region_size;
    if (!ok) {
        failed_ = true;
        return false;
    }
    ++frames_;
    return true;
}

void MdspWriter::Close() {
    if (file_ == nullptr) {
        return;
    }
    if (std::fclose(file_) != 0) {
        failed_ = true;
    }
    file_ = nullptr;
    if (failed_) {
        MH4N_LOG(Error, kLog, "writing {} failed; the capture is incomplete", path_);
    }
    if (std::FILE* manifest = std::fopen((path_ + ".json").c_str(), "w")) {
        std::fprintf(manifest,
                     "{\n  \"tool\": \"%s\",\n  \"format\": \"MDSP\",\n  \"version\": %u,\n"
                     "  \"game\": \"0x%016" PRIx64 "\",\n  \"frames\": %u,\n"
                     "  \"records\": {\"1\": %u, \"2\": 0, \"3\": 0}\n}\n",
                     tool_.c_str(), kMdspVersion, program_id_, frames_, frames_);
        std::fclose(manifest);
    } else {
        MH4N_LOG(Error, kLog, "cannot write {}.json", path_);
    }
}

}  // namespace mh4n::audio
