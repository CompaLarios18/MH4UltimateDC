// Headless runner: boots a 3DS executable with Azahar's core and no window, with either
// the recompiled code or one of Azahar's own CPU backends, until the program exits or an
// emulated-time limit is reached. Debug output (svcOutputDebugString) goes to stdout.
// For testing the real game it can also install a CIA (the update) first, press buttons
// and touch the screen on an emulated-time script, save both screens as images, and join
// a network room (Azahar's multiplayer) so that two runners can play locally together.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <tuple>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fmt/format.h>

#include "arm_recomp.h"
#include "hooks.h"
#include "schedule.h"
#include "audio_core/hle/common.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/image_interface.h"
#include "core/frontend/input.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/object.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/server_session.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/kernel/session.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/fs/directory.h"
#include "core/hle/service/fs/file.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "network/network.h"
#include "network/verify_user.h"
#include "video_core/gpu.h"
#include "video_core/renderer_software/renderer_software.h"

namespace {

constexpr double TICKS_PER_MS = 268111.856; // ARM11 clock

const char* const USAGE = R"(usage: mh4u_runner <file> [options]
  --code LIB        recompiled code library (with --cpu recomp)
  --cpu KIND        recomp (default), jit or interp
  --ms N            stop after N ms of emulated time (default 10000); f<N>: after game frame N
  --frames N        stop when the game's frame counter (the u32 at 0x010571d4) reaches N
                    (exit status 0, as --ms)
  --log FILTER      log filter (default "*:Warning Debug.Emulated:Debug Kernel:Info")
  --user DIR        Azahar user directory (NAND, SD card, saves) to use
  --install CIA     install a CIA, e.g. the game's update, before booting (repeatable)
  --shots DIR       save the top and bottom screens as PPM images in DIR
  --shot-every MS   how often, in emulated time (default 1000)
  --shots-from MS   first screenshot at this emulated time (default: one interval in)
  --draw-window MS  draw only during the MS of emulated time before each screenshot (and
                    nothing without --shots; the GPU's vertex shaders are skipped too): much
                    faster with the software renderer, but a screen built up over several
                    frames can come out incomplete
  --speed PERCENT   emulation speed limit (default 0 = as fast as possible; 100 = real time)
  --cpu-clock PERCENT  emulated CPU clock (default 100; Azahar's CPU Clock Speed setting)
  --async-io        let file reads finish on host threads and start the clock at the
                    host's time, as Azahar does by default; the runner otherwise does reads in
                    place and starts at a fixed date, so that a run repeats exactly (an
                    asynchronous read wakes the game at an emulated time that depends on how
                    fast the host was, which moves loading screens by seconds, and the game
                    seeds its random numbers from the clock)
  --dump MS:ADDR:SIZE  at emulated time MS, write SIZE bytes of guest memory from ADDR to
                    mem_<ms>_<addr>.bin (mem_f<frame>_<addr>.bin for a frame) in the --shots
                    directory (or the current one);
                    repeatable, e.g. to diff the game's state with and without an input
  --replace MS:FROM:TO  at emulated time MS, replace every FROM in the game's writable
                    memory with TO (hex bytes, same length) and print how many it found;
                    repeatable, e.g. to give the second of two runners from copies of one
                    user folder another hunter
  --call-counts MS  at emulated time MS (or at the end, if the run ends first), write how
                    often each recompiled function has run so far to calls_<ms>.txt in the
                    --shots directory (repeatable; needs a code library built with
                    -D RC_CALL_COUNTS): diff two to see what ran in between
  --trace-svc FILE  write every system call to FILE (thread, number, pc, lr, r0-r3, ticks,
                    r4-r6 on entry, r0-r3 when the call completes for its thread), a line per
                    thread created and a header, to compare CPU backends thread by thread
                    (tools/compare_svc_traces.py)
  --trace-ipc FILE  write every IPC request with its reply (also the ones a service answers
                    later) to FILE, one line each: thread, order sent, session, header, the
                    words both ways, the buffers and handles with CRC-32s and names, ticks;
                    a header and a line per thread created. The file holds game data (file
                    names, save bytes): keep it in build/ (tools/compare_ipc_traces.py)
  --trace-ipc-full  with --trace-ipc: also write the bytes of the input buffers (data=)
  --state-trace FILE  write the game's observable state (the values listed in --observable)
                    on every game frame: a line whenever the frame counter changes, to compare
                    runtimes scene by scene (tools/native/state_diff.py)
  --observable FILE  the values --state-trace records (default config/native/observable.txt,
                    relative to the current directory)
  --count-unmapped FILE  at the end of the run, write how often the game touched unmapped
                    memory, by address, size, read or write and caller (LR), most often first
  --capture-dsp START:FRAMES:FILE  from the first DSP audio frame at or after game frame START,
                    record FRAMES audio frames to FILE (an MDSP capture): per frame the DSP shared
                    memory, the sample data the DSP read (bytes only the first time) and the 160
                    mixed stereo samples, before the sink (tools/native/capfmt.py). The file holds
                    game data: keep it in build/
  --capture-hid FILE  record the HID shared memory (and ir:USER's, once a game sets it up) on every
                    game frame to FILE (an MHID capture; tools/native/capfmt.py)
  --new-3ds on|off  run as a New 3DS or an Old 3DS (default off: the reference is an Old 3DS)
  --input SCRIPT    comma-separated presses at emulated times, e.g.
                    "3000:A,3500:START:200,5000:TOUCH:160:120,6000:CIRCLE:0:1:500"
                    (ms:BUTTON[:hold ms], ms:TOUCH:x:y[:hold], ms:CIRCLE:x:y[:hold];
                    hold defaults to 100 ms; buttons A B X Y UP DOWN LEFT RIGHT L R
                    START SELECT ZL ZR HOME)
  --input-file PATH  the same presses, one per line; '#' starts a comment (repeatable)
  --print-schedule  print the parsed schedule (stop, presses, dumps, replaces, options, ...)
                    and exit, without loading a game
                    Everywhere a time MS is accepted (--input, --dump, --replace, --set,
                    --freeze, --call-counts, --shots-from, --ms), f<N> means the first moment
                    the game's frame counter reads N or more; a hold may be f<N> frames too.
  --room-host PORT  host a network room (Azahar's multiplayer) on this UDP port and join it
  --room-join HOST:PORT  join a network room; the game's local wireless play then reaches
                    the other members (use --speed 100 on every runner so they keep time)
  --nick NAME       nickname in the room, 4 to 20 letters, digits, spaces, '.', '_' or '-'
                    (default "runner")
  --new-console-id  give the console a new random ID before the game starts (a room turns
                    away a second member with the same ID, e.g. two copies of a user folder)
  --lockstep FILE:COUNT:INDEX  keep the emulated clocks of COUNT runners on this machine
                    together (this one is INDEX, 0..COUNT-1; they share FILE): a runner waits
                    while it is more than a few ms ahead of the slowest. Without it, a runner
                    that stops to draw a screenshot (the clock stands still while the GPU
                    works) falls silent for the others, and the game's local play drops it
  --stalls MS       print each step of the emulation that took longer than MS of host time
  --freeze MS:HOST_MS  at emulated time MS, stop emulating for HOST_MS of host time, as a PC
                    that stalls would (shader compilation): the port's net sync makes the other
                    consoles in a network room wait instead of dropping this one
  --set MS:KEY=VALUE  at emulated time MS, change one of the port's options (mh4u.ini keys
                    the hooks read as the game runs: fps, refresh_rate, dither, kick, target,
                    quest_overlay, hud_edges, bloom, depth_of_field, ...), e.g. one player of a
                    session at 120 fps once everyone is in (repeatable)
or:    mh4u_runner --overlay-layout SPEC WIDTHxHEIGHT[:TOP_WIDTH] [QUEST SLOT [TARGET TSLOT]]
                    print the custom screen layout the port's touch_overlay option SPEC
                    gives a window that size (top screen TOP_WIDTH x 240, default 400);
                    with the quest_overlay option QUEST and the map's touch-panel slot,
                    the layout while a quest's panels are up, the part of the touch screen
                    shown, and where on the touch screen a click in the overlay's middle lands;
                    with the target_overlay option TARGET and the target camera's slot, the
                    same for the target camera's view
or:    mh4u_runner --panel-view quest_overlay|target_overlay VALUE
                    print the option's value as the port reads it and writes it back
or:    mh4u_runner --quest-overlay-frames FPS KIND:SECONDS[,KIND:SECONDS...]
                    run a made-up quest's frames through the choice of what the touch overlay
                    shows and print each change; KIND: map0..map5 (the small panels up, the map
                    in that slot), nomap, drawn (on the touch screen but not running: someone
                    talking), none (no panels: a change of area), menu (the quest menu or a big
                    panel), off (F12)
or:    mh4u_runner --install CIA [--install CIA...] [--user DIR]
                    install CIAs (the game's update, say) into an Azahar user folder and
                    exit, as Azahar's File > Install CIA... does
)";

class HeadlessWindow final : public Frontend::EmuWindow {
public:
    void PollEvents() override {}
};

int Usage() {
    std::fputs(USAGE, stderr);
    return 1;
}

s64 g_start_ticks = 0; // the emulated clock doesn't start at 0

/// Emulated time since the program was loaded.
double EmulatedMs() {
    return (Core::System::GetInstance().CoreTiming().GetGlobalTicks() - g_start_ticks) /
           TICKS_PER_MS;
}

// ── scripted input ──────────────────────────────────────────────────────────────────

// The schedule (times, input events, scheduled options) is parsed by schedule.cpp, which the native
// runner shares; a time is emulated ms or a frame of the game's counter (g_frame, read after
// each slice of emulation).
Schedule::Plan g_plan;
u32 g_frame = 0;

// Button indexes of Schedule::ButtonName, in the same order.
constexpr int kNativeButtons[Schedule::kButtonCount] = {
    Settings::NativeButton::A,     Settings::NativeButton::B,    Settings::NativeButton::X,
    Settings::NativeButton::Y,     Settings::NativeButton::Up,   Settings::NativeButton::Down,
    Settings::NativeButton::Left,  Settings::NativeButton::Right, Settings::NativeButton::L,
    Settings::NativeButton::R,     Settings::NativeButton::Start, Settings::NativeButton::Select,
    Settings::NativeButton::ZL,    Settings::NativeButton::ZR,   Settings::NativeButton::Home,
};

/// A moment as it appears in file names: 0001500 for ms, f0000600 for frames.
std::string WhenStem(const Schedule::When& when) {
    return when.by_frame ? fmt::format("f{:07}", when.frame) : fmt::format("{:07.0f}", when.ms);
}

/// Replaces byte strings in every writable region of the game's address space whose time has come.
void DoReplaces(Core::System& system) {
    const double now = EmulatedMs();
    for (Schedule::Replace& r : g_plan.replaces) {
        if (r.done || !r.when.Due(now, g_frame)) {
            continue;
        }
        r.done = true;
        const auto process = system.Kernel().GetCurrentProcess();
        if (!process) {
            continue;
        }
        auto& memory = system.Memory();
        unsigned found = 0;
        std::vector<u8> bytes;
        for (const auto& [base, vma] : process->vm_manager.vma_map) {
            if (vma.type != Kernel::VMAType::BackingMemory ||
                (static_cast<u32>(vma.permissions) & static_cast<u32>(Kernel::VMAPermission::Write)) == 0) {
                continue;
            }
            bytes.resize(vma.size);
            memory.ReadBlock(*process, vma.base, bytes.data(), vma.size);
            auto at = bytes.begin();
            while ((at = std::search(at, bytes.end(), r.from.begin(), r.from.end())) != bytes.end()) {
                memory.WriteBlock(*process, vma.base + static_cast<u32>(at - bytes.begin()), r.to.data(),
                                  r.to.size());
                ++found;
                at += static_cast<std::ptrdiff_t>(r.from.size());
            }
        }
        std::printf("replace at %.0f ms: %u found\n", now, found);
        std::fflush(stdout);
    }
}

// ── lockstep ────────────────────────────────────────────────────────────────────────

/// Runners in lockstep share a file with one slot per runner: its emulated time in ms (a
/// double; +infinity once it has finished). Each runner writes its own slot and waits while
/// it is ahead of the slowest of the others by more than kLockstepSlackMs.
struct Lockstep {
    std::FILE* file = nullptr;
    int count = 0, index = 0;
    double written = -1.0;  // last time written to our slot
    double others = 0.0;    // the slowest other runner, as last read
};
Lockstep g_lockstep;
constexpr double kLockstepSlackMs = 4.0;
constexpr double kLockstepGiveUpS = 120.0; // a runner that hasn't moved for this long has died

bool ParseLockstep(const std::string& arg) {
    const auto second = arg.rfind(':');
    const auto first = second == std::string::npos || second == 0 ? std::string::npos : arg.rfind(':', second - 1);
    if (first == std::string::npos || first == 0) {
        return false;
    }
    const int count = std::atoi(arg.c_str() + first + 1);
    const int index = std::atoi(arg.c_str() + second + 1);
    if (count < 2 || count > 16 || index < 0 || index >= count) {
        return false;
    }
    const std::string path = arg.substr(0, first);
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) {
        f = std::fopen(path.c_str(), "w+b");
    }
    if (!f) {
        return false;
    }
    std::setvbuf(f, nullptr, _IONBF, 0); // every read and write goes to the shared file
    std::fseek(f, 0, SEEK_END);
    for (long size = std::ftell(f); size < count * static_cast<long>(sizeof(double)); size += sizeof(double)) {
        const double zero = 0.0;
        std::fwrite(&zero, sizeof zero, 1, f);
    }
    g_lockstep = {f, count, index};
    return true;
}

void LockstepWrite(double ms) {
    std::fseek(g_lockstep.file, g_lockstep.index * static_cast<long>(sizeof(double)), SEEK_SET);
    std::fwrite(&ms, sizeof ms, 1, g_lockstep.file);
    g_lockstep.written = ms;
}

double LockstepReadOthers() {
    std::vector<double> slots(g_lockstep.count, 0.0);
    std::fseek(g_lockstep.file, 0, SEEK_SET);
    if (std::fread(slots.data(), sizeof(double), slots.size(), g_lockstep.file) != slots.size()) {
        return g_lockstep.others;
    }
    double slowest = std::numeric_limits<double>::infinity();
    for (int i = 0; i < g_lockstep.count; i++) {
        if (i != g_lockstep.index) {
            slowest = std::min(slowest, slots[i]);
        }
    }
    return slowest;
}

/// Publishes this runner's emulated time and waits until it is no more than a few ms ahead of
/// every other runner (or one of them has stopped moving for a long time).
void LockstepWait(double now) {
    if (!g_lockstep.file) {
        return;
    }
    if (now - g_lockstep.written >= 1.0) {
        LockstepWrite(now);
    }
    if (now <= g_lockstep.others + kLockstepSlackMs) {
        return;
    }
    auto moved = std::chrono::steady_clock::now();
    double last = g_lockstep.others;
    while ((g_lockstep.others = LockstepReadOthers()) + kLockstepSlackMs < now) {
        const auto host = std::chrono::steady_clock::now();
        if (g_lockstep.others != last) {
            last = g_lockstep.others;
            moved = host;
        } else if (std::chrono::duration<double>(host - moved).count() > kLockstepGiveUpS) {
            std::printf("lockstep: the other runners stopped at %.0f ms; going on alone\n", last);
            std::fflush(stdout);
            std::fclose(g_lockstep.file);
            g_lockstep.file = nullptr;
            return;
        }
        if (now - g_lockstep.written > 0.0) {
            LockstepWrite(now);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void LockstepFinish() {
    if (g_lockstep.file) {
        LockstepWrite(std::numeric_limits<double>::infinity());
        std::fclose(g_lockstep.file);
        g_lockstep.file = nullptr;
    }
}

/// Applies the option changes that are due; false if one names an option that can't change.
bool ApplyOptionChanges() {
    for (auto& c : g_plan.option_changes) {
        if (c.done || !c.when.Due(EmulatedMs(), g_frame)) {
            continue;
        }
        c.done = true;
        if (!Port::Hooks::SetOption(c.key, c.value)) {
            std::fprintf(stderr, "--set: %s is not an option that can change while the game runs\n",
                         c.key.c_str());
            return false;
        }
        std::printf("set %s = %s at %.0f ms\n", c.key.c_str(), c.value.c_str(), EmulatedMs());
        std::fflush(stdout);
    }
    return true;
}

/// Writes the dumps that are due; unmapped bytes come out as zero.

/// --call-counts: each function's calls so far, sorted by address (at the end of the run,
/// `all` writes the snapshots whose time hasn't come).
void WriteCallCounts(const std::string& dir, bool all = false) {
    for (auto& snapshot : g_plan.call_counts) {
        if (snapshot.done || (!snapshot.when.Due(EmulatedMs(), g_frame) && !all)) {
            continue;
        }
        snapshot.done = true;
        const auto path = fmt::format("{}/calls_{}.txt", dir.empty() ? "." : dir, WhenStem(snapshot.when));
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            for (const auto& [addr, calls] : Port::ARM_Recomp::CallCounts()) {
                std::fprintf(f, "%08x %u\n", addr, calls);
            }
            std::fclose(f);
        }
    }
}

void WriteDumps(Core::System& system, const std::string& dir) {
    const double now = EmulatedMs();
    for (Schedule::Dump& d : g_plan.dumps) {
        if (d.done || !d.when.Due(now, g_frame)) {
            continue;
        }
        d.done = true;
        std::vector<u8> bytes(d.size);
        auto& memory = system.Memory();
        auto process = system.Kernel().GetCurrentProcess();
        for (u32 off = 0; off < d.size;) {
            const VAddr a = d.addr + off;
            const u32 chunk = std::min<u32>(d.size - off, 0x1000 - (a & 0xfff)); // page by page
            if (process && memory.IsValidVirtualAddress(*process, a)) {
                memory.ReadBlock(*process, a, bytes.data() + off, chunk);
            }
            off += chunk;
        }
        const std::string path = fmt::format("{}/mem_{}_{:08x}.bin", dir.empty() ? "." : dir, WhenStem(d.when), d.addr);
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
}

// ── game-state trace and unmapped-access counter (N0-07) ────────────────────────────

constexpr u32 kFrameCounterAddress = 0x010571d4; // the game's frame counter (mirrors sLtRender+0x6b90)

/// One line of the observable file: `<expr> <size> <name> <eq|ignore>` (the syntax is documented in
/// config/native/observable.txt and implemented the same way by tools/native/state_diff.py).
struct StateValue {
    std::string expr; // as written, without the outermost brackets if it has them
    u32 size;
    std::string name;
};

struct StateTrace {
    std::FILE* file = nullptr;
    std::vector<StateValue> values;
    bool have_frame = false;
    u32 last_frame = 0;
} g_state_trace;

bool IsWrapped(const std::string& text) {
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
        return false;
    }
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        depth += text[i] == '[';
        depth -= text[i] == ']';
        if (depth == 0) {
            return i + 1 == text.size();
        }
    }
    return false;
}

/// Splits at the '+' outside brackets; false if the brackets do not match.
bool SplitTerms(const std::string& text, std::vector<std::string>& terms) {
    int depth = 0;
    std::string current;
    terms.clear();
    for (const char c : text) {
        depth += c == '[';
        depth -= c == ']';
        if (depth < 0) {
            return false;
        }
        if (c == '+' && depth == 0) {
            terms.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    terms.push_back(current);
    return depth == 0;
}

bool IsHexTerm(const std::string& term) {
    return term.size() > 2 && term[0] == '0' && term[1] == 'x' &&
           term.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos;
}

bool CheckAddressExpr(const std::string& text) {
    std::vector<std::string> terms;
    if (!SplitTerms(text, terms)) {
        return false;
    }
    for (const std::string& term : terms) {
        if (IsWrapped(term) ? !CheckAddressExpr(term.substr(1, term.size() - 2)) : !IsHexTerm(term)) {
            return false;
        }
    }
    return true;
}

/// Reads SIZE (1, 2 or 4) bytes of guest memory, little-endian. Looks the pages up first, so an
/// unmapped address gives false instead of a logged error that the unmapped counter would count,
/// and nothing is written.
bool ReadGuest(Core::System& system, u32 address, u32 size, u32& value) {
    const auto process = system.Kernel().GetCurrentProcess();
    if (!process) {
        return false;
    }
    auto& memory = system.Memory();
    if (address + size < address || !memory.IsValidVirtualAddress(*process, address) ||
        !memory.IsValidVirtualAddress(*process, address + size - 1)) {
        return false;
    }
    value = 0;
    u8 bytes[4] = {};
    memory.ReadBlock(*process, address, bytes, size);
    for (u32 i = 0; i < size; ++i) {
        value |= static_cast<u32>(bytes[i]) << (8 * i);
    }
    return true;
}

/// The address an expression stands for; false if a pointer in a chain is null or unreadable.
bool EvalAddress(Core::System& system, const std::string& text, u32& address) {
    std::vector<std::string> terms;
    if (!SplitTerms(text, terms)) {
        return false;
    }
    address = 0;
    for (const std::string& term : terms) {
        u32 part = 0;
        if (IsWrapped(term)) {
            u32 inner = 0;
            if (!EvalAddress(system, term.substr(1, term.size() - 2), inner) ||
                !ReadGuest(system, inner, 4, part) || part == 0) {
                return false;
            }
        } else {
            part = static_cast<u32>(std::strtoul(term.c_str(), nullptr, 16));
        }
        address += part;
    }
    return true;
}

/// config/native/observable.txt: lines `<expr> <size> <name> <eq|ignore>`, `#` starts a comment.
bool LoadObservable(const std::string& path, std::vector<StateValue>& values, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = fmt::format("cannot read the observable file {} (--observable FILE)", path);
        return false;
    }
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        line = line.substr(0, line.find('#'));
        std::istringstream fields(line);
        std::string expr, size, name, mode, extra;
        if (!(fields >> expr)) {
            continue; // blank
        }
        if (!(fields >> size >> name >> mode) || (fields >> extra)) {
            error = fmt::format("{}:{}: want '<expr> <size> <name> <eq|ignore>'", path, number);
            return false;
        }
        if (!CheckAddressExpr(IsWrapped(expr) ? expr.substr(1, expr.size() - 2) : expr) ||
            (size != "1" && size != "2" && size != "4") || (mode != "eq" && mode != "ignore")) {
            error = fmt::format("{}:{}: bad expression, size or mode", path, number);
            return false;
        }
        values.push_back({IsWrapped(expr) ? expr.substr(1, expr.size() - 2) : expr,
                          static_cast<u32>(std::atoi(size.c_str())), name});
    }
    if (values.empty()) {
        error = path + " lists no values";
        return false;
    }
    return true;
}

/// `# <tool> v1 <ISO date> game=0x<title id> cpu=<cpu> new3ds=<on|off>`
void WriteHeader(std::FILE* file, Core::System& system, const char* tool, const std::string& cpu,
                 bool new_3ds) {
    u64 program_id = 0;
    if (system.GetAppLoader().ReadProgramId(program_id) != Loader::ResultStatus::Success) {
        program_id = 0;
    }
    const std::time_t now = std::time(nullptr);
    char date[16] = "0000-00-00";
    if (const std::tm* utc = std::gmtime(&now)) {
        std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
    }
    std::fprintf(file, "# %s v1 %s game=0x%016llx cpu=%s new3ds=%s\n", tool, date,
                 static_cast<unsigned long long>(program_id), cpu.c_str(), new_3ds ? "on" : "off");
}

bool BeginStateTrace(Core::System& system, const std::string& path, const std::string& observable,
                     const std::string& cpu, bool new_3ds) {
    std::string error;
    if (!LoadObservable(observable, g_state_trace.values, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    g_state_trace.file = std::fopen(path.c_str(), "w");
    if (!g_state_trace.file) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    WriteHeader(g_state_trace.file, system, "statetrace", cpu, new_3ds);
    return true;
}

/// After each slice of emulation: the frame counter the schedule's `f<N>` times are read against.
void UpdateFrame(Core::System& system) {
    u32 frame = 0;
    if (ReadGuest(system, kFrameCounterAddress, 4, frame)) {
        g_frame = frame;
    }
}

/// After each slice of emulation: a line if the frame counter changed since the last one.
/// `f=<frame> t=<ticks> <name>=<0x value | ->`, every value as wide as its size.
void WriteStateTrace(Core::System& system) {
    if (!g_state_trace.file) {
        return;
    }
    u32 frame = 0;
    if (!ReadGuest(system, kFrameCounterAddress, 4, frame) ||
        (g_state_trace.have_frame && frame == g_state_trace.last_frame)) {
        return;
    }
    g_state_trace.have_frame = true;
    g_state_trace.last_frame = frame;
    std::string line = fmt::format("f={} t={}", frame,
                                   static_cast<long long>(system.CoreTiming().GetGlobalTicks() - g_start_ticks));
    for (const StateValue& v : g_state_trace.values) {
        u32 address = 0, value = 0;
        if (EvalAddress(system, v.expr, address) && ReadGuest(system, address, v.size, value)) {
            line += fmt::format(" {}=0x{:0{}x}", v.name, value, v.size * 2);
        } else {
            line += fmt::format(" {}=-", v.name);
        }
    }
    line += '\n';
    std::fputs(line.c_str(), g_state_trace.file);
}

void EndStateTrace() {
    if (g_state_trace.file) {
        std::fclose(g_state_trace.file);
        g_state_trace.file = nullptr;
    }
}

/// Accesses to unmapped pages, by (address, size, read, caller's LR).
struct UnmappedKey {
    u32 address, size, lr;
    bool read;
    bool operator<(const UnmappedKey& o) const {
        return std::tie(address, size, read, lr) < std::tie(o.address, o.size, o.read, o.lr);
    }
};
std::map<UnmappedKey, unsigned long long> g_unmapped;
std::mutex g_unmapped_mutex;

void CountUnmapped(VAddr address, u32 size, bool read, u32 lr) {
    std::lock_guard lock{g_unmapped_mutex};
    ++g_unmapped[{address, size, lr, read}];
}

/// `# unmapped v1 ...` header, then `count=<n> addr=<hex> size=<n> rw=<read|write> lr=<hex>`,
/// the most frequent first.
void WriteUnmappedReport(Core::System& system, const std::string& path, const std::string& cpu,
                         bool new_3ds) {
    Memory::SetUnmappedObserver(nullptr);
    std::vector<std::pair<UnmappedKey, unsigned long long>> rows;
    {
        std::lock_guard lock{g_unmapped_mutex};
        rows.assign(g_unmapped.begin(), g_unmapped.end());
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    WriteHeader(f, system, "unmapped", cpu, new_3ds);
    for (const auto& [key, count] : rows) {
        std::fprintf(f, "count=%llu addr=0x%08x size=%u rw=%s lr=0x%08x\n", count, key.address, key.size,
                     key.read ? "read" : "write", key.lr);
    }
    std::fclose(f);
    std::printf("unmapped accesses: %zu distinct, %s\n", rows.size(), path.c_str());
}

/// The event of this kind being held now (for a button, the one of that NativeButton).
const Schedule::Press* Active(Schedule::PressKind kind, int native_button = -1) {
    const double now = EmulatedMs();
    for (Schedule::Press& p : g_plan.presses) {
        if (p.kind == kind && (kind != Schedule::PressKind::Button || kNativeButtons[p.button] == native_button) &&
            p.Active(now, g_frame)) {
            return &p;
        }
    }
    return nullptr;
}

class ScriptButton final : public Input::ButtonDevice {
public:
    explicit ScriptButton(int button) : button(button) {}
    bool GetStatus() const override {
        return Active(Schedule::PressKind::Button, button) != nullptr;
    }

private:
    int button;
};

class ScriptCirclePad final : public Input::AnalogDevice {
public:
    std::tuple<float, float> GetStatus() const override {
        const Schedule::Press* p = Active(Schedule::PressKind::Circle);
        return p ? std::make_tuple(p->x, p->y) : std::make_tuple(0.0f, 0.0f);
    }
};

class ScriptTouch final : public Input::TouchDevice {
public:
    std::tuple<float, float, bool> GetStatus() const override {
        const Schedule::Press* p = Active(Schedule::PressKind::Touch);
        return p ? std::make_tuple(p->x / 320.0f, p->y / 240.0f, true)
                 : std::make_tuple(0.0f, 0.0f, false);
    }
};

template <typename Device, typename Make>
class ScriptFactory final : public Input::Factory<Device> {
public:
    explicit ScriptFactory(Make make) : make(make) {}
    std::unique_ptr<Device> Create(const Common::ParamPackage& params) override {
        return make(params);
    }

private:
    Make make;
};

template <typename Device, typename Make>
void Register(Make make) {
    Input::RegisterFactory<Device>("script", std::make_shared<ScriptFactory<Device, Make>>(make));
}

void UseScriptedInput() {
    Register<Input::ButtonDevice>([](const Common::ParamPackage& params) {
        return std::make_unique<ScriptButton>(params.Get("button", 0));
    });
    Register<Input::AnalogDevice>(
        [](const Common::ParamPackage&) { return std::make_unique<ScriptCirclePad>(); });
    Register<Input::TouchDevice>(
        [](const Common::ParamPackage&) { return std::make_unique<ScriptTouch>(); });
    auto& profile = Settings::values.current_input_profile;
    for (std::size_t i = 0; i < profile.buttons.size(); ++i) {
        profile.buttons[i] = "engine:script,button:" + std::to_string(i);
    }
    profile.analogs[Settings::NativeAnalog::CirclePad] = "engine:script";
    profile.touch_device = "engine:script";
    profile.use_touchpad = false;
    profile.use_touch_from_button = false;
}

// ── system call trace ──────────────────────────────────────────────────────────────
//
// One line per call, written when the call completes for its thread (see N0-04 in
// docs/native/wp/N0.md for the format): right after the handler for a call that returns at
// once, when the thread next runs for a call that blocks it (Azahar writes the real result of
// a wait into the thread's saved registers when it wakes the thread up, so what the handler
// leaves in r0-r3 is only provisional).

FILE* g_svc_trace = nullptr;
bool g_hid_capture_active = false; // --capture-hid keeps Azahar's IPC recorder enabled (N0-06)
FILE* g_ipc_trace = nullptr; // --trace-ipc (N0-21); shares the thread lines of the system call trace

/// A call between its start and its completion: the line up to `out=`, and the registers it
/// started with (r0-r6), which the thread line of a CreateThread needs.
struct PendingSvc {
    std::string head;
    std::array<u32, 7> regs{};
};
std::map<u32, PendingSvc> g_svc_pending; // by thread id: one call per thread at a time

void WriteSvcLine(const PendingSvc& call, const u32* r) {
    std::fprintf(g_svc_trace, "%s out=%08X,%08X,%08X,%08X\n", call.head.c_str(), r[0], r[1], r[2], r[3]);
}

/// The call never completed (the run ended first): its results are unknown.
void WriteUnfinishedSvcLine(const PendingSvc& call) {
    std::fprintf(g_svc_trace, "%s out=-\n", call.head.c_str());
}

u32 CurrentThreadId(Core::System& system) {
    const auto* thread = system.Kernel().GetCurrentThreadManager().GetCurrentThread();
    return thread ? thread->GetThreadId() : 0u;
}

/// `# thread <id> parent=<id> entry=<hex> arg=<hex> stack=<hex> prio=<n> core=<requested>/<resolved>`
void WriteThreadLineTo(FILE* file, u32 id, u32 parent, u32 entry, u32 arg, u32 stack, u32 priority, s32 requested,
                       s32 resolved) {
    std::fprintf(file, "# thread %u parent=%u entry=%08X arg=%08X stack=%08X prio=%u core=%d/%d\n", id, parent,
                 entry, arg, stack, priority, requested, resolved);
}

/// The line of a thread created while the traces run, in every trace that is open.
void WriteThreadLine(u32 id, u32 parent, u32 entry, u32 arg, u32 stack, u32 priority, s32 requested,
                     s32 resolved) {
    for (FILE* file : {g_svc_trace, g_ipc_trace}) {
        if (file) {
            WriteThreadLineTo(file, id, parent, entry, arg, stack, priority, requested, resolved);
        }
    }
}

/// The lines of the threads that exist when a trace starts (the process starts its main thread
/// before the first call).
void WriteExistingThreadLines(FILE* file, Core::System& system) {
    for (u32 core = 0; core < system.GetNumCores(); ++core) {
        for (const auto& thread : system.Kernel().GetThreadManager(core).GetThreadList()) {
            WriteThreadLineTo(file, thread->GetThreadId(), 0, static_cast<u32>(thread->entry_point),
                              thread->context.cpu_registers[0], static_cast<u32>(thread->stack_top),
                              thread->nominal_priority, thread->processor_id, thread->processor_id);
        }
    }
}

/// A CreateThread (SVC 0x08) that succeeded in thread CALLER, made with the registers REGS
/// (priority, entry, arg, stack top, processor id) and the handle HANDLE in r1 after the call:
/// the new thread's line.
void NoteCreatedThread(Core::System& system, u32 caller, const std::array<u32, 7>& regs, u32 handle) {
    if (const auto process = system.Kernel().GetCurrentProcess()) {
        if (const auto created = process->handle_table.Get<Kernel::Thread>(handle)) {
            WriteThreadLine(created->GetThreadId(), caller, regs[1], regs[2], regs[3], regs[0],
                            static_cast<s32>(regs[4]), created->processor_id);
        }
    }
}

/// Before the call: the registers it was made with.
void TraceSvc(Core::System& system, u32 imm) {
    auto& cpu = system.GetRunningCore();
    const u32 id = CurrentThreadId(system);
    if (const auto old = g_svc_pending.find(id); old != g_svc_pending.end()) {
        WriteUnfinishedSvcLine(old->second); // cannot happen: a thread that called has not run since
    }
    PendingSvc call;
    for (std::size_t i = 0; i < call.regs.size(); ++i) {
        call.regs[i] = cpu.GetReg(static_cast<int>(i));
    }
    call.head = fmt::format("{} {:02X} pc={:08X} lr={:08X} {:08X} {:08X} {:08X} {:08X} t={} in4={:08X},{:08X},{:08X}",
                            id, imm, cpu.GetReg(15), cpu.GetReg(14), call.regs[0], call.regs[1], call.regs[2],
                            call.regs[3],
                            static_cast<long long>(system.CoreTiming().GetGlobalTicks() - g_start_ticks),
                            call.regs[4], call.regs[5], call.regs[6]);
    g_svc_pending[id] = std::move(call);
}

/// After the handler: a call that left its thread runnable (or dead) is done; one that put it
/// to sleep waits for TraceSvcResume.
void TraceSvcDone(Core::System& system, u32 imm) {
    auto& cpu = system.GetRunningCore();
    const auto* thread = system.Kernel().GetCurrentThreadManager().GetCurrentThread();
    const u32 id = thread ? thread->GetThreadId() : 0u;
    const auto it = g_svc_pending.find(id);
    if (it == g_svc_pending.end()) {
        return;
    }
    const u32 out[4] = {cpu.GetReg(0), cpu.GetReg(1), cpu.GetReg(2), cpu.GetReg(3)};
    if (imm == 0x08 && out[0] == 0) {
        // CreateThread succeeded: the new thread's id comes from the handle in r1
        NoteCreatedThread(system, id, it->second.regs, out[1]);
    }
    const bool blocked = thread && thread->status != Kernel::ThreadStatus::Running &&
                         thread->status != Kernel::ThreadStatus::Dead;
    if (!blocked) {
        WriteSvcLine(it->second, out);
        g_svc_pending.erase(it);
    }
}

/// A thread is being switched to, with its saved registers loaded: if a call blocked it, they
/// hold the call's real results.
void TraceSvcResume(u32 thread_id, const u32* regs) {
    const auto it = g_svc_pending.find(thread_id);
    if (it != g_svc_pending.end()) {
        WriteSvcLine(it->second, regs);
        g_svc_pending.erase(it);
    }
}

/// The header, and the main thread's line (the process starts it before the first call).
void BeginSvcTrace(Core::System& system, const std::string& cpu, bool new_3ds) {
    u64 program_id = 0;
    if (system.GetAppLoader().ReadProgramId(program_id) != Loader::ResultStatus::Success) {
        program_id = 0;
    }
    const std::time_t now = std::time(nullptr);
    char date[16] = "0000-00-00";
    if (const std::tm* utc = std::gmtime(&now)) {
        std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
    }
    std::fprintf(g_svc_trace, "# svctrace v1 %s game=0x%016llx cpu=%s new3ds=%s\n", date,
                 static_cast<unsigned long long>(program_id), cpu.c_str(), new_3ds ? "on" : "off");
    WriteExistingThreadLines(g_svc_trace, system);
    Kernel::SetSVCObserver(TraceSvc);
    Kernel::SetSVCPostObserver(TraceSvcDone);
    Kernel::SetThreadResumeObserver(TraceSvcResume);
}

void EndSvcTrace() {
    Kernel::SetSVCObserver(nullptr);
    Kernel::SetSVCPostObserver(nullptr);
    Kernel::SetThreadResumeObserver(nullptr);
    for (const auto& [id, call] : g_svc_pending) {
        WriteUnfinishedSvcLine(call); // threads still blocked when the run ended
    }
    g_svc_pending.clear();
    std::fclose(g_svc_trace);
    g_svc_trace = nullptr;
}

// ── IPC trace (N0-21) ──────────────────────────────────────────────────────────────
//
// Derived from Azahar src/core/hle/ipc.h, src/core/hle/kernel/hle_ipc.cpp,
// src/core/hle/kernel/server_port.cpp (the "_Client" suffix of a client port's name) and
// src/core/hle/kernel/ipc_debugger/recorder.h, recorder.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "GPLv2 or any later version"): the descriptor layouts and the walk over the translate
// parameters (ParseIpcCommand) follow ipc.h and hle_ipc.cpp; the recorder is used as recorder.h
// and recorder.cpp define it. This section only.
// Copyright 2016 Citra Emulator Project (ipc.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (hle_ipc.cpp)
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (server_port.cpp)
// Copyright 2019 Citra Emulator Project (recorder.h, recorder.cpp)
//
// One line per IPC request, written when its reply is recorded (see N0-21 in docs/native/wp/N0.md
// and the docstring of tools/compare_ipc_traces.py for the format). Azahar's own IPC recorder
// (core/hle/kernel/ipc_debugger) calls us three times per request: when the thread sends it
// (RegisterRequest), when the service has the command (SetRequestInfo) and when the reply is
// written (SetReplyInfo, which a service that answers later calls later, from wherever it does).
// The buffers' CRC-32s are taken from the client's memory at the second and the third call.

constexpr u32 kIpcCommandWords = 0x40; // the command buffer, in words

bool g_ipc_full = false; // --trace-ipc-full
std::mutex g_ipc_mutex;
IPCDebugger::CallbackHandle g_ipc_callback;

/// A command's translate parameters (the descriptors after the header and the normal parameters).
struct IpcPart {
    enum class Type { Handles, Pid, Static, Mapped, Pxi };
    Type type = Type::Handles;
    bool move = false; // Handles: MoveHandle (the kernel closes them in the client), else CopyHandle
    u32 index = 0;     // the descriptor's word
    u32 size = 0;      // Handles: how many; buffers: bytes
    u32 perms = 0;     // Mapped: 1 read, 2 write
    u32 address = 0;   // buffers: the word after the descriptor
};

/// Splits the translate parameters of COMMAND (IPC::Header: translate size in bits 0-5, normal size
/// in bits 6-11; descriptors as IPC::GetDescriptorType reads them) into parts. Stops at a descriptor
/// of a kind it does not know.
std::vector<IpcPart> ParseIpcCommand(const std::vector<u32>& command) {
    std::vector<IpcPart> parts;
    if (command.empty()) {
        return parts;
    }
    const std::size_t end = std::min<std::size_t>(command.size(), 1 + ((command[0] >> 6) & 0x3F) + (command[0] & 0x3F));
    std::size_t i = 1 + ((command[0] >> 6) & 0x3F);
    while (i < end) {
        const u32 d = command[i];
        IpcPart part;
        part.index = static_cast<u32>(i++);
        if ((d & 0xF) == 0) { // handle descriptors: bits 4-5 tell copy (0), move (1), calling PID (2)
            const u32 kind = d & 0x30;
            if (kind == 0x20) {
                part.type = IpcPart::Type::Pid;
                part.size = 1;
            } else if (kind == 0x00 || kind == 0x10) {
                part.type = IpcPart::Type::Handles;
                part.move = kind == 0x10;
                part.size = std::min<u32>((d >> 26) + 1, static_cast<u32>(end - i));
            } else {
                break;
            }
            i += part.size;
        } else {
            if (d & 0x8) { // mapped buffer: permissions in bits 1-2, size above bit 4
                part.type = IpcPart::Type::Mapped;
                part.perms = (d >> 1) & 3;
                part.size = d >> 4;
            } else if (d & 0x4) { // PXI buffer: size above bit 8
                part.type = IpcPart::Type::Pxi;
                part.size = d >> 8;
            } else { // static buffer: size above bit 14
                part.type = IpcPart::Type::Static;
                part.size = d >> 14;
            }
            if (i < end) {
                part.address = command[i];
            }
            ++i;
        }
        parts.push_back(part);
    }
    return parts;
}

/// CRC-32 (the one zlib and gzip use) of DATA, continuing from CRC (0 to start).
u32 Crc32(const u8* data, std::size_t size, u32 crc = 0) {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/// The CRC-32 of SIZE bytes of the process's memory from ADDRESS, and their hex when HEX is given.
/// False when any page of the range is unmapped (it looks first, so nothing is logged or counted).
bool CrcGuest(Core::System& system, Kernel::Process& process, u32 address, u32 size, u32& crc, std::string* hex) {
    crc = 0;
    if (size == 0) {
        return true;
    }
    auto& memory = system.Memory();
    const u64 end = static_cast<u64>(address) + size;
    if (end > 0x100000000ull) {
        return false;
    }
    for (u64 page = address; page < end; page = (page | 0xFFF) + 1) {
        if (!memory.IsValidVirtualAddress(process, static_cast<u32>(page))) {
            return false;
        }
    }
    std::vector<u8> chunk(std::min<u32>(size, 0x10000));
    for (u64 at = address; at < end;) {
        const std::size_t n = static_cast<std::size_t>(std::min<u64>(chunk.size(), end - at));
        memory.ReadBlock(process, static_cast<u32>(at), chunk.data(), n);
        crc = Crc32(chunk.data(), n, crc);
        if (hex) {
            for (std::size_t i = 0; i < n; ++i) {
                *hex += fmt::format("{:02x}", chunk[i]);
            }
        }
        at += n;
    }
    return true;
}

/// A request between its sending and its reply.
struct IpcPending {
    u32 seq = 0;
    u32 thread = 0;
    u32 process_id = 0;
    std::string session;                      // the session's name (see SessionName)
    std::string service;                      // what names the sessions its replies hand out
    std::map<u32, std::string> in_handles;    // word of a request handle -> name, taken when it was sent
    bool have_request = false;                // the service has seen the command
    bool is_hle = false;
    std::vector<u32> in_words;                // the command, as the client wrote it
    std::vector<IpcPart> in_parts;
    std::vector<std::string> desc;            // "kind:index:size:token"
    std::vector<std::string> data;            // "index:hex" (--trace-ipc-full)
};

std::map<int, IpcPending> g_ipc_pending;      // by the recorder's request id
u32 g_ipc_seq = 0;
std::map<u32, std::string> g_ipc_names;       // kernel object id -> the name a reply gave it
std::map<std::string, u32> g_ipc_counts;      // "<service>/<kind>" -> how many were handed out
u32 g_ipc_unnamed = 0;

std::string IpcJoin(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : ",") + item;
    }
    return out.empty() ? "-" : out;
}

std::string IpcWords(const std::vector<u32>& words) {
    std::string out;
    for (u32 w : words) {
        out += fmt::format("{}0x{:08x}", out.empty() ? "" : ",", w);
    }
    return out.empty() ? "-" : out;
}

/// The service's port name for a session that has one (fs:USER); the name a reply gave a session
/// without one (fs:USER/file#3); else a number. Never the session's own name, which holds the
/// file's path.
std::string IpcSessionName(const IPCDebugger::RequestRecord& r) {
    if (!r.client_port.name.empty()) {
        // Azahar's CreatePortPair names the client port "<service>_Client"; the service is the bare name.
        constexpr std::string_view suffix = "_Client";
        std::string name = r.client_port.name;
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            name.resize(name.size() - suffix.size());
        }
        return name;
    }
    const auto known = g_ipc_names.find(static_cast<u32>(r.client_session.id));
    if (known != g_ipc_names.end()) {
        return known->second;
    }
    const std::string name = fmt::format("unnamed#{}", g_ipc_unnamed++);
    g_ipc_names[static_cast<u32>(r.client_session.id)] = name;
    return name;
}

/// `fs:USER/file#3` -> `fs:USER`.
std::string IpcServiceOf(const std::string& session) {
    return session.substr(0, session.find('/'));
}

/// What kind of object a reply handed out: file, dir, session, or the lowercase kernel type.
std::string IpcKindOf(const std::shared_ptr<Kernel::Object>& object) {
    if (const auto client = Kernel::DynamicObjectCast<Kernel::ClientSession>(object)) {
        if (client->parent && client->parent->server) {
            const auto& handler = client->parent->server->hle_handler;
            if (std::dynamic_pointer_cast<Service::FS::File>(handler)) {
                return "file";
            }
            if (std::dynamic_pointer_cast<Service::FS::Directory>(handler)) {
                return "dir";
            }
        }
        return "session";
    }
    std::string kind = object->GetTypeName();
    std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return std::tolower(c); });
    return kind;
}

/// The name of an object a reply handed out: `<service>/<kind>#<k>`, k counting from 0 per service
/// and kind in the order they are handed out; the same object keeps its name.
std::string IpcNameHandedOut(const std::shared_ptr<Kernel::Object>& object, const std::string& service) {
    const auto known = g_ipc_names.find(object->GetObjectId());
    if (known != g_ipc_names.end()) {
        return known->second;
    }
    const std::string prefix = service + "/" + IpcKindOf(object);
    const std::string name = fmt::format("{}#{}", prefix, g_ipc_counts[prefix]++);
    g_ipc_names[object->GetObjectId()] = name;
    return name;
}

/// The names of the handles of PART, in the client's handle table: `0` for a null handle, `-` for one
/// that is not there (or, for a request, that the trace has no name for).
std::string IpcHandleToken(Kernel::Process* process, const std::vector<u32>& words, const IpcPart& part,
                           const std::string* service, const std::map<u32, std::string>* snapshot) {
    std::string token;
    for (u32 j = 0; j < part.size; ++j) {
        const std::size_t at = part.index + 1 + j;
        std::string name = "-";
        if (at < words.size() && words[at] == 0) {
            name = "0";
        } else if (at < words.size() && snapshot) {
            const auto it = snapshot->find(static_cast<u32>(at));
            if (it != snapshot->end()) {
                name = it->second;
            }
        } else if (at < words.size() && service && process) {
            if (const auto object = process->handle_table.GetGeneric(words[at])) {
                name = IpcNameHandedOut(object, *service);
            }
        }
        token += (j ? "+" : "") + name;
    }
    return token;
}

std::string IpcDescription(const char* kind, u32 index, u32 size, const std::string& token) {
    return fmt::format("{}:{}:{}:{}", kind, index, size, token);
}

std::string IpcCrcToken(bool ok, u32 crc) {
    return ok ? fmt::format("0x{:08x}", crc) : std::string("-");
}

const char* IpcMappedKind(u32 perms) {
    return perms == 1 ? "mapped-r" : perms == 2 ? "mapped-w" : perms == 3 ? "mapped-rw" : "mapped-0";
}

void WriteIpcLine(Core::System& system, const IpcPending& p, const std::vector<u32>* reply,
                  const std::vector<std::string>& outdesc) {
    std::string line = fmt::format(
        "{} seq={} session={} cmd={} in={} desc={} out={} outdesc={} t={}", p.thread, p.seq, p.session,
        p.have_request ? fmt::format("0x{:08x}", p.in_words[0]) : std::string("-"),
        p.have_request ? IpcWords(p.in_words) : std::string("-"), p.have_request ? IpcJoin(p.desc) : std::string("-"),
        reply ? IpcWords(*reply) : std::string("-"), reply ? IpcJoin(outdesc) : std::string("-"),
        static_cast<long long>(system.CoreTiming().GetGlobalTicks() - g_start_ticks));
    for (const std::string& data : p.data) {
        line += " data=" + data;
    }
    line += '\n';
    std::fputs(line.c_str(), g_ipc_trace);
}

/// The thread sends the request: its number, the session, and the names of the handles it passes (a
/// handle the request moves is gone from the table once the service has the command).
void IpcSent(Core::System& system, const IPCDebugger::RequestRecord& r) {
    const u32 thread_id = static_cast<u32>(r.client_thread.id);
    for (auto it = g_ipc_pending.begin(); it != g_ipc_pending.end();) {
        if (it->second.thread == thread_id) { // a request that never got a reply (the call failed)
            WriteIpcLine(system, it->second, nullptr, {});
            it = g_ipc_pending.erase(it);
        } else {
            ++it;
        }
    }
    IpcPending p;
    p.seq = ++g_ipc_seq;
    p.thread = thread_id;
    p.process_id = static_cast<u32>(r.client_process.id);
    p.session = IpcSessionName(r);
    p.service = IpcServiceOf(p.session);
    const auto* thread = system.Kernel().GetCurrentThreadManager().GetCurrentThread();
    std::shared_ptr<Kernel::Process> process;
    if (thread) {
        process = thread->owner_process.lock();
    }
    if (thread && process && thread->GetThreadId() == thread_id) {
        auto& memory = system.Memory();
        const VAddr buffer = thread->GetCommandBufferAddress();
        u32 header = 0;
        if (memory.IsValidVirtualAddress(*process, buffer)) {
            memory.ReadBlock(*process, buffer, &header, sizeof(header));
            std::vector<u32> command(std::min<u32>(1 + ((header >> 6) & 0x3F) + (header & 0x3F), kIpcCommandWords));
            if (memory.IsValidVirtualAddress(*process, buffer + static_cast<u32>(command.size() * 4 - 1))) {
                memory.ReadBlock(*process, buffer, command.data(), command.size() * 4);
                for (const IpcPart& part : ParseIpcCommand(command)) {
                    if (part.type != IpcPart::Type::Handles) {
                        continue;
                    }
                    for (u32 j = 0; j < part.size; ++j) {
                        const std::size_t at = part.index + 1 + j;
                        if (command[at] != 0) {
                            const auto object = process->handle_table.GetGeneric(command[at]);
                            const auto known = object ? g_ipc_names.find(object->GetObjectId()) : g_ipc_names.end();
                            p.in_handles[static_cast<u32>(at)] = known != g_ipc_names.end() ? known->second : "-";
                        }
                    }
                }
            }
        }
    }
    g_ipc_pending[r.id] = std::move(p);
}

/// The service has the command: the words the client wrote, and the CRC-32 of every input buffer.
void IpcRequestSeen(Core::System& system, const IPCDebugger::RequestRecord& r) {
    const auto it = g_ipc_pending.find(r.id);
    if (it == g_ipc_pending.end() || r.untranslated_request_cmdbuf.empty()) {
        return;
    }
    IpcPending& p = it->second;
    p.have_request = true;
    p.is_hle = r.is_hle;
    p.in_words = r.untranslated_request_cmdbuf;
    p.in_parts = ParseIpcCommand(p.in_words);
    const auto process = system.Kernel().GetProcessById(p.process_id);
    for (const IpcPart& part : p.in_parts) {
        switch (part.type) {
        case IpcPart::Type::Handles:
            p.desc.push_back(IpcDescription(part.move ? "move" : "copy", part.index, part.size,
                                            IpcHandleToken(process.get(), p.in_words, part, nullptr, &p.in_handles)));
            break;
        case IpcPart::Type::Pid:
            p.desc.push_back(IpcDescription("pid", part.index, part.size, "-"));
            break;
        case IpcPart::Type::Pxi:
            p.desc.push_back(IpcDescription("pxi", part.index, part.size, "-"));
            break;
        case IpcPart::Type::Static:
        case IpcPart::Type::Mapped: {
            const bool is_static = part.type == IpcPart::Type::Static;
            const bool readable = is_static || (part.perms & 1);
            u32 crc = 0;
            std::string hex;
            const bool ok = readable && process && CrcGuest(system, *process, part.address, part.size, crc,
                                                            g_ipc_full ? &hex : nullptr);
            p.desc.push_back(IpcDescription(is_static ? "static" : IpcMappedKind(part.perms), part.index, part.size,
                                            IpcCrcToken(ok, crc)));
            if (ok && g_ipc_full) {
                p.data.push_back(fmt::format("{}:{}", part.index, hex));
            }
            break;
        }
        }
    }
}

/// The reply is written: the words the client reads, the CRC-32 of the static buffers it wrote and of
/// the request's writable mapped buffers, the names of the handles it handed out; then the line.
void IpcReplied(Core::System& system, const IPCDebugger::RequestRecord& r) {
    const auto it = g_ipc_pending.find(r.id);
    if (it == g_ipc_pending.end()) {
        return;
    }
    IpcPending& p = it->second;
    const std::vector<u32>& reply = r.translated_reply_cmdbuf; // the client's view of the reply
    const auto process = system.Kernel().GetProcessById(p.process_id);
    std::vector<std::string> outdesc;
    for (const IpcPart& part : ParseIpcCommand(reply)) {
        switch (part.type) {
        case IpcPart::Type::Handles:
            outdesc.push_back(IpcDescription(part.move ? "move" : "copy", part.index, part.size,
                                             IpcHandleToken(process.get(), reply, part, &p.service, nullptr)));
            break;
        case IpcPart::Type::Pid:
            outdesc.push_back(IpcDescription("pid", part.index, part.size, "-"));
            break;
        case IpcPart::Type::Static: {
            // an HLE service has written the bytes by now; a game's own server writes them after this
            u32 crc = 0;
            const bool ok = p.is_hle && process && CrcGuest(system, *process, part.address, part.size, crc, nullptr);
            outdesc.push_back(IpcDescription("static", part.index, part.size, IpcCrcToken(ok, crc)));
            break;
        }
        case IpcPart::Type::Mapped: // the request's own descriptor covers it
        case IpcPart::Type::Pxi:
            break;
        }
    }
    for (const IpcPart& part : p.in_parts) {
        if (part.type == IpcPart::Type::Mapped && (part.perms & 2)) {
            u32 crc = 0;
            const bool ok = p.is_hle && process && CrcGuest(system, *process, part.address, part.size, crc, nullptr);
            outdesc.push_back(IpcDescription(IpcMappedKind(part.perms), part.index, part.size, IpcCrcToken(ok, crc)));
        }
    }
    WriteIpcLine(system, p, &reply, outdesc);
    g_ipc_pending.erase(it);
}

void IpcRecorded(const IPCDebugger::RequestRecord& r) {
    std::lock_guard<std::mutex> lock(g_ipc_mutex);
    if (!g_ipc_trace) {
        return;
    }
    auto& system = Core::System::GetInstance();
    if (r.status == IPCDebugger::RequestStatus::Sent) {
        IpcSent(system, r);
    } else if (r.translated_reply_cmdbuf.empty()) {
        IpcRequestSeen(system, r);
    } else {
        IpcReplied(system, r);
    }
}

/// With --trace-ipc alone, the thread lines come from these (the system call trace's observers
/// write them to both files when it is on too).
std::map<u32, std::array<u32, 7>> g_ipc_create_regs; // CreateThread's registers, by calling thread

void IpcThreadSvc(Core::System& system, u32 imm) {
    if (imm == 0x08) {
        auto& cpu = system.GetRunningCore();
        std::array<u32, 7> regs{};
        for (std::size_t i = 0; i < regs.size(); ++i) {
            regs[i] = cpu.GetReg(static_cast<int>(i));
        }
        g_ipc_create_regs[CurrentThreadId(system)] = regs;
    }
}

void IpcThreadSvcDone(Core::System& system, u32 imm) {
    if (imm == 0x08) {
        auto& cpu = system.GetRunningCore();
        const u32 id = CurrentThreadId(system);
        const auto it = g_ipc_create_regs.find(id);
        if (it != g_ipc_create_regs.end() && cpu.GetReg(0) == 0) {
            NoteCreatedThread(system, id, it->second, cpu.GetReg(1));
        }
        g_ipc_create_regs.erase(id);
    }
}

bool BeginIpcTrace(Core::System& system, const std::string& path, bool full, const std::string& cpu, bool new_3ds) {
    g_ipc_trace = std::fopen(path.c_str(), "w");
    if (!g_ipc_trace) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    g_ipc_full = full;
    WriteHeader(g_ipc_trace, system, "ipctrace", cpu, new_3ds);
    WriteExistingThreadLines(g_ipc_trace, system);
    if (!g_svc_trace) {
        Kernel::SetSVCObserver(IpcThreadSvc);
        Kernel::SetSVCPostObserver(IpcThreadSvcDone);
    }
    system.Kernel().GetIPCRecorder().SetEnabled(true);
    g_ipc_callback = system.Kernel().GetIPCRecorder().BindCallback(IpcRecorded);
    return true;
}

void EndIpcTrace(Core::System& system) {
    auto& recorder = system.Kernel().GetIPCRecorder();
    if (!g_hid_capture_active) {
        recorder.SetEnabled(false);
    }
    recorder.UnbindCallback(g_ipc_callback);
    std::lock_guard<std::mutex> lock(g_ipc_mutex);
    if (!g_svc_trace) {
        Kernel::SetSVCObserver(nullptr);
        Kernel::SetSVCPostObserver(nullptr);
    }
    for (const auto& [id, pending] : g_ipc_pending) {
        WriteIpcLine(system, pending, nullptr, {}); // requests still waiting for a reply when the run ended
    }
    g_ipc_pending.clear();
    std::fclose(g_ipc_trace);
    g_ipc_trace = nullptr;
}

// ── DSP and HID captures (N0-06) ───────────────────────────────────────────────────
//
// Derived from Azahar src/audio_core/hle/hle.cpp, src/audio_core/hle/shared_memory.h,
// src/audio_core/hle/source.cpp, src/audio_core/hle/common.h, src/audio_core/audio_types.h,
// src/core/hle/service/hid/hid.h, src/core/hle/service/ir/ir_user.cpp,
// src/core/hle/kernel/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of those files says "Licensed under GPLv2 or any later version"):
// the sizes and offsets of the DSP shared-memory regions and the layout of the IR header follow
// those headers; the three hooks the DSP section uses are in azahar.patch. This section only.
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (audio_core/hle/shared_memory.h)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (source.cpp)
// Copyright 2016 Citra Emulator Project (common.h); 2017 (audio_types.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.h)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (ir_user.cpp)
// Copyright 2014 Citra Emulator Project (kernel/shared_memory.h)
//
// --capture-dsp START:FRAMES:FILE and --capture-hid FILE write the binary captures that
// tools/native/capfmt.py reads (its docstring defines the records): magic, u32 version 1, then
// {u32 type, u32 frame, u64 ticks, u32 size, payload}, little-endian (this host is), and a sibling
// FILE.json manifest. The frame is the game's frame counter read when the record is written.

constexpr u32 kDspMemoryRegion0 = 0x50000; // HLE::DspMemory::region_0 in the 0x80000-byte DSP memory
constexpr u32 kDspMemoryRegion1 = 0x70000; // region_1: 0x50000 + 0x8000 + 0x18000 of padding
constexpr u32 kDspRegionSize = 0x8000;
constexpr u32 kDspPcmBytes = 160 * 2 * 2; // one audio frame: 160 stereo s16 samples
constexpr u32 kHidReaderBlock = 0x0110e488; // the game's HID reader block (docs/research/system-services.md)
constexpr u32 kHidMappedPointer = kHidReaderBlock + 0x34; // where it keeps the mapped shared memory's address
constexpr u32 kHidSize = 0x2b0;
constexpr u32 kIrMaxBytes = 0x200;

/// An open capture file with its record counts (index = record type).
struct CaptureFile {
    std::FILE* file = nullptr;
    std::string path;
    const char* magic = "";
    u32 frames = 0; // audio frames (DSP) or game frames (HID)
    std::array<u32, 4> counts{};
};

bool CaptureOpen(CaptureFile& cap, const std::string& path, const char* magic) {
    cap.file = std::fopen(path.c_str(), "wb");
    if (!cap.file) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    cap.path = path;
    cap.magic = magic;
    const u32 version = 1;
    std::fwrite(magic, 1, 4, cap.file);
    std::fwrite(&version, 4, 1, cap.file);
    return true;
}

void CaptureRecord(Core::System& system, CaptureFile& cap, u32 type, u32 frame, const void* data, u32 size) {
    const u64 ticks = static_cast<u64>(system.CoreTiming().GetGlobalTicks() - g_start_ticks);
    std::fwrite(&type, 4, 1, cap.file);
    std::fwrite(&frame, 4, 1, cap.file);
    std::fwrite(&ticks, 8, 1, cap.file);
    std::fwrite(&size, 4, 1, cap.file);
    if (size) {
        std::fwrite(data, 1, size, cap.file);
    }
    ++cap.counts[type];
}

/// Closes the file and writes FILE.json: tool, format, version, game (title id), frames, record counts.
void CaptureClose(Core::System& system, CaptureFile& cap) {
    if (!cap.file) {
        return;
    }
    std::fclose(cap.file);
    cap.file = nullptr;
    u64 program_id = 0;
    if (system.GetAppLoader().ReadProgramId(program_id) != Loader::ResultStatus::Success) {
        program_id = 0;
    }
    if (std::FILE* manifest = std::fopen((cap.path + ".json").c_str(), "w")) {
        std::fprintf(manifest,
                     "{\n  \"tool\": \"mh4u_runner\",\n  \"format\": \"%s\",\n  \"version\": 1,\n"
                     "  \"game\": \"0x%016llx\",\n  \"frames\": %u,\n"
                     "  \"records\": {\"1\": %u, \"2\": %u, \"3\": %u}\n}\n",
                     cap.magic, static_cast<unsigned long long>(program_id), cap.frames, cap.counts[1],
                     cap.counts[2], cap.counts[3]);
        std::fclose(manifest);
    }
}

/// The game's frame counter now (not the value of the last slice), 0 if it can't be read.
u32 CaptureFrame(Core::System& system) {
    u32 frame = 0;
    return ReadGuest(system, kFrameCounterAddress, 4, frame) ? frame : g_frame;
}

/// `bytes` bytes of guest memory from ADDRESS into OUT, if the whole range is mapped (nothing is
/// logged or counted otherwise).
bool ReadGuestBytes(Core::System& system, u32 address, u32 size, u8* out) {
    const auto process = system.Kernel().GetCurrentProcess();
    if (!process || address + size < address) {
        return false;
    }
    auto& memory = system.Memory();
    for (u64 page = address; page < static_cast<u64>(address) + size; page = (page | 0xFFF) + 1) {
        if (!memory.IsValidVirtualAddress(*process, static_cast<u32>(page))) {
            return false;
        }
    }
    memory.ReadBlock(*process, address, out, size);
    return true;
}

// DSP: the hooks of AudioCore::HLE::g_capture_hooks (azahar.patch) run on the emulation thread,
// from the DSP's audio event: frame_start, the sample reads while the frame is generated,
// frame_output. Records of one frame: 1, then the 3s, then 2.
struct DspCapture {
    CaptureFile cap;
    std::mutex mutex;
    u32 start_frame = 0;
    u32 frames = 0;
    bool started = false;
    bool in_frame = false;
    std::set<std::tuple<u32, u32, u32>> seen; // (address, size, CRC-32) whose bytes are in the file
    u32 frame_now = 0;
} g_dsp_capture;

void DspFrameStart(const u8* dsp_memory) {
    auto& c = g_dsp_capture;
    std::lock_guard lock{c.mutex};
    auto& system = Core::System::GetInstance();
    if (!c.cap.file) {
        return;
    }
    c.frame_now = CaptureFrame(system);
    if (!c.started && c.frame_now < c.start_frame) {
        return;
    }
    c.started = true;
    c.in_frame = true;
    std::vector<u8> both(2 * kDspRegionSize);
    std::memcpy(both.data(), dsp_memory + kDspMemoryRegion0, kDspRegionSize);
    std::memcpy(both.data() + kDspRegionSize, dsp_memory + kDspMemoryRegion1, kDspRegionSize);
    CaptureRecord(system, c.cap, 1, c.frame_now, both.data(), static_cast<u32>(both.size()));
}

void DspSampleRead(u32 address, u32 size, const u8* data) {
    auto& c = g_dsp_capture;
    std::lock_guard lock{c.mutex};
    if (!c.cap.file || !c.in_frame) {
        return;
    }
    const u32 crc = Crc32(data, size);
    std::vector<u8> payload(12);
    std::memcpy(payload.data(), &address, 4);
    std::memcpy(payload.data() + 4, &size, 4);
    std::memcpy(payload.data() + 8, &crc, 4);
    if (c.seen.insert({address, size, crc}).second) { // the bytes only the first time
        payload.insert(payload.end(), data, data + size);
    }
    CaptureRecord(Core::System::GetInstance(), c.cap, 3, c.frame_now, payload.data(), static_cast<u32>(payload.size()));
}

void DspFrameOutput(const s16* samples) {
    auto& c = g_dsp_capture;
    std::lock_guard lock{c.mutex};
    if (!c.cap.file || !c.in_frame) {
        return;
    }
    auto& system = Core::System::GetInstance();
    c.in_frame = false;
    CaptureRecord(system, c.cap, 2, c.frame_now, samples, kDspPcmBytes);
    if (++c.cap.frames >= c.frames) {
        CaptureClose(system, c.cap); // the hooks stay set and do nothing from now on
    }
}

/// START and FRAMES are decimal (START may be written f<N>); FILE is the rest.
bool ParseCaptureDsp(const std::string& arg, u32& start, u32& frames, std::string& file) {
    const auto first = arg.find(':');
    const auto second = first == std::string::npos ? first : arg.find(':', first + 1);
    if (second == std::string::npos || second + 1 >= arg.size()) {
        return false;
    }
    std::string start_text = arg.substr(0, first);
    if (!start_text.empty() && start_text[0] == 'f') {
        start_text.erase(0, 1);
    }
    const std::string frames_text = arg.substr(first + 1, second - first - 1);
    const auto numeric = [](const std::string& s) {
        return !s.empty() && s.size() <= 9 && std::all_of(s.begin(), s.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); });
    };
    if (!numeric(start_text) || !numeric(frames_text)) {
        return false;
    }
    start = static_cast<u32>(std::stoul(start_text));
    frames = static_cast<u32>(std::stoul(frames_text));
    file = arg.substr(second + 1);
    return frames > 0;
}

bool BeginDspCapture(const std::string& path, u32 start, u32 frames) {
    auto& c = g_dsp_capture;
    c.start_frame = start;
    c.frames = frames;
    if (!CaptureOpen(c.cap, path, "MDSP")) {
        return false;
    }
    auto& hooks = AudioCore::HLE::g_capture_hooks;
    hooks.frame_start = DspFrameStart;
    hooks.sample_read = DspSampleRead;
    hooks.frame_output = DspFrameOutput;
    return true;
}

void EndDspCapture(Core::System& system) {
    auto& hooks = AudioCore::HLE::g_capture_hooks;
    hooks = {};
    std::lock_guard lock{g_dsp_capture.mutex};
    CaptureClose(system, g_dsp_capture.cap);
}

// HID: after each slice of emulation, once per change of the frame counter: the HID shared memory
// as the game maps it (record 1) and, when a game has set up ir:USER, the start of that
// shared memory (record 2). The IR memory is found from the request that handed it to ir:USER.
struct HidCapture {
    CaptureFile cap;
    bool have_frame = false;
    u32 last_frame = 0;
    std::shared_ptr<Kernel::SharedMemory> ir_memory;
    IPCDebugger::CallbackHandle callback;
} g_hid_capture;

/// ir:USER's Initialize requests carry the shared memory block as a handle: keep the first block.
void HidIpcRecorded(const IPCDebugger::RequestRecord& r) {
    auto& c = g_hid_capture;
    if (c.ir_memory || r.status == IPCDebugger::RequestStatus::Sent || !r.translated_reply_cmdbuf.empty() ||
        r.untranslated_request_cmdbuf.empty() || r.client_port.name.rfind("ir:USER", 0) != 0) {
        return;
    }
    const auto process = Core::System::GetInstance().Kernel().GetProcessById(static_cast<u32>(r.client_process.id));
    if (!process) {
        return;
    }
    const std::vector<u32>& words = r.untranslated_request_cmdbuf;
    for (const IpcPart& part : ParseIpcCommand(words)) {
        if (part.type != IpcPart::Type::Handles) {
            continue;
        }
        for (u32 j = 0; j < part.size && !c.ir_memory; ++j) {
            const std::size_t at = part.index + 1 + j;
            if (at < words.size() && words[at] != 0) {
                c.ir_memory = Kernel::DynamicObjectCast<Kernel::SharedMemory>(process->handle_table.GetGeneric(words[at]));
            }
        }
    }
}

bool BeginHidCapture(Core::System& system, const std::string& path) {
    if (!CaptureOpen(g_hid_capture.cap, path, "MHID")) {
        return false;
    }
    auto& recorder = system.Kernel().GetIPCRecorder();
    g_hid_capture_active = true;
    recorder.SetEnabled(true);
    g_hid_capture.callback = recorder.BindCallback(HidIpcRecorded);
    return true;
}

void WriteHidCapture(Core::System& system) {
    auto& c = g_hid_capture;
    if (!c.cap.file) {
        return;
    }
    u32 frame = 0;
    if (!ReadGuest(system, kFrameCounterAddress, 4, frame) || (c.have_frame && frame == c.last_frame)) {
        return;
    }
    c.have_frame = true;
    c.last_frame = frame;
    std::array<u8, kHidSize> hid{}; // zeros until the game has mapped the shared memory
    u32 mapped = 0;
    if (ReadGuest(system, kHidMappedPointer, 4, mapped) && mapped != 0) {
        ReadGuestBytes(system, mapped, kHidSize, hid.data());
    }
    CaptureRecord(system, c.cap, 1, frame, hid.data(), kHidSize);
    if (c.ir_memory) {
        const u32 size = static_cast<u32>(std::min<u64>(c.ir_memory->GetSize(), kIrMaxBytes));
        if (size >= 16) {
            CaptureRecord(system, c.cap, 2, frame, c.ir_memory->GetPointer(), size);
        }
    }
    ++c.cap.frames;
}

void EndHidCapture(Core::System& system) {
    auto& c = g_hid_capture;
    if (!c.cap.file) {
        return;
    }
    auto& recorder = system.Kernel().GetIPCRecorder();
    recorder.UnbindCallback(c.callback);
    g_hid_capture_active = false;
    if (!g_ipc_trace) {
        recorder.SetEnabled(false);
    }
    c.ir_memory.reset();
    CaptureClose(system, c.cap);
}

// ── screenshots ─────────────────────────────────────────────────────────────────────

/// Writes one screen of the software renderer as a binary PPM (landscape, as displayed).
bool SaveScreen(Core::System& system, VideoCore::ScreenId id, const std::string& path) {
    auto* renderer = dynamic_cast<SwRenderer::RendererSoftware*>(&system.GPU().Renderer());
    if (!renderer) {
        return false;
    }
    const auto& info = renderer->Screen(id);
    if (info.pixels.empty()) {
        return false;
    }
    // The renderer stores the screen rotated: info.height pixels per output row.
    const u32 width = info.height, height = info.width;
    std::vector<u8> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        std::memcpy(&rgb[i * 3], &info.pixels[i * 4], 3);
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

// ── network room ────────────────────────────────────────────────────────────────────

/// A new random console ID, saved in the user folder's system settings.
void NewConsoleId(Core::System& system) {
    const auto cfg = Service::CFG::GetModule(system);
    const auto [random_number, console_id] = cfg->GenerateConsoleUniqueId();
    cfg->SetConsoleUniqueId(random_number, console_id);
    cfg->UpdateConfigNANDSavegame();
    std::printf("console id %016llx\n", static_cast<unsigned long long>(console_id));
}

/// Hosts a room on `host_port` (when it isn't 0) and joins it, or joins the room at `join`
/// (HOST:PORT), the way Azahar's Multiplayer menu does. Waits until the room has accepted
/// or refused this member.
bool JoinRoom(Core::System& system, u16 host_port, const std::string& join,
              const std::string& nickname) {
    if (!Network::Init()) {
        std::fprintf(stderr, "room: the network library didn't start\n");
        return false;
    }
    std::string host = "127.0.0.1";
    u16 port = host_port;
    if (host_port) {
        const auto room = Network::GetRoom().lock();
        if (!room || !room->Create("mh4u_runner", "", "", host_port, "", 16, "", "", 0,
                                   std::make_unique<Network::VerifyUser::NullBackend>())) {
            std::fprintf(stderr, "room: could not host on port %u\n", host_port);
            return false;
        }
        std::printf("room: hosting on port %u\n", host_port);
        std::fflush(stdout);
    } else {
        const auto colon = join.rfind(':');
        host = join.substr(0, colon);
        port = static_cast<u16>(std::atoi(join.c_str() + colon + 1));
    }
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        return false;
    }
    member->BindOnError([](const Network::RoomMember::Error& error) {
        std::printf("room: %s\n", Network::GetErrorStr(error));
        std::fflush(stdout);
    });
    member->Join(nickname, Service::CFG::GetConsoleIdHash(system), host.c_str(), port);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (member->GetState() == Network::RoomMember::State::Joining &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!member->IsConnected()) {
        std::fprintf(stderr, "room: could not join %s:%u (%s)\n", host.c_str(), port,
                     Network::GetStateStr(member->GetState()));
        return false;
    }
    std::printf("room: joined %s:%u as %s, MAC %s\n", host.c_str(), port, nickname.c_str(),
                Service::CFG::MacToString(member->GetMacAddress()).c_str());
    std::fflush(stdout);
    return true;
}

/// The room's members as this runner last heard of them, sorted, for the log.
void PrintRoomMembers() {
    const auto member = Network::GetRoomMember().lock();
    if (!member || !member->IsConnected()) {
        return;
    }
    std::vector<std::string> names;
    for (const auto& info : member->GetMemberInformation()) {
        names.push_back(info.nickname);
    }
    std::sort(names.begin(), names.end());
    std::string list;
    for (const auto& name : names) {
        list += (list.empty() ? "" : ", ") + name;
    }
    std::printf("room: members: %s\n", list.c_str());
}

} // namespace

