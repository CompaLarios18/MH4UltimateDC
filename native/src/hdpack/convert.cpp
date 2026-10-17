#include "hdpack/convert.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <fmt/format.h>

#include "hdpack/archive.h"
#include "hdpack/bytes.h"
#include "hdpack/image.h"
#include "hdpack/mips.h"
#include "mh4n/crc32.h"
#include "mh4n/log.h"

namespace mh4n::hdpack {

namespace fs = std::filesystem;

namespace {

// What one converted file left behind (the journal's record).
struct Record {
    std::string path;
    u64 size = 0;
    s64 mtime = 0;
    bool ok = false;
    PixelFormat format = PixelFormat::BC7;
    u8 mips = 0;
    u32 width = 0;
    u32 height = 0;
    u64 offset = 0;
    u64 blob_size = 0;
    u32 crc = 0;
};

constexpr char kJournalMagic[8] = {'M', 'H', '4', 'H', 'D', 'J', '0', '1'};
constexpr std::size_t kJournalHeader = 12;

u32 Fingerprint(const PackOptions& options, const Bc7Quality& quality) {
    const std::string text = fmt::format("v1 new={} flip={} uber={} parts={}", options.use_new_hash,
                                         options.flip_png_files, quality.uber_level,
                                         quality.max_partitions);
    return Crc32(text.data(), text.size());
}

std::vector<u8> EncodeRecord(const Record& r) {
    std::vector<u8> out(4 + r.path.size() + 8 + 8 + 4 + 4 + 4 + 8 + 8 + 4 + 4);
    u8* p = out.data();
    Put32(p, static_cast<u32>(r.path.size()));
    p += 4;
    std::copy(r.path.begin(), r.path.end(), p);
    p += r.path.size();
    Put64(p, r.size);
    Put64(p + 8, static_cast<u64>(r.mtime));
    p += 16;
    p[0] = r.ok ? 1 : 0;
    p[1] = static_cast<u8>(r.format);
    p[2] = r.mips;
    p[3] = 0;
    p += 4;
    Put32(p, r.width);
    Put32(p + 4, r.height);
    Put64(p + 8, r.offset);
    Put64(p + 16, r.blob_size);
    Put32(p + 24, r.crc);
    p += 28;
    Put32(p, Crc32(out.data(), static_cast<std::size_t>(p - out.data())));
    return out;
}

// Reads the records of a journal up to the first damaged one; returns the bytes they cover.
u64 ReadJournal(const std::string& path, u32 fingerprint, std::map<std::string, Record>& records) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return 0;
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < kJournalHeader || std::equal(kJournalMagic, kJournalMagic + 8, bytes.begin()) == false ||
        Get32(bytes.data() + 8) != fingerprint) {
        return 0;
    }
    std::size_t pos = kJournalHeader;
    for (;;) {
        if (pos + 4 > bytes.size()) {
            break;
        }
        const u32 path_len = Get32(&bytes[pos]);
        const std::size_t total = 4 + static_cast<std::size_t>(path_len) + 16 + 4 + 28 + 4;
        if (path_len > 4096 || pos + total > bytes.size()) {
            break;
        }
        const u8* p = &bytes[pos];
        if (Get32(p + total - 4) != Crc32(p, total - 4)) {
            break;
        }
        Record r;
        r.path.assign(reinterpret_cast<const char*>(p + 4), path_len);
        p += 4 + path_len;
        r.size = Get64(p);
        r.mtime = static_cast<s64>(Get64(p + 8));
        p += 16;
        r.ok = p[0] != 0;
        r.format = static_cast<PixelFormat>(p[1]);
        r.mips = p[2];
        p += 4;
        r.width = Get32(p);
        r.height = Get32(p + 4);
        r.offset = Get64(p + 8);
        r.blob_size = Get64(p + 16);
        r.crc = Get32(p + 24);
        records[r.path] = r;
        pos += total;
    }
    return pos;
}

struct Converted {
    Record record;
    std::vector<u8> blob;
    std::string failure;
};

