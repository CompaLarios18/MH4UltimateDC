// Derived from Azahar src/audio_core/hle/hle.cpp, src/audio_core/hle/shared_memory.h, src/audio_core/hle/mixers.h
// and src/audio_core/hle/source.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or any later version"): DspHle::Impl::GenerateCurrentFrame and the region rule, which this driver gets by including driver_frame.cpp (the oracle's own copy of them, N3-03) and then plays a frame case as the application would, writing what Azahar's own DSP code did instead of CRC-32s.
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (mixers.h, source.h)
//
// The recorder of N3-04's second oracle (tools/native/dsp_oracle/replay_capture.py): the frame case of
// dsp_oracle.py's driver input (an `fcase` with its `mem` and `ev` lines, up to `end`) is played on Azahar's
// own source, mixer and effect code exactly as driver_frame.cpp plays it, and for every frame the DSP
// generates it appends to the file named by argv[1]
//     the two regions as they are when the frame starts (2 * 0x8000 bytes, region 0 first), after the
//     application's writes and its frame counter, and
//     the frame's 160 stereo s16 samples (640 bytes), as Azahar's mixer made them,
// which is what `--capture-dsp` of the reference runner records at the start and the end of a frame (type 1
// and type 2 records). It prints the number of frames it wrote. The sample reads (type 3) are the
// script's: this driver cannot see inside Azahar's source.cpp, whose memory is a stand-in. Nothing else of
// the frame is printed: no CRC, no value computed here.
//
// driver_frame.cpp is included, not copied: its Dsp (Azahar's frame) lives in an anonymous namespace.
#include "driver_frame.cpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: replay_driver OUT < fcase\n");
        return 2;
    }
    std::FILE* out = std::fopen(argv[1], "wb");
    if (out == nullptr) {
        Die("cannot open the output file");
    }
    std::string header_line;
    if (!std::getline(std::cin, header_line)) {
        Die("no fcase");
    }
    std::istringstream header(header_line);
    std::string word, name;
    u32 frames = 0, first_counter = 0;
    u32 sc, ac, dc, mc, ss, fs, ds;
    header >> word >> name >> frames >> first_counter >> sc >> ac >> dc >> mc >> ss >> fs >> ds;
    if (word != "fcase") {
        Die("the input is not an fcase");
    }
    if (sc != offsetof(HLE::SharedMemory, source_configurations) ||
        ac != offsetof(HLE::SharedMemory, adpcm_coefficients) ||
        dc != offsetof(HLE::SharedMemory, dsp_configuration) ||
        mc != offsetof(HLE::SharedMemory, intermediate_mix_samples) ||
        ss != offsetof(HLE::SharedMemory, source_statuses) ||
        fs != offsetof(HLE::SharedMemory, final_samples) || ds != offsetof(HLE::SharedMemory, dsp_status)) {
        Die("the script's offsets in the shared memory are not Azahar's");
    }

    Dsp dsp;
    std::vector<std::pair<u32, Event>> events;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string op_word;
        in >> op_word;
        if (op_word == "end") {
            break;
        } else if (op_word == "mem") {
            std::string address, hex;
            in >> address >> hex;
            const std::vector<u8> data = Unhex(hex);
            dsp.memory.Place(static_cast<u32>(std::stoul(address, nullptr, 16)), data.data(), data.size());
        } else if (op_word == "ev") {
            std::string op, hex;
            u32 frame = 0;
            Event e{};
            in >> frame >> op >> e.offset >> hex;
            e.op = op[0];
            e.bytes = Unhex(hex);
            events.emplace_back(frame, e);
        } else if (!op_word.empty()) {
            Die("bad line in a frame case");
        }
    }

    const u32 region_base[2] = {HLE::region0_offset, HLE::region1_offset};
    u8* const raw = dsp.dsp_memory->raw_memory.data();
    u32 written = 0;
    for (u32 frame = 0; frame < frames; ++frame) {
        const std::size_t target = (frame + 1) % 2;  // the region the DSP reads in this frame
        u8* const region = raw + region_base[target];
        for (const auto& [at, e] : events) {
            if (at != frame) {
                continue;
            }
            switch (e.op) {
            case 'r':
                if (e.offset + e.bytes.size() > 0x8000) {
                    Die("a write outside the region");
                }
                std::memcpy(region + e.offset, e.bytes.data(), e.bytes.size());
                break;
            case 'o': {
                u32 value;
                std::memcpy(&value, region + e.offset, 4);
                value |= LoadU32(e.bytes, 0);
                std::memcpy(region + e.offset, &value, 4);
                break;
            }
            case 'p': {
                u16 value;
                std::memcpy(&value, region + e.offset, 2);
                value = static_cast<u16>(value | (e.bytes[0] | (e.bytes[1] << 8)));
                std::memcpy(region + e.offset, &value, 2);
                break;
            }
            case 'g': {
                const std::vector<u8> data = GeneratePcm32(LoadU32(e.bytes, 0), LoadU32(e.bytes, 4), LoadU32(e.bytes, 8));
                if (Crc32(data.data(), data.size()) != LoadU32(e.bytes, 12)) {
                    Die("the generator of the samples differs from the script's");
                }
                std::memcpy(region + e.offset, data.data(), data.size());
                break;
            }
            case 'm':
                dsp.memory.Place(e.offset, e.bytes.data(), e.bytes.size());
                break;
            case 'S':
                dsp.Sleep();
                break;
            case 'W':
                dsp.Wakeup();
                break;
            default:
                Die("unknown event");
            }
        }
        const u16 counter = static_cast<u16>(first_counter + frame + 1);
        std::memcpy(region + 0x7FFE, &counter, 2);

        if (!dsp.on) {
            continue;  // a DSP that is asleep makes no frame and records nothing
        }
        if (dsp.CurrentRegionIndex() != target) {
            Die("the frame counters do not pick the region the application wrote");
        }
        // The start of the frame: both regions, as they are.
        std::fwrite(raw + region_base[0], 1, 0x8000, out);
        std::fwrite(raw + region_base[1], 1, 0x8000, out);
        HLE::SharedMemory& write = dsp.WriteRegion();
        dsp.GenerateCurrentFrame();
        std::fwrite(&write.final_samples, 1, sizeof(write.final_samples), out);
        ++written;
    }
    std::fclose(out);
    std::printf("%u\n", written);
    return 0;
}
