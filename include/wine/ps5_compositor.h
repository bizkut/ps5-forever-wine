/* RuneScape title GPU window compositor.
 *
 * One process-wide compositor owns the only VideoOut swapchain. Every Win32
 * window swapchain becomes a private chain whose images live in title host
 * memory imported into both the producer's VkDevice and the compositor's
 * VkDevice (VK_EXT_external_memory_host), so producers on other devices need
 * no copy. Window geometry, visibility, z-order and ancestor clipping are
 * published by win32u as a scene; the compositor thread latches the newest
 * presented frame of each window, letterboxes the union of the visible
 * top-level client areas into the output and paints the windows bottom to top.
 * A single drawn window takes exactly one compute pass from its image to the
 * output, the same work as the direct single-swapchain blit.
 *
 * The planner (ps5c_plan_compose) is pure and has no Vulkan side effects. */
#ifndef WINE_PS5_COMPOSITOR_H
#define WINE_PS5_COMPOSITOR_H

#include <stdint.h>
#ifdef PS5C_KHRONOS_HEADERS
#include <vulkan/vulkan.h>
#else
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wine/vulkan.h"
#endif
#include "wine/ps5_viewport.h"

#define PS5C_MAX_WINDOWS 64
#define PS5C_MAX_CHAIN_IMAGES 8
#define PS5C_MAX_OUTPUT_IMAGES 8
#define PS5C_MAX_VIEW_FORMATS 8
#define PS5C_MAX_QUEUE_FAMILIES 8
#define PS5C_Z_DEPTH 8

struct ps5c_rect
{
    int32_t left, top, right, bottom;
};

/* A window that has (or had) a chain. Desktop coordinates are the virtual
 * screen pixels Win32 reports. z[] is the sibling position (0 = bottom) of
 * each ancestor from the top-level window down to the window itself, so
 * children sort above their parents and top-level order decides the rest. */
struct ps5c_window_state
{
    uint64_t key;
    uint32_t visible;      /* window and all ancestors visible, root not minimized */
    uint32_t top_level;    /* its client area joins the composed view */
    struct ps5c_rect client;
    struct ps5c_rect clip; /* client clipped by every ancestor's client area */
    uint32_t z_depth;
    uint16_t z[PS5C_Z_DEPTH];
};

/* Mirrors the shader's push constant block (std430). */
struct ps5c_push
{
    int32_t dst_offset[2];
    int32_t dst_size[2];
    float scale[2];
    float offset[2];
    float client[4];
    float clip[4];
    uint32_t background;
};

struct ps5c_view
{
    uint32_t valid;
    struct ps5c_rect desktop;          /* composed desktop rectangle */
    struct wine_ps5_viewport fit;      /* where it lands in the output */
};

struct ps5c_plan
{
    struct ps5c_view view;
    uint32_t clear;                    /* nothing drawn: clear the output to black */
    uint32_t pass_count;
    uint32_t window[PS5C_MAX_WINDOWS]; /* index into the planned states per pass */
    struct ps5c_push passes[PS5C_MAX_WINDOWS];
};

int ps5c_rect_empty( const struct ps5c_rect *rect );
int ps5c_rect_intersect( struct ps5c_rect *out, const struct ps5c_rect *a, const struct ps5c_rect *b );
int ps5c_z_compare( const struct ps5c_window_state *a, const struct ps5c_window_state *b );

/* has_content[i] says whether states[i] has a presented image. Returns 0 only
 * for invalid arguments. */
int ps5c_plan_compose( const struct ps5c_window_state *states, const uint8_t *has_content, uint32_t count,
                       uint32_t output_width, uint32_t output_height, struct ps5c_plan *plan );

/* --- Vulkan side ---------------------------------------------------------- */

struct ps5c_device
{
    VkDevice device;
#define PS5C_DEVICE_FUNCS \
    X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory) X(vkBeginCommandBuffer) \
    X(vkBindImageMemory) X(vkCmdBindDescriptorSets) X(vkCmdBindPipeline) X(vkCmdClearColorImage) \
    X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkCmdPushConstants) X(vkCreateCommandPool) \
    X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) X(vkCreateFence) \
    X(vkCreateImage) X(vkCreateImageView) X(vkCreatePipelineLayout) X(vkCreateSampler) X(vkCreateSemaphore) \
    X(vkCreateShaderModule) X(vkDestroyCommandPool) X(vkDestroyDescriptorPool) X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyFence) X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyPipeline) X(vkDestroyPipelineLayout) \
    X(vkDestroySampler) X(vkDestroySemaphore) X(vkDestroyShaderModule) X(vkEndCommandBuffer) \
    X(vkFreeCommandBuffers) X(vkFreeDescriptorSets) X(vkFreeMemory) X(vkGetFenceStatus) \
    X(vkGetImageMemoryRequirements) X(vkGetMemoryHostPointerPropertiesEXT) X(vkQueueSubmit) \
    X(vkQueueWaitIdle) X(vkResetFences) X(vkUpdateDescriptorSets) X(vkWaitForFences)
