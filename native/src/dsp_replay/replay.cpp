// Derived in part from Azahar src/audio_core/hle/hle.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the rule that picks the region the DSP reads in a frame (DspHle::Impl::CurrentRegionIndex: the higher frame counter, unless one wrapped around), repeated in NextCounter to read the next frame's captured regions.
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
//
// The replay engine of N3-04 (replay.h): everything else in this file is ours.
#include "dsp_replay/replay.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "audio/dsp_layout.h"
#include "dsp_hle/dsp.h"
#include "dsp_replay/capture.h"

namespace mh4n::dsp_replay {

namespace {

using Bytes = std::shared_ptr<const std::vector<u8>>;

// The sparse physical image: pages that have been written; the rest reads as zeros, and a range
// none of whose bytes was ever written reads as an empty span.
class SparseImage final : public dsp_hle::SampleMemory {
public:
    void Write(u32 address, const u8* bytes, u32 size) {
        u64 at = address;
        u32 left = size;
        while (left > 0) {
            const u32 page = static_cast<u32>(at >> kPageBits);
            const u32 offset = static_cast<u32>(at & (kPageSize - 1));
            const u32 n = std::min<u32>(left, kPageSize - offset);
            std::vector<u8>& data = pages_[page];
            if (data.empty()) {
                data.assign(kPageSize, 0);
            }
            std::memcpy(data.data() + offset, bytes, n);
            bytes += n;
            at += n;
            left -= n;
        }
    }

    std::span<const u8> Read(u32 address, u32 size) override {
        if (size == 0) {
            return {};
        }
        const u32 first_page = address >> kPageBits;
        const u32 offset = address & (kPageSize - 1);
        if (offset + static_cast<u64>(size) <= kPageSize) {
            const auto it = pages_.find(first_page);
            if (it == pages_.end()) {
                return {};
            }
            return {it->second.data() + offset, size};
        }
        scratch_.assign(size, 0);
        bool any = false;
        u64 at = address;
        u32 done = 0;
        while (done < size) {
            const u32 page = static_cast<u32>(at >> kPageBits);
            const u32 in_page = static_cast<u32>(at & (kPageSize - 1));
            const u32 n = std::min<u32>(size - done, kPageSize - in_page);
            const auto it = pages_.find(page);
            if (it != pages_.end()) {
                std::memcpy(scratch_.data() + done, it->second.data() + in_page, n);
                any = true;
            }
            at += n;
            done += n;
        }
        if (!any) {
            return {};
        }
        return {scratch_.data(), size};
    }

private:
    static constexpr u32 kPageBits = 12;
    static constexpr u32 kPageSize = 1u << kPageBits;
    std::unordered_map<u32, std::vector<u8>> pages_;
    std::vector<u8> scratch_;
};

struct Read {
    u32 address;
    Bytes bytes;
};

struct Pending {
    bool present = false;
    u32 index = 0;
    std::vector<u8> regions;
    std::vector<u8> pcm;
    std::vector<Read> reads;
};

u32 SwapHalves(u32 v) { return (v << 16) | (v >> 16); }

class Replayer {
public:
    Replayer() : memory_(audio::kDspMemorySize, 0), dsp_(memory_.data()) {
        dsp_.SetSampleMemory(&image_);
        dsp_.SetFrameOutput([this](std::span<const s16> samples) {
            output_.assign(samples.begin(), samples.end());
        });
        dsp_.PipeWrite(static_cast<u32>(audio::DspPipe::Audio), std::array<u8, 4>{0, 0, 0, 0});
    }

    void Add(const Record& record) {
        switch (record.type) {
        case kRegionsRecord:
            Finish(record.payload.data());
            pending_ = {};
            pending_.present = true;
            pending_.index = total_++;
            pending_.regions = record.payload;
            break;
        case kPcmRecord:
            if (pending_.present) {
                pending_.pcm = record.payload;
            }  // a record before the first REGIONS belongs to a frame the capture started in the middle of
            break;
        case kSamplesRecord:
            if (pending_.present) {
                AddRead(record);
            }
            break;
        }
    }

