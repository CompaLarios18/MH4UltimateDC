// The conversion of a pack into an archive (N2-17).
//
// Convert reads a pack folder (pack.h), converts each texture file once (decode, mip chain,
// BC7; a block-compressed DDS is kept as it is) on worker threads, appends the results to
// `<out>.part` and records each one in `<out>.journal`. When every file is done it writes the
// index, renames the partial file to `<out>` and removes the journal. If the run is interrupted
// (cancelled, killed, out of disk) the next run with the same pack and settings reads the
// journal, cuts the partial file back to the last complete blob and converts only what is left.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "hdpack/bc7.h"
#include "hdpack/pack.h"
#include "mh4n/types.h"

namespace mh4n::hdpack {

struct ConvertProgress {
    u32 done = 0;          // files finished, counting those a previous run finished
    u32 total = 0;         // files to convert
    u32 failed = 0;        // of those, files that could not be decoded
    u64 bytes = 0;         // archive data written so far
    double seconds = 0.0;  // since this run started
    u32 resumed = 0;       // files taken from a previous run
};

struct ConvertOptions {
    std::string pack_dir;
    std::string out_path;
    u32 threads = 0; // 0: the number of hardware threads
    Bc7Quality quality;
    bool restart = false; // ignore the partial work of an earlier run
    // Polled between files; true stops the run (the partial work is kept).
    std::function<bool()> cancel;
    // Called from the calling thread after a file is written, at most once a second and after
    // the last one.
    std::function<void(const ConvertProgress&)> progress;
};

struct ConvertResult {
    enum class Status { Ok, Cancelled, BadPack, IoError };
    Status status = Status::Ok;
    std::string error;
    ConvertProgress progress;
    u32 textures = 0;        // entries in the archive (hash, map)
    u32 dropped_materials = 0; // hashes left out: no colour map, a map that failed, or maps whose
                               // size or format differ
    u64 archive_bytes = 0;
    ScanStats scan;
    std::vector<std::string> failures; // "path: reason", at most the first 50
};

ConvertResult Convert(const ConvertOptions& options);

} // namespace mh4n::hdpack
