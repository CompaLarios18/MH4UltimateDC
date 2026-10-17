#include "hdpack/tool.h"

#include <atomic>
#include <csignal>
#include <ostream>

#include <fmt/format.h>

#include "hdpack/archive.h"
#include "hdpack/bc7.h"
#include "hdpack/convert.h"
#include "mh4n/options.h"

namespace mh4n::hdpack {

MH4N_OPTION("hdpack", "--out", "OUT", "the archive to write (required); -o is the same");
MH4N_OPTION("hdpack", "--threads", "N", "worker threads (default: all hardware threads)");
MH4N_OPTION("hdpack", "--quality", "Q", "BC7 effort: fast, normal (default) or high");
MH4N_OPTION("hdpack", "--restart", "", "ignore the partial work of an interrupted run");
MH4N_OPTION("hdpack", "--verify", "", "after writing, check every blob against its CRC");

namespace {

std::atomic<bool> g_interrupted{false};

extern "C" void OnSignal(int) {
    g_interrupted = true;
}

std::string Mib(u64 bytes) {
    return fmt::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

} // namespace

int RunHdPack(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    // ParseOptions takes "--" options only: -o is --out.
    std::vector<std::string> rewritten;
    for (const std::string& a : args) {
        rewritten.push_back(a == "-o" ? "--out" : a);
    }
    const Options options = ParseOptions("hdpack", rewritten);
    if (!options.ok()) {
        err << "mh4n_hdpack: " << options.error() << "\nusage: mh4n_hdpack PACK_DIR -o OUT [options]\n"
            << Usage("hdpack");
        return 2;
    }
    if (options.positional().size() != 1 || !options.Has("--out")) {
        err << "usage: mh4n_hdpack PACK_DIR -o OUT [options]\n" << Usage("hdpack");
        return 2;
    }
    ConvertOptions convert;
    convert.pack_dir = options.positional()[0];
    convert.out_path = options.GetString("--out");
    try {
        convert.threads = static_cast<u32>(options.GetUInt("--threads", 0));
    } catch (const OptionError& e) {
        err << "mh4n_hdpack: " << e.what() << "\n";
        return 2;
    }
    if (!ParseBc7Quality(options.GetString("--quality", "normal"), convert.quality)) {
        err << "mh4n_hdpack: --quality is fast, normal or high\n";
        return 2;
    }
    convert.restart = options.Has("--restart");
    g_interrupted = false;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    convert.cancel = [] { return g_interrupted.load(); };
    convert.progress = [&](const ConvertProgress& p) {
        const double fraction = p.total ? 100.0 * p.done / p.total : 100.0;
        const u32 new_done = p.done - p.resumed;
        std::string eta;
        if (new_done > 0 && p.done < p.total) {
            eta = fmt::format(", about {:.0f} s left", p.seconds / new_done * (p.total - p.done));
        }
        out << fmt::format("hdpack: {}/{} textures ({:.1f}%), {} written, {} failed, {:.0f} s{}\n",
                           p.done, p.total, fraction, Mib(p.bytes), p.failed, p.seconds, eta)
            << std::flush;
    };

    const ConvertResult result = Convert(convert);
    switch (result.status) {
    case ConvertResult::Status::BadPack:
        err << "mh4n_hdpack: " << result.error << "\n";
        return 2;
    case ConvertResult::Status::IoError:
        err << "mh4n_hdpack: " << result.error << "\n";
        return 3;
    case ConvertResult::Status::Cancelled:
        out << "hdpack: interrupted; run the same command again to continue\n";
        return 4;
    case ConvertResult::Status::Ok:
        break;
    }
    for (const std::string& failure : result.failures) {
        err << "mh4n_hdpack: " << failure << "\n";
    }
    out << fmt::format("hdpack: done: {} textures in {} ({} s); {} files failed, {} hashes left out\n",
                       result.textures, Mib(result.archive_bytes), static_cast<u64>(result.progress.seconds),
                       result.progress.failed, result.dropped_materials);
    if (options.Has("--verify")) {
        ArchiveReader reader;
        std::string error;
        if (!reader.Open(convert.out_path, error)) {
            err << "mh4n_hdpack: " << error << "\n";
            return 3;
        }
        for (const ArchiveEntry& entry : reader.entries()) {
            if (!reader.Verify(entry, error)) {
                err << "mh4n_hdpack: " << error << "\n";
                return 3;
            }
        }
        out << "hdpack: verified " << reader.entries().size() << " entries\n";
    }
    return 0;
}

} // namespace mh4n::hdpack
