// The replay (N1-11): the requests of an IPC trace, sent in the order they were made to the native
// services in a runtime that runs no guest code, and a comparison of every reply with the recorded one.
//
// The runtime comes from Runtime::Prepare with the devices and services phases (the caller makes it);
// the replay adds a scratch thread to its process (the requests are sent on it, one at a time, with
// the reply delay of the IPC layer turned off) and maps scratch guest memory:
//  - the thread's thread-local storage, whose command buffer receives each request;
//  - the buffers a request carries, at the addresses the trace recorded, filled from the trace's
//    `data=` bytes (zeros for a buffer the trace has no bytes for, such as a write-only mapped one);
//  - one receive area for the static buffers of replies.
//
// Handles in a request are made natively:
//  - `0` and the pseudo handles stay as they are;
//  - a handle a reply introduced (reference value -> native value, built as replies are compared)
//    becomes the native one;
//  - a handle the SVC trace shows was created by SVC 0x13 (mutex), 0x15 (semaphore), 0x17 (event),
//    0x1A (timer), 0x1E (memory block, always taken from the BASE region at an address of the
//    kernel's choosing) or 0x21 (address arbiter) becomes a new object of that kind, one per creating
//    call; a mutex is made unlocked;
//  - any other handle (no SVC trace, or an object this cannot make) skips the request.
// The SVC trace and the IPC trace are lined up as tracefmt.py lines them up: the n-th
// SendSyncRequest of a thread (not counting the ones that failed with 'invalid handle', 0xD8E007F7: the
// session handle was not a client session, and neither runtime writes an IPC record for that) is that
// thread's n-th request. A call that fails some other way before a request exists (a session whose
// server side is closed, 0xC920181A) leaves no record either, and is counted; the lining-up is wrong
// from there on for that thread. A handle's object is the one the last creating call before that
// SendSyncRequest, in the SVC trace's file order, made. A CloseHandle that follows a request in the SVC
// trace closes the native handle after the request that most recently precedes it there.
//
// A session named `<service>` is connected the first time (svcConnectToPort for a named port, the
// service manager's GetServiceHandle otherwise); a session a reply handed out is known by the name the
// trace gave it (`fs:USER/file#3`) and, if the native session has a port name, by that. When the same
// name is handed out again the newest session answers to it.
//
// Time (N1-11b): game time stands still between requests unless the replay moves it. Before it sends a
// request the replay advances the game clock to that record's tick (`t=`, the tick the recorder wrote the
// reply at), counted from the first record's: the first request is sent at the clock's reading when the
// replay starts, and a record that is n ticks after the first is sent n ticks after that. Only
// forward: a record whose tick is not beyond the clock's target stays where it is (replies are written
// when they complete, so the ticks of consecutive requests need not grow). The clock is moved to each
// timer that falls due on the way, one at a time and in (tick, id) order, and the timer and the posted
// work run on this thread (GuestScheduler::RunPending) with the clock at the timer's own tick, as the
// runtime runs them; nothing sleeps, so a gap of hours costs only the events inside it. That needs the
// runtime's clock to be a ReplayTimeSource (MakeReplayRuntimeConfig); ReplayConfig::advance_time false
// leaves the clock to the runtime (today's behaviour: the host's, which no request moves).
// A deferred reply is waited for in real time, up to ReplayConfig::reply_timeout, by running the
// scheduler's posted work. With the replay clock, game time passes during that wait only toward a timer
// the request itself armed (while its handler ran, or from the callback of such a timer): the clock goes
// there timer by timer (each runs, in order, with no sleep), at most 60 s of game time from the start of
// the wait. A reply that waits for game time (a file read's latency, the UDS delays of up to 5 s) is
// delivered at its tick instead of never; a reply that waits only for a host thread (an I/O pool) sees
// no game time pass, however long the thread takes, so the clock does not depend on the host's speed;
// a hung service does not run the clock away.
#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ipc_replay/replay_clock.h"
#include "ipc_replay/time_dependent.h"
#include "ipc_replay/trace.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

namespace mh4n::ipc_replay {

struct ReplayConfig {
    // The services to replay; a name also selects the sessions it hands out (`name/...`). Empty: every
    // service registered natively.
    std::vector<std::string> services;
    // Report every difference. Without it a service stops being replayed after its first.
    bool keep_going = false;
    TimeDependent time_dependent;
    std::chrono::milliseconds reply_timeout{30000};
    // Advance the game clock to each record's tick before its request is sent (above). The runtime
    // must have been made with MakeReplayRuntimeConfig; otherwise the replay cannot start (error).
    bool advance_time = true;
    // The most game time a trace may span, from its first record (by seq) to any other: a record beyond
    // it is an error (a damaged trace, whose timers would keep the replay busy for days of game time).
    u64 max_span_ticks = 24ull * 3600ull * kTicksPerSecond;
};

struct Difference {
    std::string service;
    u32 seq = 0;
    std::string session;
    u32 header = 0;
    std::vector<std::string> what;  // one line per part that differs: no values of buffers or words
};

struct ServiceTally {
    u32 replayed = 0;
    u32 differ = 0;
};

struct ReplayReport {
    std::vector<Difference> differences;
    std::map<std::string, ServiceTally> services;
    // (service, reason) -> requests that were not replayed
    std::map<std::pair<std::string, std::string>, u32> skipped;
    // Set when the replay could not start (an unknown --service, no memory for the scratch area).
    std::string error;

    u32 Replayed() const;
    u32 Differing() const;
    // 0 nothing differs; 1 something does; 2 the replay could not start or replayed nothing.
    int ExitStatus() const;
};

// Replays `trace` (and uses `svc`, which may be null, for the objects the requests carry).
ReplayReport Replay(Runtime& runtime, ServiceManager& manager, const IpcTrace& trace,
                    const SvcTrace* svc, const ReplayConfig& config);

// The report as the tool prints it: a line per difference, then the totals. What appears is the service,
// sequence number, session and header of a request, result codes, descriptor kinds, indices, sizes and
// CRCs, handle counts, the kinds of objects, and fixed phrases. The session, the kinds and the CRCs come
// from the trace only after ParseIpcTrace checked their shape; never the words of a reply that differ,
// the bytes of a buffer, or any other text of the trace.
std::string FormatReport(const ReplayReport& report);

}  // namespace mh4n::ipc_replay
