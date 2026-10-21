// The options of the file system services (N1-12).
#include "fs_state.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::fs {

MH4N_OPTION("runner", "--io-latency", "azahar|none",
            "when file replies arrive: no earlier than Azahar's modelled delay, on the game clock "
            "(azahar, the default), or as soon as the data is read (none)");
MH4N_OPTION("ipc-replay", "--io-latency", "azahar|none",
            "when file replies arrive: no earlier than Azahar's modelled delay (azahar, the default), or "
            "as soon as the data is read (none)");

namespace {

// A value that is neither is refused before the program runs, with the exit status of a bad option.
void CheckIoLatency(Runtime& runtime) {
    const std::string value = runtime.GetOptions().GetString("--io-latency", "azahar");
    IoLatency ignored;
    if (!ParseIoLatency(value, &ignored)) {
        MH4N_LOG(Error, "Service.FS", "--io-latency takes azahar or none, not '{}'", value);
        runtime.RequestExit(2, "bad --io-latency");
    }
}

MH4N_REGISTER(BootStep, "services.fs.options", BootStep{Phase::Services, CheckIoLatency});

}  // namespace

}  // namespace mh4n::fs
