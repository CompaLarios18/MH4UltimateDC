// The bookkeeping of a ring buffer (N2-26), apart from any GPU object so that it can be tested with a
// made-up clock: which bytes are in use by which submission, and where the next allocation goes.
#pragma once

#include <deque>
#include <string>

#include "gpu/backend/rhi.h"

namespace mh4n::gpu::rhi::core {

class RingAllocator {
public:
    explicit RingAllocator(u64 capacity) : capacity_(capacity) {}

    enum class Status : u8 {
        Ok,       // reserved at `offset`
        MustWait, // free again once the timeline has passed `wait_for`; call TryMap again afterwards
        Failed,   // cannot be satisfied: see `error`
    };
    struct Reservation {
        Status status = Status::Failed;
        u64 offset = 0;
        bool wrapped = false; // the allocation starts below the previous write position
        TimelineValue wait_for = 0;
        std::string error;
    };

    // Reserves `size` bytes at a multiple of `alignment` (a power of two). `completed` is the highest
    // value the GPU has passed. At most one reservation is open: a new TryMap replaces it.
    Reservation TryMap(u64 size, u64 alignment, TimelineValue completed);

    // Marks the first `used` bytes of the open reservation as in use and returns the id of the span
    // (0 when nothing was used, or when no reservation is open; ids start at 1).
    u64 Commit(u64 used);
    // The span `id` is in use by the submission with `value` (0: not at all, free at once).
    void Stamp(u64 id, TimelineValue value);

    u64 capacity() const { return capacity_; }
    size_t live_spans() const { return spans_.size(); }

private:
    struct Span {
        u64 id = 0;
        u64 begin = 0;
        u64 end = 0;
        TimelineValue value = 0;
        bool stamped = false;
    };
    void Retire(TimelineValue completed);

    u64 capacity_;
    u64 head_ = 0;
    u64 next_id_ = 1;
    std::deque<Span> spans_;       // in the order they were written; the front is the oldest
    bool wrapped_pending_ = false; // the head has been moved to the start since the last reservation
    bool open_ = false;
    u64 open_offset_ = 0;
    u64 open_size_ = 0;
};

} // namespace mh4n::gpu::rhi::core
