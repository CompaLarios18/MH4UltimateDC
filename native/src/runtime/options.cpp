// The runner's own options (scope "runner", with mh4u_runner's names). --help, --version, --log
// and --print-process belong to the program (native/runner/main.cpp); the options other modules
// need are registered by those modules and show up in --help by themselves.
#include "mh4n/options.h"

MH4N_OPTION("runner", "--code", "LIB",
            "the recompiled code library of the program (built with --native); required for a program "
            "with guest code");
MH4N_OPTION("runner", "--ms", "N",
            "stop after N ms of game time (default 10000; f<N>: after game frame N, as --frames)");
MH4N_OPTION("runner", "--frames", "N",
            "stop when the game's frame counter (the u32 at 0x010571d4) reaches N; without --ms there is "
            "no time limit");
MH4N_OPTION("runner", "--trace-svc", "FILE",
            "write every system call to FILE, a line per thread created and a header (format of "
            "tools/native/tracefmt.py; compare with tools/compare_svc_traces.py --normalize --outputs "
            "--ignore-regs config/native/svc_unused_registers.txt)");
MH4N_OPTION("runner", "--state-trace", "FILE",
            "write the game's observable state on every game frame (tools/native/state_diff.py)");
MH4N_OPTION("runner", "--observable", "FILE",
            "the values --state-trace records (default config/native/observable.txt, relative to the "
            "current directory)");
MH4N_OPTION("runner", "--slice-us", "N",
            "length of a scheduling slice in microseconds when both virtual cores have work (default 1000)");
MH4N_OPTION("runner", "--user", "DIR", "the user directory (NAND, SD card, saves) for the services that need one");
MH4N_OPTION("runner", "--init-time", "SECONDS",
            "the date the console starts at, seconds since the Unix epoch, UTC (default 1767268800, "
            "2026-01-01 12:00 UTC: the game seeds its random numbers from the clock)");
MH4N_OPTION("runner", "--gpu", "NAME", "the GPU (default null)");
MH4N_OPTION("runner", "--input-source", "NAME", "where the buttons, the circle pad and the touch screen come from (default script)");
MH4N_OPTION("runner", "--audio", "NAME", "where the DSP's output goes (default null)");
