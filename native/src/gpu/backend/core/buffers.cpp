// Buffers and ring buffers (N2-26).
#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "gpu/backend/core/impl.h"
#include "gpu/backend/core/ring_alloc.h"
#include "mh4n/log.h"

namespace mh4n::gpu::rhi {

namespace core {

RingAllocator::Reservation RingAllocator::TryMap(u64 size, u64 alignment, TimelineValue completed) {
    Reservation result;
    open_ = false;
    if (size == 0 || size > capacity_) {
        result.error = fmt::format("a ring of {} bytes cannot hand out {} bytes", capacity_, size);
        return result;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        result.error = fmt::format("the alignment {} is not a power of two", alignment);
        return result;
    }
    const u64 head_at_entry = head_;
    Retire(completed);
    if (spans_.empty()) {
        head_ = 0;
    }
    const auto align_up = [&](u64 value) { return (value + alignment - 1) & ~(alignment - 1); };

    for (int round = 0; round < 2; ++round) {
        const u64 tail = spans_.empty() ? 0 : spans_.front().begin;
        const bool wrapped_state = !spans_.empty() && head_ <= tail;
        const u64 offset = align_up(head_);
        if (!wrapped_state) {
            if (offset + size <= capacity_) {
                open_ = true;
                open_offset_ = offset;
                open_size_ = size;
                result.status = Status::Ok;
                result.offset = offset;
                result.wrapped = wrapped_pending_ || offset < head_at_entry;
                wrapped_pending_ = false;
                return result;
            }
            // The end is reached: the bytes from the head to the end are given up, and the allocation
            // restarts at the beginning (the gap is a span that is already free).
            if (head_ < capacity_) {
                spans_.push_back(Span{next_id_++, head_, capacity_, 0, true});
            }
            head_ = 0;
            wrapped_pending_ = true;
            Retire(completed);
            if (spans_.empty()) {
                head_ = 0;
            }
            continue;
        }
        if (offset + size <= tail) {
            open_ = true;
            open_offset_ = offset;
            open_size_ = size;
            result.status = Status::Ok;
            result.offset = offset;
            result.wrapped = wrapped_pending_ || offset < head_at_entry;
            wrapped_pending_ = false;
            return result;
        }
        // The space is held by the oldest span: wait for it, unless no submission is known to cover it.
        const Span& front = spans_.front();
        if (!front.stamped) {
            result.error = fmt::format(
                "the ring is full of space committed in a recorder that has not been submitted (the oldest span is [{}, {})): submit it first",
                front.begin, front.end);
            return result;
        }
        result.status = Status::MustWait;
        result.wait_for = front.value;
        return result;
    }
    result.error = "ring bookkeeping did not settle";
    return result;
}

u64 RingAllocator::Commit(u64 used) {
    if (!open_ || used == 0 || used > open_size_) {
        open_ = false;
        return 0;
    }
    open_ = false;
    Span span;
    span.id = next_id_++;
    span.begin = open_offset_;
    span.end = open_offset_ + used;
    spans_.push_back(span);
    head_ = span.end;
    return span.id;
}

void RingAllocator::Stamp(u64 id, TimelineValue value) {
    if (spans_.empty() || id < spans_.front().id) {
        return;
    }
    for (Span& span : spans_) {
        if (span.id == id) {
            span.value = value;
            span.stamped = true;
            return;
        }
    }
}

void RingAllocator::Retire(TimelineValue completed) {
    while (!spans_.empty() && spans_.front().stamped && spans_.front().value <= completed) {
        spans_.pop_front();
    }
}

}  // namespace core

// ---- Buffer --------------------------------------------------------------------------------------

Buffer::Impl::~Impl() {
    if (mapped != nullptr && buffer != nullptr) {
        buffer->unmap();
    }
}

Buffer::Buffer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Buffer::~Buffer() = default;

u64 Buffer::Size() const {
    return impl_->desc.size;
}
Heap Buffer::GetHeap() const {
    return impl_->desc.heap;
}
BufferUsage Buffer::Usage() const {
    return impl_->desc.usage;
}
std::span<u8> Buffer::Mapped() const {
    return impl_->mapped != nullptr ? std::span<u8>(impl_->mapped, impl_->desc.size) : std::span<u8>();
}

void Buffer::Flush(u64 offset, u64 size) {
    if (impl_->mapped != nullptr && offset < impl_->desc.size) {
        Backend::Impl& backend = *impl_->backend;
        vmaFlushAllocation(backend.device->allocator, impl_->vulkan()->allocation, offset, std::min(size, impl_->desc.size - offset));
    }
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (impl_->mapped != nullptr && offset < impl_->desc.size) {
        Backend::Impl& backend = *impl_->backend;
        vmaInvalidateAllocation(backend.device->allocator, impl_->vulkan()->allocation, offset, std::min(size, impl_->desc.size - offset));
    }
}

Result<std::shared_ptr<Buffer>> Backend::CreateBuffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        return Error{fmt::format("buffer \"{}\": size 0", desc.name)};
    }
    ::plume::RenderBufferDesc plume_desc;
    plume_desc.size = desc.size;
    switch (desc.heap) {
    case Heap::Device: plume_desc.heapType = ::plume::RenderHeapType::DEFAULT; break;
    case Heap::Upload: plume_desc.heapType = ::plume::RenderHeapType::UPLOAD; break;
    case Heap::Readback: plume_desc.heapType = ::plume::RenderHeapType::READBACK; break;
    }
    if (HasUsage(desc.usage, BufferUsage::Vertex)) plume_desc.flags |= ::plume::RenderBufferFlag::VERTEX;
    if (HasUsage(desc.usage, BufferUsage::Index)) plume_desc.flags |= ::plume::RenderBufferFlag::INDEX;
    if (HasUsage(desc.usage, BufferUsage::Uniform)) plume_desc.flags |= ::plume::RenderBufferFlag::CONSTANT;
    if (HasUsage(desc.usage, BufferUsage::Storage)) plume_desc.flags |= ::plume::RenderBufferFlag::STORAGE;
    if (HasUsage(desc.usage, BufferUsage::Texel)) plume_desc.flags |= ::plume::RenderBufferFlag::FORMATTED;

    auto impl = std::make_unique<Buffer::Impl>();
    impl->backend = impl_->shared_from_this();
    impl->desc = desc;
    impl->buffer = impl_->device_owner->createBuffer(plume_desc);
    if (impl->buffer == nullptr || impl->vulkan()->vk == VK_NULL_HANDLE) {
        return Error{fmt::format("buffer \"{}\" of {} bytes could not be made", desc.name, desc.size)};
    }
    if (!desc.name.empty()) {
        impl->buffer->setName(desc.name);
    }
    if (desc.heap != Heap::Device) {
        impl->mapped = static_cast<u8*>(impl->buffer->map());
        if (impl->mapped == nullptr) {
            return Error{fmt::format("buffer \"{}\" could not be mapped", desc.name)};
        }
    }
    return std::make_shared<Buffer>(std::move(impl));
}