/// --overlay-layout: the screens' rectangles for a touch_overlay option and window size, and
/// with a quest_overlay option and the map's panel slot, what a quest shows: the part of the
/// touch screen in the corner and where a click in its middle touches.
int PrintOverlayLayout(const std::string& spec, const std::string& size, const std::string& quest,
                       const std::string& slot, const std::string& target, const std::string& target_slot) {
    unsigned width = 0, height = 0, top_width = Core::kScreenTopWidth;
    if (std::sscanf(size.c_str(), "%ux%u:%u", &width, &height, &top_width) < 2 || !width ||
        !height || !top_width) {
        return Usage();
    }
    Layout::SetTopScreenWidth(top_width);
    auto overlay = Port::Hooks::ParseOverlay(spec);
    if (!quest.empty()) {
        const auto map = Port::Hooks::ParseQuestOverlay(quest);
        if (map.on) {
            overlay = Port::Hooks::QuestOverlay(overlay, std::atoi(slot.c_str()), map,
                                                target.empty() ? -1 : std::atoi(target_slot.c_str()),
                                                Port::Hooks::ParseTargetOverlay(target));
        }
    }
    Layout::SetScreenOverlay(overlay);
    const auto layout = Layout::CustomFrameLayout(width, height, false, false);
    const auto& t = layout.top_screen;
    const auto& b = layout.bottom_screen;
    std::printf("top %u,%u %u,%u opacity %.2f\nbottom %u,%u %u,%u opacity %.2f\n", t.left, t.top,
                t.right, t.bottom, layout.top_opacity, b.left, b.top, b.right, b.bottom,
                layout.bottom_opacity);
    if (quest.empty()) {
        return 0;
    }
    const auto& crop = layout.bottom_crop;
    std::printf("shows %.0f,%.0f %.0f,%.0f of the touch screen\n", crop.left * Core::kScreenBottomWidth,
                crop.top * Core::kScreenBottomHeight, crop.right * Core::kScreenBottomWidth,
                crop.bottom * Core::kScreenBottomHeight);
    // a click through Azahar's own touch handling, in a window with the custom layout
    Settings::values.layout_option.SetValue(Settings::LayoutOption::CustomLayout);
    HeadlessWindow window;
    window.UpdateCurrentFramebufferLayout(width, height);
    const auto& shown = window.GetFramebufferLayout().bottom_screen;
    const unsigned x = (shown.left + shown.right) / 2, y = (shown.top + shown.bottom) / 2;
    window.TouchPressed(x, y);
    const auto touch = Input::CreateDevice<Input::TouchDevice>(
        Common::ParamPackage{{"engine", "emu_window"}}.Serialize());
    const auto [touch_x, touch_y, pressed] = touch->GetStatus();
    std::printf("a click at %u,%u touches %.0f,%.0f%s\n", x, y, touch_x * Core::kScreenBottomWidth,
                touch_y * Core::kScreenBottomHeight, pressed ? "" : " (not pressed)");
    window.TouchReleased();
    // the further views (the target camera): where, which part, and a click on each, then
    // dragged to the window's bottom-left corner (the touch stays on the view's part)
    const auto& framebuffer = window.GetFramebufferLayout();
    for (u32 i = 0; i < framebuffer.bottom_view_count; ++i) {
        const auto& [r, part] = framebuffer.bottom_views[i];
        std::printf("view %u,%u %u,%u shows %.0f,%.0f %.0f,%.0f of the touch screen\n", r.left, r.top,
                    r.right, r.bottom, part.left * Core::kScreenBottomWidth, part.top * Core::kScreenBottomHeight,
                    part.right * Core::kScreenBottomWidth, part.bottom * Core::kScreenBottomHeight);
        const unsigned vx = (r.left + r.right) / 2, vy = (r.top + r.bottom) / 2;
        window.TouchPressed(vx, vy);
        const auto [view_x, view_y, view_pressed] = touch->GetStatus();
        std::printf("a click at %u,%u touches %.0f,%.0f%s\n", vx, vy, view_x * Core::kScreenBottomWidth,
                    view_y * Core::kScreenBottomHeight, view_pressed ? "" : " (not pressed)");
        window.TouchMoved(0, height - 1);
        const auto [drag_x, drag_y, drag_pressed] = touch->GetStatus();
        std::printf("dragged to 0,%u it touches %.0f,%.0f%s\n", height - 1, drag_x * Core::kScreenBottomWidth,
                    drag_y * Core::kScreenBottomHeight, drag_pressed ? "" : " (not pressed)");
        window.TouchReleased();
    }
    return 0;
}

