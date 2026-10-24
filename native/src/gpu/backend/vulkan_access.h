// The Vulkan side of the backend (N2-26), which rhi.h's boundary leaves out (rhi-spike.md 9.4 item 1,
// "Boundary caveat"): the raw handles that Dear ImGui and the swapchain code need, the command buffer
// being recorded, the queue lock for presenting, a surface the caller made, and images over a VkImage
// the backend does not own. The compositor (N2b-05), the present thread (N2b-06), Dear ImGui and tests
// use it; the renderer does not.
//
// Written once and FROZEN when N2-26 is merged: it changes only through the orchestrator.
//
// It includes volk's header, so every user compiles against the one volk (the backend loads the loader at
// run time; the global function pointers volk declares are loaded for the instance when it is created, and
// device-level calls go through the loader's dispatch: do not call volkLoadDevice). Nothing in it can be
// used before a CreateInstance has succeeded.
#pragma once

// VK_KHR_portability_subset lives among the beta declarations; the backend's own headers define this too.
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include <volk.h>

#include <memory>
#include <mutex>

#include "gpu/backend/rhi.h"

namespace mh4n::gpu::rhi {

// The handles of the device (R4). All of them belong to the backend; none may be destroyed by the caller.
struct VulkanHandles {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    u32 queue_family_index = 0;  // the family of `queue` (graphics, compute and transfer)
    VkQueue queue = VK_NULL_HANDLE;  // the one queue the backend submits to; use it only under QueueLock
};
VulkanHandles Handles(const Backend& backend);

// The VkInstance of an instance, for creating a surface on it before the backend exists (CreateBackend
// takes the surface). Valid while the Instance lives.
VkInstance NativeInstance(const Instance& instance);

// The command buffer the recorder is recording into. A pass the recorder has open is begun first (the
// backend begins a pass at its first draw), so raw draw commands inside it are valid. Raw commands may
// change any state a Recorder call set (pipeline, bindings, vertex and index buffers, dynamic state);
// the Recorder keeps no copy of it, so the next Recorder call that needs a state sets it again. Raw
// commands must leave the command buffer in the state they found it (a pass open or not), and must
// not touch an image's layout: images stay in the one layout of the targets module.
VkCommandBuffer CommandBuffer(Recorder& recorder);

// A surface the caller made on the instance (for example SDL's, or vkCreateHeadlessSurfaceEXT's),
// as the handle BackendOptions takes, and back.
SurfaceHandle SurfaceFrom(VkSurfaceKHR surface);
VkSurfaceKHR VulkanSurface(SurfaceHandle surface);

// Holds the submit lock of the queue (the backend's per-queue mutex, the one every Submit takes) for as
// long as it lives: the caller's vkQueuePresentKHR goes under it, never around it, because the queue is
// shared by the renderer thread and the present thread and Vulkan requires external synchronisation of
// a queue. Do not call Backend::Submit (or anything that submits) while holding it: it would wait for
// the lock the caller holds.
class QueueLock {
public:
    explicit QueueLock(Backend& backend);
    ~QueueLock();
    QueueLock(const QueueLock&) = delete;
    QueueLock& operator=(const QueueLock&) = delete;

private:
    std::unique_lock<std::mutex> lock_;
};

// The VkImage of a backend image (N2-27). The caller must not change its layout or destroy it.
VkImage NativeImage(const Image& image);

// An image over a VkImage the backend does not own (a swapchain image; N2-27): never destroyed by the
// backend, under the same rules as its own images (one owning thread, the one layout). The format must be
// a member of Format the backend knows the device format of, and `width` x `height` the image's size.
// Its first operation moves it to the backend's layout without keeping its contents.
Result<std::shared_ptr<Image>> WrapExternalImage(Backend& backend, VkImage image, Format format, u32 width, u32 height);

}  // namespace mh4n::gpu::rhi