    Verdict Finish() {
        Finish(nullptr);
        if (total_ == 0) {
            throw CaptureError("the capture has no frames");
        }
        return Judge();
    }

private:
    void AddRead(const Record& record) {
        const SampleRead read = ParseSampleRead(record);
        const auto key = std::make_tuple(read.address, read.size, read.crc);
        Bytes bytes;
        if (read.has_bytes) {
            bytes = std::make_shared<const std::vector<u8>>(read.bytes, read.bytes + read.size);
            table_[key] = bytes;
        } else {
            const auto it = table_.find(key);
            if (it == table_.end()) {
                throw CaptureError("a SAMPLES record without bytes names a key (address, size, CRC) "
                                   "that no earlier record gave");
            }
            bytes = it->second;
        }
        pending_.reads.push_back({read.address, std::move(bytes)});
    }

    // Plays the pending frame; `next_regions` is the next frame's captured regions (null at the end).
    void Finish(const u8* next_regions) {
        if (!pending_.present) {
            return;
        }
        if (pending_.pcm.empty()) {
            throw CaptureError("frame " + std::to_string(pending_.index) + " has no PCM record");
        }
        for (const Read& read : pending_.reads) {
            if (!read.bytes->empty()) {
                image_.Write(read.address, read.bytes->data(), static_cast<u32>(read.bytes->size()));
            }
        }
        for (u32 i = 0; i < 2; ++i) {
            std::memcpy(memory_.data() + audio::kRegionOffset[i], pending_.regions.data() + i * audio::kRegionSize,
                        audio::kRegionSize);
        }
        const std::size_t read_region = dsp_.CurrentRegionIndex();
        const std::size_t write_region = read_region == 0 ? 1 : 0;
        const u32 counter = Counter(memory_.data() + audio::kRegionOffset[read_region]);

        // The captured statuses of the region the DSP reads: with our own first output they say which
        // sources are enabled in the first frame.
        const u8* captured_status =
            pending_.regions.data() + read_region * audio::kRegionSize + audio::shm::kSourceStatuses;
        output_.clear();
        if (!dsp_.Tick()) {
            throw CaptureError("our DSP is not running");
        }
        if (output_.size() != 320) {
            throw CaptureError("our DSP made no frame");
        }
        const u8* ours = memory_.data() + audio::kRegionOffset[write_region] + audio::shm::kSourceStatuses;

        // The output against the reference's.
        FrameStat stat;
        stat.index = pending_.index;
        stat.counter = counter;
        for (std::size_t i = 0; i < 320; ++i) {
            s16 ref;
            std::memcpy(&ref, pending_.pcm.data() + i * 2, 2);
            const s64 diff = static_cast<s64>(ref) - static_cast<s64>(output_[i]);
            stat.ref_energy += static_cast<u64>(static_cast<s64>(ref) * ref);
            stat.err_energy += static_cast<u64>(diff * diff);
            stat.max_diff = std::max<u32>(stat.max_diff, static_cast<u32>(std::llabs(diff)));
        }
        stats_.push_back(stat);
        counters_.push_back(counter);

        // The statuses we wrote against the next frame's captured ones (the region we wrote is the one
        // the next frame reads, when the counters are consecutive).
        u8 status = 2;  // not compared
        if (next_regions != nullptr) {
            const u32 next_counter = NextCounter(next_regions);
            if (next_counter == ((counter + 1) & 0xFFFF)) {
                const u8* theirs = next_regions + write_region * audio::kRegionSize + audio::shm::kSourceStatuses;
                status = std::memcmp(ours, theirs, audio::kNumSources * 12) == 0 ? 0 : 1;
            }
        }
        status_.push_back(status);

        Settle(pending_.index == 0, captured_status, ours);
    }

    static u32 Counter(const u8* region) {
        u16 value;
        std::memcpy(&value, region + audio::shm::kFrameCounter, 2);
        return value;
    }
    // The counter of the region the DSP reads in a frame whose regions are `regions`.
    static u32 NextCounter(const u8* regions) {
        const u32 c0 = Counter(regions);
        const u32 c1 = Counter(regions + audio::kRegionSize);
        std::size_t index;
        if (c0 == 0xFFFF && c1 != 0xFFFE) {
            index = 1;
        } else if (c1 == 0xFFFF && c0 != 0xFFFE) {
            index = 0;
        } else {
            index = c0 > c1 ? 0 : 1;
        }
        return index == 0 ? c0 : c1;
    }