/// --quest-overlay-frames: runs the frames of a made-up quest at FPS frames a second through
/// the choice of what the touch overlay shows (Port::Hooks::QuestOverlayChoice) and prints each
/// change. SPANS: KIND:SECONDS,... with KIND map0..map5 (the small panels up, the map in that
/// slot), nomap (up, with no map), drawn (not running but on the touch screen: someone
/// talking), none (not there: a change of area), menu (the quest menu or a big panel) or off
/// (F12: the whole touch screen, the panels up).
int PrintQuestOverlayFrames(const std::string& fps_text, const std::string& spans) {
    using Choice = Port::Hooks::QuestOverlayChoice;
    const double fps = std::atof(fps_text.c_str());
    if (fps <= 0) {
        return Usage();
    }
    Choice choice;
    long long frame = 0;
    double span_end = 0;
    std::stringstream list(spans);
    std::string span;
    while (std::getline(list, span, ',')) {
        const auto colon = span.find(':');
        if (colon == std::string::npos) {
            return Usage();
        }
        const std::string kind = span.substr(0, colon);
        bool panels = false, map_drawn = false, touch_ui = false;
        int slot = -1, forced = Choice::FREE;
        if (kind.size() == 4 && kind.compare(0, 3, "map") == 0 && kind[3] >= '0' && kind[3] <= '5') {
            panels = true;
            slot = kind[3] - '0';
        } else if (kind == "nomap") {
            panels = true;
        } else if (kind == "drawn") {
            map_drawn = true;
        } else if (kind == "menu") {
            touch_ui = true;
        } else if (kind == "off") {
            panels = true;
            slot = 0;
            forced = -1;
        } else if (kind != "none") {
            return Usage();
        }
        span_end += std::atof(span.c_str() + colon + 1);
        for (; static_cast<double>(frame) / fps < span_end - 1e-9; ++frame) {
            const double time = static_cast<double>(frame) / fps;
            if (choice.Update(time, panels, slot, map_drawn, touch_ui, forced)) {
                if (choice.Shown() < 0) {
                    std::printf("%.3f s: the whole touch screen\n", time);
                } else {
                    std::printf("%.3f s: the map in slot %d\n", time, choice.Shown());
                }
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--panel-view") {
        // a quest_overlay or target_overlay value as the hooks read it and the menu writes it
        const std::string key = argv[2];
        if (key != "quest_overlay" && key != "target_overlay") {
            return Usage();
        }
        const bool map = key == "quest_overlay";
        const auto view = map ? Port::Hooks::ParseQuestOverlay(argv[3]) : Port::Hooks::ParseTargetOverlay(argv[3]);
        std::printf("%s\n", (map ? Port::Hooks::FormatQuestOverlay(view) : Port::Hooks::FormatTargetOverlay(view)).c_str());
        return 0;
    }
    if ((argc == 4 || argc == 6 || argc == 8) && std::string(argv[1]) == "--overlay-layout") {
        return PrintOverlayLayout(argv[2], argv[3], argc >= 6 ? argv[4] : "", argc >= 6 ? argv[5] : "",
                                  argc == 8 ? argv[6] : "", argc == 8 ? argv[7] : "");
    }
    if (argc == 4 && std::string(argv[1]) == "--quest-overlay-frames") {
        return PrintQuestOverlayFrames(argv[2], argv[3]);
    }
    std::string path;
    std::string code_library;
    std::string cpu = "recomp";
    std::string log_filter = "*:Warning Debug.Emulated:Debug Kernel:Info";
    std::string user_dir, shots_dir, svc_trace, ipc_trace;
    bool ipc_trace_full = false;
    bool print_schedule = false;
    std::string schedule_error;
    std::string capture_dsp_file, capture_hid_file;
    u32 capture_dsp_start = 0, capture_dsp_frames = 0;
    std::string state_trace, unmapped_report, observable_file = "config/native/observable.txt";
    std::vector<std::string> install;
    double shot_every = 1000.0;
    double speed = 0.0;
    double draw_window = -1.0; // < 0: draw every frame
    double stall_ms = 0.0;     // > 0: report emulation steps that take longer
    int cpu_clock = 100;
    bool async_io = false;
    int room_host = 0;
    std::string room_join;
    std::string nickname = "runner";
    bool new_console_id = false;
    bool new_3ds = false; // the reference is an Old 3DS
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--code" && i + 1 < argc) {
            code_library = argv[++i];
        } else if (arg == "--cpu" && i + 1 < argc) {
            cpu = argv[++i];
        } else if (arg == "--ms" && i + 1 < argc) {
            if (!Schedule::ParseStop(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --ms %s (%s)\n", argv[i], schedule_error.c_str());
                return 1;
            }
        } else if (arg == "--frames" && i + 1 < argc) {
            if (!Schedule::ParseFrames(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --frames %s (%s)\n", argv[i], schedule_error.c_str());
                return 1;
            }
        } else if (arg == "--log" && i + 1 < argc) {
            log_filter = argv[++i];
        } else if (arg == "--user" && i + 1 < argc) {
            user_dir = argv[++i];
        } else if (arg == "--install" && i + 1 < argc) {
            install.push_back(argv[++i]);
        } else if (arg == "--shots" && i + 1 < argc) {
            shots_dir = argv[++i];
        } else if (arg == "--lockstep" && i + 1 < argc) {
            if (!ParseLockstep(argv[++i])) {
                std::fprintf(stderr, "bad --lockstep: %s (want FILE:COUNT:INDEX, 2 <= COUNT <= 16)\n", argv[i]);
                return 1;
            }
        } else if (arg == "--stalls" && i + 1 < argc) {
            stall_ms = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--freeze" && i + 1 < argc) {
            if (!Schedule::ParseFreeze(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --freeze %s (MS:HOST_MS; %s)\n", argv[i], schedule_error.c_str());
                return 1;
            }
        } else if (arg == "--set" && i + 1 < argc) {
            if (!Schedule::ParseOptionChange(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --set %s (MS:KEY=VALUE)\n", argv[i]);
                return 1;
            }
        } else if (arg == "--replace" && i + 1 < argc) {
            if (!Schedule::ParseReplace(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --replace: %s (want MS:FROM:TO, hex bytes of equal length)\n", argv[i]);
                return 1;
            }
        } else if (arg == "--dump" && i + 1 < argc) {
            if (!Schedule::ParseDump(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --dump: %s (want MS:ADDR:SIZE, hex address and size)\n", argv[i]);
                return 1;
            }
        } else if (arg == "--call-counts" && i + 1 < argc) {
            if (!Schedule::ParseCallCounts(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --call-counts %s (%s)\n", argv[i], schedule_error.c_str());
                return 1;
            }
        } else if (arg == "--trace-svc" && i + 1 < argc) {
            svc_trace = argv[++i];
        } else if (arg == "--trace-ipc" && i + 1 < argc) {
            ipc_trace = argv[++i];
        } else if (arg == "--trace-ipc-full") {
            ipc_trace_full = true;
        } else if (arg == "--capture-dsp" && i + 1 < argc) {
            if (!ParseCaptureDsp(argv[++i], capture_dsp_start, capture_dsp_frames, capture_dsp_file)) {
                std::fprintf(stderr, "bad --capture-dsp: %s (want START:FRAMES:FILE, START a game frame, FRAMES above 0)\n", argv[i]);
                return 1;
            }
        } else if (arg == "--capture-hid" && i + 1 < argc) {
            capture_hid_file = argv[++i];
        } else if (arg == "--state-trace" && i + 1 < argc) {
            state_trace = argv[++i];
        } else if (arg == "--observable" && i + 1 < argc) {
            observable_file = argv[++i];
        } else if (arg == "--count-unmapped" && i + 1 < argc) {
            unmapped_report = argv[++i];
        } else if (arg == "--new-3ds" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode != "on" && mode != "off") {
                std::fprintf(stderr, "bad --new-3ds: %s (want on or off)\n", mode.c_str());
                return 1;
            }
            new_3ds = mode == "on";
        } else if (arg == "--speed" && i + 1 < argc) {
            speed = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--shots-from" && i + 1 < argc) {
            if (!Schedule::ParseShotsFrom(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --shots-from %s (%s)\n", argv[i], schedule_error.c_str());
                return 1;
            }
        } else if (arg == "--shot-every" && i + 1 < argc) {
            shot_every = std::max(1.0, std::atof(argv[++i]));
        } else if (arg == "--cpu-clock" && i + 1 < argc) {
            cpu_clock = std::clamp(std::atoi(argv[++i]), 5, 400);
        } else if (arg == "--async-io") {
            async_io = true;
        } else if (arg == "--draw-window" && i + 1 < argc) {
            draw_window = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--room-host" && i + 1 < argc) {
            room_host = std::atoi(argv[++i]);
            if (room_host <= 0 || room_host > 65535) {
                return Usage();
            }
        } else if (arg == "--room-join" && i + 1 < argc) {
            room_join = argv[++i];
            const auto colon = room_join.rfind(':');
            if (colon == std::string::npos || colon == 0 || std::atoi(room_join.c_str() + colon + 1) <= 0) {
                return Usage();
            }
        } else if (arg == "--nick" && i + 1 < argc) {
            nickname = argv[++i];
            // what rooms accept; anything else comes back as a name collision
            if (nickname.size() < 4 || nickname.size() > 20 ||
                !std::all_of(nickname.begin(), nickname.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || std::strchr(" ._-", c);
                })) {
                std::fprintf(stderr, "bad --nick: %s\n", nickname.c_str());
                return 1;
            }
        } else if (arg == "--new-console-id") {
            new_console_id = true;
        } else if (arg == "--input" && i + 1 < argc) {
            if (!Schedule::ParseInput(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --input script: %s (%s)\n", argv[i], schedule_error.c_str());
                return 1;
            }
        } else if (arg == "--input-file" && i + 1 < argc) {
            if (!Schedule::ParseInputFile(argv[++i], g_plan, schedule_error)) {
                std::fprintf(stderr, "bad --input-file %s: %s\n", argv[i], schedule_error.c_str());
                return 1;
            }
        } else if (arg == "--print-schedule") {
            print_schedule = true;
        } else if (!arg.empty() && arg[0] != '-' && path.empty()) {
            path = arg;
        } else {
            return Usage();
        }
    }
    if (print_schedule) {
        std::fputs(Schedule::Describe(g_plan).c_str(), stdout);
        return 0;
    }
    if (ipc_trace_full && ipc_trace.empty()) {
        std::fprintf(stderr, "--trace-ipc-full needs --trace-ipc FILE\n");
        return 1;
    }
    const bool install_only = path.empty() && !install.empty();
    if ((path.empty() && !install_only) || (cpu != "recomp" && cpu != "jit" && cpu != "interp") ||
        (room_host && !room_join.empty())) {
        return Usage();
    }

    if (!user_dir.empty()) {
        // before anything asks for a path: the other directories are derived from it once
        FileUtil::SetUserPath(user_dir.back() == '/' ? user_dir : user_dir + '/');
    }
    Common::Log::Initialize("mh4u_runner_log.txt"); // not over Azahar's own log in a shared folder
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    Common::Log::Filter filter;
    filter.ParseFilterString(log_filter);
    Common::Log::SetGlobalFilter(filter);

    for (const auto& cia : install) {
        const auto result = Service::AM::InstallCIA(cia);
        if (result != Service::AM::InstallStatus::Success) {
            std::fprintf(stderr, "installing %s failed (status %u)\n", cia.c_str(),
                         static_cast<unsigned>(result));
            return 1;
        }
        std::printf("installed %s\n", cia.c_str());
    }
    if (install_only) {
        Common::Log::Stop();
        return 0;
    }
    UseScriptedInput();
    Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::Software);
    Settings::values.frame_limit.SetValue(speed); // 0: don't wait for real time
    Settings::values.cpu_clock_percentage.SetValue(cpu_clock);
    // (the port's Azahar patch also keeps host time out of file delays in this mode)
    Settings::values.deterministic_async_operations.SetValue(!async_io);
    if (!async_io) {
        // the game seeds its random numbers from the clock: a fixed start date makes two runs
        // the same down to the monster's moves (2026-01-01 12:00 UTC)
        Settings::values.init_clock.SetValue(Settings::InitClock::FixedTime);
        Settings::values.init_time.SetValue(1767268800ULL);
    }
    // no sound card needed (and on Windows, cubeb's WASAPI backend asserts on a thread
    // without COM, which a runner without Qt never initialises)
    Settings::values.output_type.SetValue(AudioCore::SinkType::Null);
    Settings::values.input_type.SetValue(AudioCore::InputType::Null);
    Settings::values.is_new_3ds.SetValue(new_3ds); // before system.Load: it picks the cores and memory
    Settings::values.use_cpu_jit = cpu == "jit";
    Settings::values.use_fastinterp = false;

    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false); // all services HLE
    }

    Core::System& system = Core::System::GetInstance();
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());
    std::vector<Port::ARM_Recomp*> recomp_cores;
    if (cpu == "recomp") {
        std::string error;
        if (code_library.empty() || !Port::ARM_Recomp::LoadCode(code_library, error)) {
            std::fprintf(stderr, "%s\n", code_library.empty() ? "--cpu recomp needs --code LIB" : error.c_str());
            return 1;
        }
        if (!g_plan.call_counts.empty() && !Port::ARM_Recomp::HasCallCounts()) {
            std::fprintf(stderr, "--call-counts needs a code library built with -D RC_CALL_COUNTS\n");
            return 1;
        }
        system.SetCPUFactory([&recomp_cores](Core::System& sys, Memory::MemorySystem& memory,
                                             u32 core_id, std::shared_ptr<Core::Timing::Timer> timer) {
            auto core = std::make_shared<Port::ARM_Recomp>(sys, memory, core_id, std::move(timer));
            recomp_cores.push_back(core.get());
            return std::shared_ptr<Core::ARM_Interface>(core);
        });
    }

    // Before loading, as Azahar's Multiplayer menu does: the system settings are read when the
    // services start, and the local wireless service only talks to a room that exists when
    // it starts (it also takes the room's MAC address for the console's).
    if (new_console_id) {
        NewConsoleId(system);
    }
    const bool in_room = room_host || !room_join.empty();
    if (in_room && !JoinRoom(system, static_cast<u16>(room_host), room_join, nickname)) {
        Network::Shutdown();
        Common::Log::Stop();
        return 5;
    }

    HeadlessWindow window;
    const auto status = system.Load(window, path);
    if (status != Core::System::ResultStatus::Success) {
        std::fprintf(stderr, "failed to load %s (status %u)\n", path.c_str(), static_cast<unsigned>(status));
        return 2;
    }
    std::weak_ptr<Kernel::Process> process = system.Kernel().GetCurrentProcess();
    // ARM11 clock: 268,111,856 Hz
    // --frames without --ms: no time limit
    const bool no_time_limit = g_plan.has_stop_frame && !g_plan.has_stop_ms;
    const s64 tick_limit = no_time_limit ? std::numeric_limits<s64>::max() : static_cast<s64>(g_plan.stop_ms * 268111.856);
    const s64 start_ticks = system.CoreTiming().GetGlobalTicks();
    g_start_ticks = start_ticks;
    if (!svc_trace.empty()) {
        g_svc_trace = std::fopen(svc_trace.c_str(), "w");
        if (g_svc_trace) {
            BeginSvcTrace(system, cpu, new_3ds);
        }
    }
    if (!ipc_trace.empty() && !BeginIpcTrace(system, ipc_trace, ipc_trace_full, cpu, new_3ds)) {
        system.Shutdown();
        return 1;
    }
    if (!state_trace.empty() && !BeginStateTrace(system, state_trace, observable_file, cpu, new_3ds)) {
        system.Shutdown();
        return 1;
    }
    if (!capture_dsp_file.empty() && !BeginDspCapture(capture_dsp_file, capture_dsp_start, capture_dsp_frames)) {
        system.Shutdown();
        return 1;
    }
    if (!capture_hid_file.empty() && !BeginHidCapture(system, capture_hid_file)) {
        system.Shutdown();
        return 1;
    }
    if (!unmapped_report.empty()) {
        Memory::SetUnmappedObserver(CountUnmapped);
    }

    if (!shots_dir.empty()) {
        FileUtil::CreateFullPath(shots_dir.back() == '/' ? shots_dir : shots_dir + '/');
    }
    // --shots-from f<N>: the first shot is taken when the frame counter gets there (the file is named
    // f<N>_top.ppm); the next ones follow every --shot-every ms from then. Until then the
    // renderer draws whenever --draw-window would.
    bool shot_frame_pending = g_plan.has_shots_from && g_plan.shots_from.by_frame;
    double next_shot = g_plan.has_shots_from && !g_plan.shots_from.by_frame ? g_plan.shots_from.ms : shot_every;
    int rc = 0;
    while (true) {
        if (draw_window >= 0.0) {
            SwRenderer::SetDrawingEnabled(!shots_dir.empty() &&
                                          (shot_frame_pending || EmulatedMs() >= next_shot - draw_window));
        }
        LockstepWait(EmulatedMs());
        if (!ApplyOptionChanges()) {
            rc = 1;
            break;
        }
        for (auto& freeze : g_plan.freezes) {
            if (!freeze.done && freeze.when.Due(EmulatedMs(), g_frame)) {
                freeze.done = true;
                std::printf("freeze at %.0f ms for %.0f ms of host time\n", EmulatedMs(), freeze.host_ms);
                std::fflush(stdout);
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(freeze.host_ms));
            }
        }
        const auto step_start = std::chrono::steady_clock::now();
        const double step_emulated = EmulatedMs();
        const auto result = system.RunLoop();
        if (stall_ms > 0.0) {
            const double host =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - step_start).count();
            if (host > stall_ms) {
                std::printf("stall: %.0f ms of host time at %.1f ms, the emulated clock moved %.1f ms\n", host,
                            step_emulated, EmulatedMs() - step_emulated);
                std::fflush(stdout);
            }
        }
        UpdateFrame(system);
        WriteDumps(system, shots_dir);
        WriteStateTrace(system);
        WriteHidCapture(system);
        DoReplaces(system);
        WriteCallCounts(shots_dir);
        const bool frame_shot = shot_frame_pending && g_plan.shots_from.Due(EmulatedMs(), g_frame);
        if (!shots_dir.empty() && (frame_shot || (!shot_frame_pending && EmulatedMs() >= next_shot))) {
            const auto stem = frame_shot ? fmt::format("{}/{}", shots_dir, WhenStem(g_plan.shots_from))
                                         : fmt::format("{}/{:07.0f}", shots_dir, next_shot);
            SaveScreen(system, VideoCore::ScreenId::TopLeft, stem + "_top.ppm");
            SaveScreen(system, VideoCore::ScreenId::Bottom, stem + "_bottom.ppm");
            if (frame_shot) {
                shot_frame_pending = false;
                next_shot = std::floor(EmulatedMs()) + shot_every;
            } else {
                next_shot += shot_every;
            }
        }
        if (result == Core::System::ResultStatus::ShutdownRequested) {
            break;
        }
        if (result != Core::System::ResultStatus::Success) {
            std::fprintf(stderr, "emulation error (status %u)\n", static_cast<unsigned>(result));
            rc = 3;
            break;
        }
        bool failed = false;
        for (const auto* core : recomp_cores) {
            if (core->HasFailed()) {
                std::fprintf(stderr, "recompiled code stopped: %s\n", core->Failure().c_str());
                failed = true;
            }
        }
        if (failed) {
            rc = 4;
            break;
        }
        const auto p = process.lock();
        if (!p || p->status == Kernel::ProcessStatus::Exited) {
            std::printf("process exited\n");
            break;
        }
        if (g_plan.has_stop_frame && g_frame >= g_plan.stop_frame) {
            std::printf("frame limit reached: frame %u\n", g_frame);
            break;
        }
        if (system.CoreTiming().GetGlobalTicks() - start_ticks > tick_limit) {
            std::printf("time limit reached\n");
            break;
        }
    }
    LockstepFinish();
    WriteCallCounts(shots_dir, true);
    std::printf("emulated time: %.1f ms\n",
                (system.CoreTiming().GetGlobalTicks() - start_ticks) / 268111.856);
    PrintRoomMembers();
    std::fflush(stdout);
    if (g_ipc_trace) {
        EndIpcTrace(system);
    }
    if (g_svc_trace) {
        EndSvcTrace();
    }
    EndStateTrace();
    EndDspCapture(system);
    EndHidCapture(system);
    if (!unmapped_report.empty()) {
        WriteUnmappedReport(system, unmapped_report, cpu, new_3ds);
    }
    system.Shutdown();
    if (in_room) {
        Network::Shutdown();
    }
    Common::Log::Stop();
    return rc;
}