// ---- Ring ----------------------------------------------------------------------------------------

namespace {

struct RingState {
    std::shared_ptr<Backend::Impl> backend;
    std::shared_ptr<Buffer> buffer;
    std::mutex mutex;
    core::RingAllocator allocator;
    RingState(std::shared_ptr<Backend::Impl> b, std::shared_ptr<Buffer> buf, u64 size)
        : backend(std::move(b)), buffer(std::move(buf)), allocator(size) {}
};

}  // namespace

struct Ring::Impl {
    std::shared_ptr<RingState> state;
    u64 last_size = 0;    // the last Map's size
    u64 last_offset = 0;  // and offset
};

Ring::Ring(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Ring::~Ring() = default;

const std::shared_ptr<Buffer>& Ring::GetBuffer() const {
    return impl_->state->buffer;
}
u64 Ring::Size() const {
    return impl_->state->allocator.capacity();
}

Result<RingAllocation> Ring::Map(u64 size, u64 alignment) {
    RingState& state = *impl_->state;
    std::unique_lock lock(state.mutex);
    for (;;) {
        const core::RingAllocator::Reservation r = state.allocator.TryMap(size, alignment, state.backend->Completed());
        if (r.status == core::RingAllocator::Status::Ok) {
            impl_->last_size = size;
            impl_->last_offset = r.offset;
            RingAllocation allocation;
            allocation.data = state.buffer->Mapped().subspan(r.offset, size);
            allocation.offset = r.offset;
            allocation.wrapped = r.wrapped;
            return allocation;
        }
        if (r.status == core::RingAllocator::Status::Failed) {
            return Error{"Ring::Map: " + r.error};
        }
        lock.unlock();
        state.backend->WaitFor(r.wait_for);
        lock.lock();
    }
}

Result<void> Ring::Commit(u64 size) {
    RingState& state = *impl_->state;
    if (size > impl_->last_size) {
        return Error{fmt::format("Ring::Commit: {} bytes, but the last Map reserved {}", size, impl_->last_size)};
    }
    Recorder::Impl* recorder = nullptr;
    {
        std::lock_guard lock(state.backend->thread_mutex);
        const auto it = state.backend->threads.find(std::this_thread::get_id());
        if (it != state.backend->threads.end()) {
            recorder = it->second.open;
        }
    }
    if (recorder == nullptr) {
        return Error{"Ring::Commit: the calling thread has no open recorder to attach the space to"};
    }
    u64 id = 0;
    {
        std::lock_guard lock(state.mutex);
        id = state.allocator.Commit(size);
    }
    if (id == 0) {
        return Result<void>{};
    }
    state.buffer->Flush(impl_->last_offset, size);  // a no-op on coherent memory
    recorder->on_submit.push_back([weak = std::weak_ptr<RingState>(impl_->state), id](TimelineValue value) {
        if (std::shared_ptr<RingState> state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            state->allocator.Stamp(id, value);
        }
    });
    return Result<void>{};
}

Result<std::unique_ptr<Ring>> Backend::CreateRing(u64 size, BufferUsage usage, std::string name) {
    BufferDesc desc;
    desc.size = size;
    desc.heap = Heap::Upload;
    desc.usage = usage;
    desc.name = std::move(name);
    Result<std::shared_ptr<Buffer>> buffer = CreateBuffer(desc);
    if (!buffer.ok()) {
        return Error{buffer.error()};
    }
    auto impl = std::make_unique<Ring::Impl>();
    impl->state = std::make_shared<RingState>(impl_, *buffer, size);
    return std::make_unique<Ring>(std::move(impl));
}

}  // namespace mh4n::gpu::rhi