bool ReadWhole(const fs::path& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

Converted ConvertFile(const std::string& dir, const PackFile& file, const PackOptions& options,
                      const Bc7Quality& quality) {
    Converted result;
    Record& r = result.record;
    r.path = file.path;
    r.size = file.size;
    r.mtime = file.mtime;
    auto fail = [&](const std::string& why) {
        result.failure = file.path + ": " + why;
        r.ok = false;
        return std::move(result);
    };
    std::vector<u8> bytes;
    if (!ReadWhole(fs::path(dir) / file.path, bytes)) {
        return fail("cannot read the file");
    }
    Image image;
    std::string error;
    switch (file.format) {
    case FileFormat::Png:
        if (!DecodePng(bytes.data(), bytes.size(), options.flip_png_files, image, error)) {
            return fail(error);
        }
        break;
    case FileFormat::Dds:
        if (!DecodeDds(bytes.data(), bytes.size(), image, error)) {
            return fail(error);
        }
        break;
    default:
        return fail("KTX files are not supported");
    }
    bytes.clear();
    bytes.shrink_to_fit();

    r.width = image.width;
    r.height = image.height;
    if (image.format == PixelFormat::RGBA8) {
        const auto chain = BuildMipChain(image.levels[0].data(), image.width, image.height,
                                         file.type == MapType::Color);
        r.format = PixelFormat::BC7;
        r.mips = static_cast<u8>(chain.size());
        u32 w = image.width;
        u32 h = image.height;
        for (const auto& level : chain) {
            const std::size_t at = result.blob.size();
            result.blob.resize(at + LevelBytes(PixelFormat::BC7, w, h));
            EncodeBc7(level.data(), w, h, quality, result.blob.data() + at);
            w = std::max(1u, w >> 1);
            h = std::max(1u, h >> 1);
        }
    } else {
        r.format = image.format;
        r.mips = static_cast<u8>(image.levels.size());
        for (const auto& level : image.levels) {
            result.blob.insert(result.blob.end(), level.begin(), level.end());
        }
    }
    r.blob_size = result.blob.size();
    r.crc = Crc32(result.blob.data(), result.blob.size());
    r.ok = true;
    return result;
}

} // namespace

