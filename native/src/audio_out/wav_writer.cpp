#include "audio_out/wav_writer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>

namespace mh4n::audio_out {

namespace {

static_assert(std::endian::native == std::endian::little, "the WAV writer stores samples as they are");

constexpr std::size_t kHeaderSize = 44;

void Put16(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
void Put32(u8* p, u32 v) {
    Put16(p, v & 0xFFFF);
    Put16(p + 2, v >> 16);
}

std::array<u8, kHeaderSize> Header(u32 sample_rate, u16 channels, u64 data_bytes) {
    // The sizes are 32-bit fields: a file past 4 GiB (over 8 hours of this stream) saturates them.
    const u32 data = static_cast<u32>(std::min<u64>(data_bytes, 0xFFFFFFFFull - 36));
    std::array<u8, kHeaderSize> h{};
    std::memcpy(&h[0], "RIFF", 4);
    Put32(&h[4], 36 + data);
    std::memcpy(&h[8], "WAVE", 4);
    std::memcpy(&h[12], "fmt ", 4);
    Put32(&h[16], 16);  // the size of the fmt chunk
    Put16(&h[20], 1);   // PCM
    Put16(&h[22], channels);
    Put32(&h[24], sample_rate);
    Put32(&h[28], sample_rate * channels * 2);  // bytes per second
    Put16(&h[32], static_cast<u32>(channels * 2u));  // block align
    Put16(&h[34], 16);  // bits per sample
    std::memcpy(&h[36], "data", 4);
    Put32(&h[40], data);
    return h;
}

}  // namespace

WavWriter::~WavWriter() {
    Close();
}

bool WavWriter::Open(const std::string& path, u32 sample_rate, u16 channels) {
    Close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        return false;
    }
    sample_rate_ = sample_rate;
    channels_ = channels;
    frames_ = 0;
    ok_ = true;
    const auto header = Header(sample_rate_, channels_, 0);
    if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
        ok_ = false;
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool WavWriter::Write(const s16* samples, std::size_t frames) {
    if (!file_ || !ok_) {
        return false;
    }
    const std::size_t count = frames * channels_;
    if (count != 0 && std::fwrite(samples, sizeof(s16), count, file_) != count) {
        ok_ = false;
        return false;
    }
    frames_ += frames;
    return true;
}

bool WavWriter::Close() {
    if (!file_) {
        return ok_;
    }
    bool ok = ok_;
    const auto header = Header(sample_rate_, channels_, frames_ * channels_ * 2);
    ok = ok && std::fseek(file_, 0, SEEK_SET) == 0 &&
         std::fwrite(header.data(), 1, header.size(), file_) == header.size();
    ok = std::fclose(file_) == 0 && ok;
    file_ = nullptr;
    ok_ = ok;
    return ok;
}

}  // namespace mh4n::audio_out