#define X(name) PFN_##name p_##name;
    PS5C_DEVICE_FUNCS
#undef X
};

/* Loads every entry point; fails if one is missing (the device must enable
 * VK_EXT_external_memory_host). */
VkResult ps5c_device_init( struct ps5c_device *device, VkDevice handle, PFN_vkGetDeviceProcAddr get_proc );

/* The single presenter. acquire must signal the semaphore; present must wait
 * for it. Both run only on the compositor thread. */
struct ps5c_output
{
    void *context;
    uint32_t image_count;
    VkImage images[PS5C_MAX_OUTPUT_IMAGES];
    VkExtent2D extent;
    VkFormat format; /* B8G8R8A8_UNORM or R8G8B8A8_UNORM; usage STORAGE | TRANSFER_DST */
    VkResult (*acquire)( void *context, VkSemaphore signal, uint32_t *index );
    VkResult (*present)( void *context, VkQueue queue, uint32_t index, VkSemaphore wait );
};

struct ps5c_compositor_desc
{
    struct ps5c_device *device;
    VkQueue queue;
    uint32_t queue_family;
    struct ps5c_output output;
    /* Called on the compositor thread when the composed view changes. */
    void (*view_changed)( void *context, const struct ps5c_view *view );
    void *view_context;
};

struct ps5c_chain_desc
{
    uint32_t image_count;
    VkFormat format;              /* B8G8R8A8_{UNORM,SRGB} or R8G8B8A8_{UNORM,SRGB} */
    VkExtent2D extent;
    VkImageUsageFlags usage;
    uint32_t view_format_count;   /* the application's mutable-format list */
    const VkFormat *view_formats;
};

struct ps5c_stats
{
    uint64_t composes, dispatches, clears, skipped;
    uint32_t last_dispatches, last_clears;
    uint32_t live_chains, live_images, host_allocations;
    uint64_t host_bytes;
    uint32_t last_output_index;
    struct ps5c_view view;
};

struct ps5c_compositor;
struct ps5c_chain;

VkResult ps5c_compositor_create( const struct ps5c_compositor_desc *desc, struct ps5c_compositor **out );
void ps5c_compositor_destroy( struct ps5c_compositor *compositor );
/* Replaces every window's state; windows missing from the scene no longer
 * exist, and their retained images are released. */
void ps5c_scene_update( struct ps5c_compositor *compositor, const struct ps5c_window_state *states, uint32_t count );
/* Keys of all windows with live or retained chains. */
uint32_t ps5c_window_keys( struct ps5c_compositor *compositor, uint64_t *keys, uint32_t capacity );
/* Blocks until everything published so far is composed and its GPU work done. */
VkResult ps5c_compositor_flush( struct ps5c_compositor *compositor );
void ps5c_get_stats( struct ps5c_compositor *compositor, struct ps5c_stats *stats );

VkResult ps5c_chain_create( struct ps5c_compositor *compositor, uint64_t window_key, struct ps5c_device *producer,
                            const struct ps5c_chain_desc *desc, struct ps5c_chain **out );
uint32_t ps5c_chain_images( struct ps5c_chain *chain, VkImage *images, uint32_t capacity );
/* timeout_ns 0 polls (VK_NOT_READY), UINT64_MAX blocks. A chain paces like
 * FIFO: a new image is handed out only once the previous present was latched. */
VkResult ps5c_chain_acquire( struct ps5c_chain *chain, uint64_t timeout_ns, uint32_t *index );
/* Submits the presented image's release on the producer queue after waits,
 * then publishes it. */
VkResult ps5c_chain_present( struct ps5c_chain *chain, VkQueue queue, uint32_t queue_family, uint32_t index,
                             uint32_t wait_count, const VkSemaphore *waits );
/* Returns an acquired image without presenting it. */
void ps5c_chain_release( struct ps5c_chain *chain, uint32_t index );
/* Producer resources go now; the last presented frame stays the window's
 * content until a newer chain for the window presents or the window goes. */
void ps5c_chain_destroy( struct ps5c_chain *chain );

#endif