ConvertResult Convert(const ConvertOptions& options) {
    ConvertResult result;
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double>(Clock::now() - started).count();
    };
    auto fail = [&](ConvertResult::Status status, std::string message) {
        result.status = status;
        result.error = std::move(message);
        return result;
    };

    std::string error;
    std::optional<Pack> scanned = ScanPack(options.pack_dir, &error);
    if (!scanned) {
        return fail(ConvertResult::Status::BadPack, error);
    }
    const Pack& pack = *scanned;
    result.scan = pack.stats;

    // The files that some material uses.
    std::vector<int> work;
    {
        std::set<int> used;
        for (const auto& [hash, material] : pack.materials) {
            for (const int index : material.file) {
                if (index >= 0) {
                    used.insert(index);
                }
            }
        }
        work.assign(used.begin(), used.end());
    }

    const std::string part_path = options.out_path + ".part";
    const std::string journal_path = options.out_path + ".journal";
    const u32 fingerprint = Fingerprint(pack.options, options.quality);

    // Take over what an earlier run finished.
    std::map<std::string, Record> records;
    u64 resume_size = 0;
    u64 journal_valid = 0;
    std::error_code ec;
    if (!options.restart && fs::exists(part_path, ec)) {
        journal_valid = ReadJournal(journal_path, fingerprint, records);
        for (const auto& [path, r] : records) {
            if (r.ok) {
                resume_size = std::max(resume_size, r.offset + r.blob_size);
            }
        }
        if (journal_valid > 0) {
            resume_size = std::max<u64>(resume_size, kArchiveHeaderSize);
        }
    }
    ArchiveWriter writer;
    if (resume_size == 0) {
        records.clear();
        journal_valid = 0;
    }
    if (!writer.Begin(part_path, pack.options.use_new_hash, resume_size, error)) {
        return fail(ConvertResult::Status::IoError, error);
    }
    std::fstream journal;
    {
        if (journal_valid == 0) {
            journal.open(journal_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
            char head[kJournalHeader];
            std::copy(kJournalMagic, kJournalMagic + 8, head);
            Put32(reinterpret_cast<u8*>(head) + 8, fingerprint);
            journal.write(head, sizeof(head));
        } else {
            fs::resize_file(journal_path, journal_valid, ec);
            journal.open(journal_path, std::ios::binary | std::ios::in | std::ios::out);
            journal.seekp(static_cast<std::streamoff>(journal_valid));
        }
        journal.flush();
        if (!journal) {
            return fail(ConvertResult::Status::IoError, journal_path + ": cannot write");
        }
    }

    // Records that still describe the files as they are now.
    std::map<std::string, Record> done;
    std::vector<int> pending;
    for (const int index : work) {
        const PackFile& file = pack.files[static_cast<std::size_t>(index)];
        auto it = records.find(file.path);
        if (it != records.end() && it->second.size == file.size && it->second.mtime == file.mtime &&
            (it->second.ok ? it->second.offset + it->second.blob_size <= resume_size : true)) {
            done[file.path] = it->second;
        } else {
            pending.push_back(index);
        }
    }

    ConvertProgress progress;
    progress.total = static_cast<u32>(work.size());
    progress.done = static_cast<u32>(done.size());
    progress.resumed = progress.done;
    for (const auto& [path, r] : done) {
        progress.failed += r.ok ? 0 : 1;
    }
    progress.bytes = writer.size();
    Clock::time_point last_report{};
    auto report = [&](bool last) {
        const auto now = Clock::now();
        if (!last && now - last_report < std::chrono::seconds(1)) {
            return;
        }
        last_report = now;
        progress.seconds = elapsed();
        progress.bytes = writer.size();
        if (options.progress) {
            options.progress(progress);
        }
    };

    // Workers convert; this thread writes, so the file and the journal have one writer.
    const u32 threads = std::max(1u, options.threads != 0 ? options.threads
                                                           : std::thread::hardware_concurrency());
    std::mutex mutex;
    std::condition_variable results_ready;
    std::condition_variable space_free;
    std::deque<Converted> queue;
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stop{false};
    const std::size_t queue_limit = static_cast<std::size_t>(threads) * 2;
    u32 running = threads;

    auto worker = [&] {
        for (;;) {
            if (stop.load() || (options.cancel && options.cancel())) {
                stop = true;
                break;
            }
            const std::size_t i = next.fetch_add(1);
            if (i >= pending.size()) {
                break;
            }
            Converted converted = ConvertFile(options.pack_dir,
                                              pack.files[static_cast<std::size_t>(pending[i])],
                                              pack.options, options.quality);
            std::unique_lock lock(mutex);
            space_free.wait(lock, [&] { return queue.size() < queue_limit || stop.load(); });
            if (stop.load()) {
                break;
            }
            queue.push_back(std::move(converted));
            results_ready.notify_one();
        }
        std::lock_guard lock(mutex);
        --running;
        results_ready.notify_one();
    };
    std::vector<std::thread> pool;
    for (u32 i = 0; i < threads && !pending.empty(); ++i) {
        pool.emplace_back(worker);
    }
    if (pending.empty()) {
        running = 0;
    }

    bool io_failed = false;
    for (;;) {
        Converted item;
        {
            std::unique_lock lock(mutex);
            results_ready.wait(lock, [&] { return !queue.empty() || running == 0; });
            if (queue.empty()) {
                break;
            }
            item = std::move(queue.front());
            queue.pop_front();
            space_free.notify_one();
        }
        if (io_failed) {
            continue;
        }
        Record& r = item.record;
        if (r.ok) {
            const std::optional<u64> offset = writer.Append(item.blob.data(), item.blob.size(), error);
            if (!offset || !writer.Flush(error)) {
                io_failed = true;
                stop = true;
                space_free.notify_all();
                continue;
            }
            r.offset = *offset;
        } else {
            ++progress.failed;
            if (result.failures.size() < 50) {
                result.failures.push_back(item.failure);
            }
            MH4N_LOG(Warning, "HdPack", "{}", item.failure);
        }
        const std::vector<u8> bytes = EncodeRecord(r);
        journal.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        journal.flush();
        if (!journal) {
            error = journal_path + ": write failed (disk full?)";
            io_failed = true;
            stop = true;
            space_free.notify_all();
            continue;
        }
        done[r.path] = r;
        ++progress.done;
        report(false);
    }
    for (std::thread& t : pool) {
        t.join();
    }
    report(true);
    result.progress = progress;
    result.failures.shrink_to_fit();

    if (io_failed) {
        return fail(ConvertResult::Status::IoError, error);
    }
    if (done.size() < work.size()) {
        result.status = ConvertResult::Status::Cancelled; // stopped before the last file
        return result;
    }

    // The index: one entry per (hash, map); the maps of a material must agree.
    std::vector<ArchiveEntry> entries;
    for (const auto& [hash, material] : pack.materials) {
        const Record* maps[kMapTypeCount] = {nullptr, nullptr};
        for (std::size_t t = 0; t < kMapTypeCount; ++t) {
            if (material.file[t] >= 0) {
                const Record& r = done.at(pack.files[static_cast<std::size_t>(material.file[t])].path);
                maps[t] = r.ok ? &r : nullptr;
            }
        }
        bool drop = maps[0] == nullptr; // Azahar: no colour map, no material
        for (std::size_t t = 0; t < kMapTypeCount; ++t) {
            if (material.file[t] >= 0 && maps[t] == nullptr) {
                drop = true; // a map that failed to load fails the material
            }
            if (maps[t] && maps[0] &&
                (maps[t]->width != maps[0]->width || maps[t]->height != maps[0]->height ||
                 maps[t]->format != maps[0]->format)) {
                drop = true;
            }
        }
        if (drop) {
            ++result.dropped_materials;
            continue;
        }
        for (std::size_t t = 0; t < kMapTypeCount; ++t) {
            if (!maps[t]) {
                continue;
            }
            ArchiveEntry e;
            e.hash = hash;
            e.offset = maps[t]->offset;
            e.size = maps[t]->blob_size;
            e.width = maps[t]->width;
            e.height = maps[t]->height;
            e.crc = maps[t]->crc;
            e.type = static_cast<MapType>(t);
            e.format = maps[t]->format;
            e.mip_count = maps[t]->mips;
            entries.push_back(e);
        }
    }
    result.textures = static_cast<u32>(entries.size());
    if (!writer.Finish(std::move(entries), error)) {
        return fail(ConvertResult::Status::IoError, error);
    }
    journal.close();
    fs::remove(options.out_path, ec); // rename does not replace a file on Windows
    fs::rename(part_path, options.out_path, ec);
    if (ec) {
        return fail(ConvertResult::Status::IoError, options.out_path + ": " + ec.message());
    }
    fs::remove(journal_path, ec);
    result.archive_bytes = fs::file_size(options.out_path, ec);
    return result;
}

} // namespace mh4n::hdpack