    // Counts, on our statuses, the dequeues, loops and stops of the sources enabled in the first frame.
    void Settle(bool first, const u8* captured_status, const u8* ours) {
        const u32 index = stats_.back().index;
        for (u32 s = 0; s < audio::kNumSources; ++s) {
            const u8* block = ours + s * 12;
            const bool enabled = block[0] != 0;
            if (first) {
                tracked_[s] = enabled || captured_status[s * 12] != 0;
                captured_enabled_[s] = captured_status[s * 12] != 0;
            }
            ever_enabled_[s] = ever_enabled_[s] || enabled;
            Source& src = sources_[s];
            const u32 position = SwapHalves([&] {
                u32 v;
                std::memcpy(&v, block + 4, 4);
                return v;
            }());
            u16 buffer_id;
            std::memcpy(&buffer_id, block + 8, 2);
            if (tracked_[s] && !src.settled) {
                if (!enabled) {
                    src.settled = true;  // stopped: disabled, or no buffer left
                } else {
                    if (block[1] != 0) {
                        ++src.dequeues;
                    }
                    const bool looped = src.have_previous && block[1] == 0 && buffer_id == src.previous_id &&
                                        position < src.previous_position;
                    if (src.dequeues >= 2 || looped) {
                        src.settled = true;
                    }
                }
                if (src.settled) {
                    src.settled_at = index;
                }
            }
            src.have_previous = true;
            src.previous_position = position;
            src.previous_id = buffer_id;
        }
    }

    Verdict Judge() {
        Verdict r;
        r.total_frames = total_;
        r.settled = true;
        u32 start = 0;
        for (u32 s = 0; s < audio::kNumSources; ++s) {
            if (captured_enabled_[s] && !ever_enabled_[s]) {
                r.never_started.push_back(s);
            }
            if (!tracked_[s]) {
                continue;
            }
            if (!sources_[s].settled) {
                r.settled = false;
                r.unsettled_sources.push_back(s);
            } else {
                start = std::max(start, sources_[s].settled_at);
            }
        }
        if (r.settled) {
            r.first_judged_index = start;
            r.first_judged_counter = counters_[start];
            r.judged = total_ - start;
        }
        const bool enough = static_cast<u64>(r.judged) * 100 >= static_cast<u64>(total_) * kJudgedSharePercent;

        std::vector<FrameStat> judged_stats(stats_.begin() + (r.settled ? start : total_), stats_.end());
        for (const FrameStat& f : judged_stats) {
            if (f.max_diff == 0) {
                ++r.bit_exact;
            } else {
                r.not_exact.push_back({f.index, f.counter, f.max_diff});
            }
            const u8 st = status_[f.index];
            if (st != 2) {
                ++r.status_frames_compared;
            }
            if (st == 1 && r.status_equal) {
                r.status_equal = false;
                r.first_status_difference_counter = f.counter;
                r.first_status_difference_index = f.index;
            }
        }
        bool snr_ok = true;
        bool windows_ok = true;
        if (r.judged > 0) {
            r.whole = JudgeSpan(judged_stats, 0, r.judged);
            snr_ok = r.whole.pass;
            r.windows = MakeWindows(r.judged);
            double worst = 0;
            bool worst_failed = false;
            for (const Window& w : r.windows) {
                const Span span = JudgeSpan(judged_stats, w.first, w.count);
                r.window_spans.push_back(span);
                windows_ok = windows_ok && span.pass;
                const bool candidate = !span.pass || !span.low_rms;
                if (candidate && (!r.has_worst || (!span.pass && !worst_failed) ||
                                  (span.pass == !worst_failed && span.db < worst))) {
                    worst = span.db;
                    worst_failed = !span.pass;
                    r.has_worst = true;
                    r.worst_window_counter = judged_stats[w.first].counter;
                }
            }
            r.worst_window_db = worst;
            if (!r.has_worst) {  // every window was silent and passed
                r.has_worst = true;
                r.worst_window_db = SnrDb(1, 0);
                r.worst_window_counter = judged_stats[0].counter;
            }
        }
        std::vector<std::string> why;
        if (!r.settled || !enough) {
            why.push_back("not settled");
        } else {
            if (!snr_ok) {
                why.push_back("snr");
            }
            if (!windows_ok) {
                why.push_back("window");
            }
            if (!r.status_equal) {
                why.push_back("status");
            }
        }
        r.pass = why.empty();
        for (std::size_t i = 0; i < why.size(); ++i) {
            r.fail_reason += (i ? ", " : "") + why[i];
        }
        return r;
    }

