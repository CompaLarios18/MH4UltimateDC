// The options of the deterministic mode (N1-25), registered here and read by the runtime (runtime.cpp)
// and by the services that have host threads (services/fs). See deterministic.h.
#include "mh4n/options.h"

MH4N_OPTION("runner", "--deterministic", "",
            "make the run repeatable: the game clock counts the cycles the code library charges (it needs "
            "the library built with --native --deterministic, tools/build_game.py), file replies and the "
            "rest of the host's work come at fixed ticks of that clock, and the clock starts at the "
            "reference runner's fixed starting tick. The same options give the same traces; much slower "
            "than real time. A real-time run is not made repeatable by it");
MH4N_OPTION("runner", "--seed", "N",
            "with --deterministic: draw the length of each scheduling slice from a generator seeded with N, "
            "between half and one and a half times the default (4274 us, or --slice-us), which moves the "
            "points where the two virtual cores take turns; the same seed gives the same run");
