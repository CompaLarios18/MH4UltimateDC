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
// SendSyncRequest of a thread (not counting the ones that failed with 'invalid handle') is that
// thread's n-th request. A handle's object is the one the last creating call before that
// SendSyncRequest, in the SVC trace's file order, made. A CloseHandle that follows a request in the SVC
// trace closes the native handle after the request that most recently precedes it there.
//
// A session named `<service>` is connected the first time (svcConnectToPort for a named port, the
// service manager's GetServiceHandle otherwise); a session a reply handed out is known by the name the
// trace gave it (`fs:USER/file#3`) and, if the native session has a port name, by that. When the same
// name is handed out again the newest session answers to it.
//
// Time: the replay waits for a deferred reply in real time (the clock the runtime was made with), up to
// ReplayConfig::reply_timeout.
#pragma once

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

// The report as the tool prints it: a line per difference, then the totals. Only service, header,
// result, descriptor index, size and CRC appear.
std::string FormatReport(const ReplayReport& report);

}  // namespace mh4n::ipc_replay