    struct Source {
        bool settled = false;
        u32 settled_at = 0;
        u32 dequeues = 0;
        bool have_previous = false;
        u32 previous_position = 0;
        u16 previous_id = 0;
    };

    std::vector<u8> memory_;
    dsp_hle::Dsp dsp_;
    SparseImage image_;
    std::map<std::tuple<u32, u32, u32>, Bytes> table_;
    Pending pending_;
    std::vector<s16> output_;
    u32 total_ = 0;
    std::vector<FrameStat> stats_;
    std::vector<u32> counters_;
    std::vector<u8> status_;  // 0 equal, 1 different, 2 not compared
    std::array<bool, audio::kNumSources> tracked_{};
    std::array<bool, audio::kNumSources> captured_enabled_{};
    std::array<bool, audio::kNumSources> ever_enabled_{};
    std::array<Source, audio::kNumSources> sources_{};
};

}  // namespace

Verdict Replay(std::istream& capture) {
    CaptureReader reader(capture);
    Replayer replayer;
    Record record;
    while (reader.Next(record)) {
        replayer.Add(record);
    }
    return replayer.Finish();
}

std::string SummaryLine(const Verdict& r) {
    std::ostringstream out;
    out << "dsp-replay: " << r.judged << " frames judged of " << r.total_frames << ", " << r.bit_exact
        << " bit-exact, ";
    if (r.judged > 0) {
        out << "snr " << FormatDb(r.whole.db) << ", worst window " << FormatDb(r.worst_window_db) << " at "
            << r.worst_window_counter;
    } else {
        out << "snr n/a, worst window n/a at -";
    }
    out << ", status ";
    if (r.status_equal) {
        out << "equal";
    } else {
        out << "first difference at frame " << r.first_status_difference_counter;
    }
    out << ", " << (r.pass ? "PASS" : "FAIL");
    return out.str();
}

void WriteDetails(const Verdict& r, std::ostream& out) {
    if (r.settled) {
        out << "dsp-replay: settled at frame " << r.first_judged_counter << " (record " << r.first_judged_index
            << "): " << r.first_judged_index << " frames reported, not judged; " << r.status_frames_compared
            << " judged frames had a next frame to compare the status blocks with\n";
    } else {
        out << "dsp-replay: not settled: source";
        for (u32 s : r.unsettled_sources) {
            out << " " << s;
        }
        out << " never settled\n";
    }
    if (!r.never_started.empty()) {
        out << "dsp-replay: sources enabled in the first captured status that our DSP never started:";
        for (u32 s : r.never_started) {
            out << " " << s;
        }
        out << "\n";
    }
    if (!r.pass) {
        out << "dsp-replay: why: " << r.fail_reason << "\n";
    }
    for (std::size_t i = 0; i < r.window_spans.size(); ++i) {
        const Span& s = r.window_spans[i];
        out << "dsp-replay: window " << i << ": " << s.frames << " frames, snr " << FormatDb(s.db)
            << (s.low_rms ? " (reference RMS below 1: judged by the largest difference " : " (largest difference ")
            << s.max_diff << "), " << (s.pass ? "ok" : "BELOW THE LIMIT") << "\n";
    }
    for (const Listed& f : r.not_exact) {
        out << "dsp-replay: not bit-exact: frame " << f.counter << " (record " << f.index
            << "), largest difference " << f.max_diff << "\n";
    }
}

}  // namespace mh4n::dsp_replay
