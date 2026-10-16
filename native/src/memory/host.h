// The host operations the guest memory needs (N1-02), one implementation per platform:
// host_posix.cpp (Linux, macOS) and host_win32.cpp (Windows). Internal to the memory module.
#pragma once

#include <cstddef>
#include <cstdint>

#include "mh4n/memory.h"

namespace mh4n::host {

// The host page size (4 KiB on x86-64, 16 KiB on Apple silicon).
std::size_t PageSize();

// Reserves `size` bytes of address space, inaccessible and not counted against memory
// (MAP_NORESERVE / MEM_RESERVE). nullptr on failure.
void* Reserve(std::size_t size);
void Release(void* base, std::size_t size);

// Both take page-aligned ranges inside a reservation.
// Commit: readable and writable; pages that were not committed read as zero.
bool Commit(void* addr, std::size_t size);
// Decommit: inaccessible again and the memory given back to the OS, so a later Commit reads zero.
bool Decommit(void* addr, std::size_t size);

// Installs the process-wide fault handler (once) and, for the calling thread, an alternate stack
// for it. Faults outside every registered reservation go to the handler that was installed
// before ours.
void InstallFaultHandler();
void InstallThreadFaultStack();

} // namespace mh4n::host

namespace mh4n::fault {

// The reservations the fault handler recognises, kept in a fixed table of atomics so that the
// handler can read it (async-signal-safe).
bool RegisterReservation(std::uintptr_t base, std::size_t size);
void UnregisterReservation(std::uintptr_t base);

// Called from the platform's handler with the faulting host address. If it lies inside a
// registered reservation, writes the report to stderr and ends the process with _exit(3) (does not
// return). Otherwise returns false and the handler passes the fault on.
bool HandleFault(std::uintptr_t host_addr, FaultAccess access);

} // namespace mh4n::fault
