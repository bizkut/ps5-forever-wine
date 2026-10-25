/*
 * RuneScape title GPU window compositor (see include/wine/ps5_compositor.h)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#ifndef PS5C_KHRONOS_HEADERS
#include "config.h"
#endif

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wine/ps5_compositor.h"
#include "ps5_compositor_spv.h"

#define PS5C_HOST_ALIGN 65536u
#define PS5C_MAX_SOURCE_SETS 512u

/* --- Planning ------------------------------------------------------------- */

int ps5c_rect_empty( const struct ps5c_rect *rect )
{
    return rect->right <= rect->left || rect->bottom <= rect->top;
}

int ps5c_rect_intersect( struct ps5c_rect *out, const struct ps5c_rect *a, const struct ps5c_rect *b )
{
    struct ps5c_rect r = {a->left > b->left ? a->left : b->left, a->top > b->top ? a->top : b->top,
                          a->right < b->right ? a->right : b->right, a->bottom < b->bottom ? a->bottom : b->bottom};
    if (ps5c_rect_empty( &r )) r.left = r.top = r.right = r.bottom = 0;
    *out = r;
    return !ps5c_rect_empty( &r );
}

static void rect_union( struct ps5c_rect *out, const struct ps5c_rect *a )
{
    if (a->left < out->left) out->left = a->left;
    if (a->top < out->top) out->top = a->top;
    if (a->right > out->right) out->right = a->right;
    if (a->bottom > out->bottom) out->bottom = a->bottom;
}

int ps5c_z_compare( const struct ps5c_window_state *a, const struct ps5c_window_state *b )
{
    uint32_t depth_a = a->z_depth < PS5C_Z_DEPTH ? a->z_depth : PS5C_Z_DEPTH;
    uint32_t depth_b = b->z_depth < PS5C_Z_DEPTH ? b->z_depth : PS5C_Z_DEPTH;
    uint32_t depth = depth_a < depth_b ? depth_a : depth_b;

    for (uint32_t i = 0; i < depth; i++)
        if (a->z[i] != b->z[i]) return a->z[i] < b->z[i] ? -1 : 1;
    /* A parent sits below its children. */
    return depth_a < depth_b ? -1 : depth_a > depth_b;
}

static int32_t clamp_pixel( double value, int32_t limit )
{
    if (value <= 0) return 0;
    if (value >= limit) return limit;
    return (int32_t)value;
}

int ps5c_plan_compose( const struct ps5c_window_state *states, const uint8_t *has_content, uint32_t count,
                       uint32_t output_width, uint32_t output_height, struct ps5c_plan *plan )
{
    uint32_t order[PS5C_MAX_WINDOWS], n = 0, view_width, view_height;
    struct ps5c_rect visible[PS5C_MAX_WINDOWS], view = {0}, any = {0};
    int have_top = 0;
    double sx, sy, ox, oy;

    if (!plan) return 0;
    memset( plan, 0, sizeof(*plan) );
    if ((count && (!states || !has_content)) || count > PS5C_MAX_WINDOWS || !output_width || !output_height ||
        output_width > INT32_MAX || output_height > INT32_MAX)
        return 0;

    for (uint32_t i = 0; i < count; i++)
    {
        const struct ps5c_window_state *state = &states[i];
        uint32_t at;

        if (!state->visible || !has_content[i] || ps5c_rect_empty( &state->client )) continue;
        if (!ps5c_rect_intersect( &visible[i], &state->client, &state->clip )) continue;
        if (!n) any = visible[i];
        else rect_union( &any, &visible[i] );
        if (state->top_level)
        {
            if (!have_top) view = state->client;
            else rect_union( &view, &state->client );
            have_top = 1;
        }
        /* Stable insertion by z-order, bottom first. */
        at = n++;
        while (at && ps5c_z_compare( &states[order[at - 1]], state ) > 0)
        {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    if (!n)
    {
        plan->clear = 1;
        return 1;
    }
    if (!have_top) view = any;
    view_width = (uint32_t)((int64_t)view.right - view.left);
    view_height = (uint32_t)((int64_t)view.bottom - view.top);
    if (!wine_ps5_viewport_fit( &plan->view.fit, view_width, view_height, output_width, output_height ))
        return 0;
    plan->view.valid = 1;
    plan->view.desktop = view;

    sx = (double)view_width / plan->view.fit.width;
    sy = (double)view_height / plan->view.fit.height;
    ox = view.left - plan->view.fit.left * sx;
    oy = view.top - plan->view.fit.top * sy;

    for (uint32_t k = 0; k < n; k++)
    {
        const struct ps5c_window_state *state = &states[order[k]];
        struct ps5c_push *pass = &plan->passes[plan->pass_count];
        struct ps5c_rect clip;

        if (!ps5c_rect_intersect( &clip, &visible[order[k]], &view )) continue;
        pass->scale[0] = (float)sx;
        pass->scale[1] = (float)sy;
        pass->offset[0] = (float)ox;
        pass->offset[1] = (float)oy;
        pass->client[0] = state->client.left;
        pass->client[1] = state->client.top;
        pass->client[2] = state->client.right;
        pass->client[3] = state->client.bottom;
        pass->clip[0] = clip.left;
        pass->clip[1] = clip.top;
        pass->clip[2] = clip.right;
        pass->clip[3] = clip.bottom;
        if (!plan->pass_count)
        {
            /* The bottom window also paints the background and bars: a lone
             * window is one pass over the output, as the direct blit is. */
            pass->dst_size[0] = output_width;
            pass->dst_size[1] = output_height;
            pass->background = 1;
        }
        else
        {
            /* Conservative pixel bounds (truncation stays within the one-pixel
             * margins, without libm); the shader tests each pixel centre. */
            int32_t x0 = clamp_pixel( (clip.left - ox) / sx - 0.5 - 1, output_width );
            int32_t y0 = clamp_pixel( (clip.top - oy) / sy - 0.5 - 1, output_height );
            int32_t x1 = clamp_pixel( (clip.right - ox) / sx - 0.5 + 2, output_width );
            int32_t y1 = clamp_pixel( (clip.bottom - oy) / sy - 0.5 + 2, output_height );
            if (x1 <= x0 || y1 <= y0) continue;
            pass->dst_offset[0] = x0;
            pass->dst_offset[1] = y0;
            pass->dst_size[0] = x1 - x0;
            pass->dst_size[1] = y1 - y0;
        }
        plan->window[plan->pass_count++] = order[k];
    }
    if (!plan->pass_count) plan->clear = 1;
    return 1;
}

/* --- Devices ---------------------------------------------------------------- */

VkResult ps5c_device_init( struct ps5c_device *device, VkDevice handle, PFN_vkGetDeviceProcAddr get_proc )
{
    memset( device, 0, sizeof(*device) );
    device->device = handle;
#define X(name) if (!(device->p_##name = (PFN_##name)get_proc( handle, #name ))) return VK_ERROR_INITIALIZATION_FAILED;
    PS5C_DEVICE_FUNCS
#undef X
    return VK_SUCCESS;
}

/* --- Objects ---------------------------------------------------------------- */

enum image_state { IMAGE_FREE, IMAGE_ACQUIRED, IMAGE_QUEUED, IMAGE_FRONT, IMAGE_RETIRED };

struct chain_image
{
    enum image_state state;
    uint64_t last_use;             /* compose sequence that last sampled it */
    int fence_pending;             /* producer release submitted, not yet seen signalled */
    void *host_base, *host;
    size_t host_bytes;
    /* producer side */
    VkImage producer_image;
    VkDeviceMemory producer_memory;
    VkFence producer_fence;
    VkCommandBuffer release[PS5C_MAX_QUEUE_FAMILIES];
    /* compositor side */
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    int reclaimed, accounted;
};

struct ps5c_chain
{
    struct ps5c_chain *next;
    struct ps5c_compositor *compositor;
    uint64_t key, serial;
    struct ps5c_device *producer;
    uint32_t image_count;
    struct chain_image images[PS5C_MAX_CHAIN_IMAGES];
    uint32_t families[PS5C_MAX_QUEUE_FAMILIES], family_count;
    VkCommandPool pools[PS5C_MAX_QUEUE_FAMILIES];
    int front, queued;
    int destroyed, producer_gone, pinned;
};

struct output_slot
{
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, rendered;
    uint64_t seq;
    int busy;
};

struct draw_entry
{
    struct ps5c_chain *chain;
    uint32_t image;
};

struct ps5c_compositor
{
    struct ps5c_compositor_desc desc;
    struct ps5c_device *device;
    VkSampler sampler;
    VkDescriptorSetLayout source_layout, target_layout;
    VkDescriptorPool pool;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkImageView target_views[PS5C_MAX_OUTPUT_IMAGES];
    VkDescriptorSet target_sets[PS5C_MAX_OUTPUT_IMAGES];
    VkCommandPool cmd_pool;
    struct output_slot slots[PS5C_MAX_OUTPUT_IMAGES];
    uint32_t slot_count, next_slot;

    pthread_t thread;
    int thread_started, quit;
    struct ps5c_chain *chains;
    uint64_t next_serial;
    struct ps5c_window_state scene[PS5C_MAX_WINDOWS];
    uint32_t scene_count;
    int scene_valid, dirty;
    uint64_t request_gen, composed_gen, submit_seq, complete_seq;
    VkResult lost;
    struct ps5c_stats stats;
    struct ps5c_plan last_plan;
    uint64_t last_serials[PS5C_MAX_WINDOWS];
    int have_last;
};

/* One compositor per process: the locks are static. */
static pthread_mutex_t ps5c_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ps5c_work = PTHREAD_COND_INITIALIZER;  /* wakes the compositor thread */
static pthread_cond_t ps5c_done = PTHREAD_COND_INITIALIZER;  /* wakes producers and flushes */

static void wake_compositor_locked( struct ps5c_compositor *c )
{
    c->dirty = 1;
    c->request_gen++;
    pthread_cond_broadcast( &ps5c_work );
}

static VkFormat srgb_format( VkFormat format )
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkFormat unorm_format( VkFormat format )
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static uint32_t lowest_bit( uint32_t bits )
{
    for (uint32_t i = 0; i < 32; i++) if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

/* --- Chain images ------------------------------------------------------------ */

static VkResult import_host_image( struct ps5c_device *device, void *host, size_t bytes, VkImage image,
                                   VkDeviceMemory *memory )
{
    VkMemoryHostPointerPropertiesEXT host_props = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkImportMemoryHostPointerInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = host};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
            .allocationSize = bytes};
    VkMemoryRequirements reqs;
    VkResult res;

    if ((res = device->p_vkGetMemoryHostPointerPropertiesEXT( device->device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &host_props ))) return res;
    device->p_vkGetImageMemoryRequirements( device->device, image, &reqs );
    if (reqs.size > bytes || (uintptr_t)host % reqs.alignment) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if ((alloc.memoryTypeIndex = lowest_bit( reqs.memoryTypeBits & host_props.memoryTypeBits )) == UINT32_MAX)
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if ((res = device->p_vkAllocateMemory( device->device, &alloc, NULL, memory ))) return res;
    if ((res = device->p_vkBindImageMemory( device->device, image, *memory, 0 )))
    {
        device->p_vkFreeMemory( device->device, *memory, NULL );
        *memory = VK_NULL_HANDLE;
    }
    return res;
}

static void destroy_producer_image( struct ps5c_chain *chain, struct chain_image *img )
{
    struct ps5c_device *p = chain->producer;
    for (uint32_t f = 0; f < chain->family_count; f++)
        if (img->release[f]) p->p_vkFreeCommandBuffers( p->device, chain->pools[f], 1, &img->release[f] );
    memset( img->release, 0, sizeof(img->release) );
    if (img->producer_fence) p->p_vkDestroyFence( p->device, img->producer_fence, NULL );
    if (img->producer_image) p->p_vkDestroyImage( p->device, img->producer_image, NULL );
    if (img->producer_memory) p->p_vkFreeMemory( p->device, img->producer_memory, NULL );
    img->producer_fence = VK_NULL_HANDLE;
    img->producer_image = VK_NULL_HANDLE;
    img->producer_memory = VK_NULL_HANDLE;
}

/* Under the lock: the descriptor pool is shared. */
static void destroy_compositor_image_locked( struct ps5c_compositor *c, struct chain_image *img )
{
    struct ps5c_device *d = c->device;
    if (img->set) d->p_vkFreeDescriptorSets( d->device, c->pool, 1, &img->set );
    if (img->view) d->p_vkDestroyImageView( d->device, img->view, NULL );
    if (img->image) d->p_vkDestroyImage( d->device, img->image, NULL );
    if (img->memory) d->p_vkFreeMemory( d->device, img->memory, NULL );
    img->set = VK_NULL_HANDLE;
    img->view = VK_NULL_HANDLE;
    img->image = VK_NULL_HANDLE;
    img->memory = VK_NULL_HANDLE;
    if (img->host_base) munmap( img->host_base, img->host_bytes );
    if (img->accounted)
    {
        c->stats.live_images--;
        c->stats.host_allocations--;
        c->stats.host_bytes -= img->host_bytes;
    }
    img->host_base = img->host = NULL;
    img->accounted = 0;
    img->reclaimed = 1;
}

/* Under the lock: descriptor sets come from the shared pool. */
static VkResult account_chain_image_locked( struct ps5c_compositor *c, struct chain_image *img )
{
    struct ps5c_device *d = c->device;
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = c->pool, .descriptorSetCount = 1, .pSetLayouts = &c->source_layout};
    VkDescriptorImageInfo image_info = {.sampler = c->sampler, .imageView = img->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image_info};
    VkResult res;

    img->accounted = 1;
    c->stats.live_images++;
    c->stats.host_allocations++;
    c->stats.host_bytes += img->host_bytes;
    if ((res = d->p_vkAllocateDescriptorSets( d->device, &set_info, &img->set ))) return res;
    write.dstSet = img->set;
    d->p_vkUpdateDescriptorSets( d->device, 1, &write, 0, NULL );
    return VK_SUCCESS;
}

static VkResult create_chain_image( struct ps5c_compositor *c, struct ps5c_chain *chain,
                                    const struct ps5c_chain_desc *desc, struct chain_image *img )
{
    struct ps5c_device *d = c->device, *p = chain->producer;
    VkFormat formats[PS5C_MAX_VIEW_FORMATS + 3];
    uint32_t format_count = 0;
    VkImageFormatListCreateInfo list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    VkExternalMemoryImageCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &list, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .imageType = VK_IMAGE_TYPE_2D, .format = desc->format,
            .extent = {desc->extent.width, desc->extent.height, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = desc->usage | VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageViewUsageCreateInfo view_usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &view_usage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = srgb_format( desc->format ),
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkMemoryRequirements reqs_c, reqs_p;
    size_t align, bytes;
    VkResult res;

    /* Identical create info on both devices of the one physical device gives
     * one layout; the sRGB view filters in linear light. */
    formats[format_count++] = unorm_format( desc->format );
    formats[format_count++] = srgb_format( desc->format );
    for (uint32_t i = 0; i < desc->view_format_count; i++)
    {
        uint32_t j;
        for (j = 0; j < format_count; j++) if (formats[j] == desc->view_formats[i]) break;
        if (j == format_count) formats[format_count++] = desc->view_formats[i];
    }
    list.viewFormatCount = format_count;
    list.pViewFormats = formats;

    if ((res = d->p_vkCreateImage( d->device, &info, NULL, &img->image ))) return res;
    if ((res = p->p_vkCreateImage( p->device, &info, NULL, &img->producer_image ))) return res;
    d->p_vkGetImageMemoryRequirements( d->device, img->image, &reqs_c );
    p->p_vkGetImageMemoryRequirements( p->device, img->producer_image, &reqs_p );
    align = reqs_c.alignment > reqs_p.alignment ? reqs_c.alignment : reqs_p.alignment;
    if (align < PS5C_HOST_ALIGN) align = PS5C_HOST_ALIGN;
    if (align & (align - 1)) return VK_ERROR_INITIALIZATION_FAILED;
    bytes = reqs_c.size > reqs_p.size ? reqs_c.size : reqs_p.size;
    bytes = (bytes + PS5C_HOST_ALIGN - 1) & ~(size_t)(PS5C_HOST_ALIGN - 1);

    /* mmap only promises page alignment (4 KiB host, 16 KiB PS5). */
    img->host_bytes = bytes + align;
    img->host_base = mmap( NULL, img->host_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    if (img->host_base == MAP_FAILED)
    {
        img->host_base = NULL;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    img->host = (void *)(((uintptr_t)img->host_base + align - 1) & ~(uintptr_t)(align - 1));
    if ((char *)img->host + bytes > (char *)img->host_base + img->host_bytes) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if ((res = import_host_image( d, img->host, bytes, img->image, &img->memory ))) return res;
    if ((res = import_host_image( p, img->host, bytes, img->producer_image, &img->producer_memory ))) return res;

    view.image = img->image;
    if ((res = d->p_vkCreateImageView( d->device, &view, NULL, &img->view ))) return res;
    return p->p_vkCreateFence( p->device, &fence, NULL, &img->producer_fence );
}

/* --- Retirement ------------------------------------------------------------- */

static int chain_drawable_locked( struct ps5c_compositor *c, struct ps5c_chain *chain )
{
    int in_scene = !c->scene_valid;
    if (chain->front < 0 && chain->queued < 0) return 0;
    for (uint32_t i = 0; i < c->scene_count && !in_scene; i++) in_scene = c->scene[i].key == chain->key;
    if (!in_scene) return 0;
    for (struct ps5c_chain *other = c->chains; other; other = other->next)
        if (other != chain && other->key == chain->key && other->serial > chain->serial &&
            (other->front >= 0 || other->queued >= 0))
            return 0;
    return 1;
}

static int producer_fence_done_locked( struct ps5c_chain *chain, struct chain_image *img )
{
    if (!img->fence_pending) return 1;
    if (chain->destroyed) return 0; /* destroy waits for it and clears the flag */
    if (chain->producer->p_vkGetFenceStatus( chain->producer->device, img->producer_fence ) != VK_SUCCESS) return 0;
    img->fence_pending = 0;
    return 1;
}

static void retire_locked( struct ps5c_compositor *c )
{
    struct ps5c_chain **link = &c->chains;
    int freed = 0;

    while (*link)
    {
        struct ps5c_chain *chain = *link;
        int drawable = chain->destroyed ? chain_drawable_locked( c, chain ) : 1, remaining = 0;

        for (uint32_t i = 0; i < chain->image_count; i++)
        {
            struct chain_image *img = &chain->images[i];
            if (img->reclaimed) continue;
            if (img->state == IMAGE_RETIRED && img->last_use <= c->complete_seq && producer_fence_done_locked( chain, img ))
            {
                img->state = IMAGE_FREE;
                freed = 1;
            }
            if (chain->destroyed && chain->producer_gone && img->last_use <= c->complete_seq &&
                (img->state == IMAGE_FREE || img->state == IMAGE_ACQUIRED || !drawable))
            {
                if ((int)i == chain->front) chain->front = -1;
                if ((int)i == chain->queued) chain->queued = -1;
                destroy_compositor_image_locked( c, img );
                continue;
            }
            remaining++;
        }
        if (chain->destroyed && chain->producer_gone && !remaining && !chain->pinned)
        {
            *link = chain->next;
            c->stats.live_chains--;
            free( chain );
            continue;
        }
        link = &chain->next;
    }
    if (freed) pthread_cond_broadcast( &ps5c_done );
}

static void poll_slots_locked( struct ps5c_compositor *c )
{
    struct ps5c_device *d = c->device;
    for (;;)
    {
        struct output_slot *oldest = NULL;
        for (uint32_t i = 0; i < c->slot_count; i++)
            if (c->slots[i].busy && (!oldest || c->slots[i].seq < oldest->seq)) oldest = &c->slots[i];
        if (!oldest || d->p_vkGetFenceStatus( d->device, oldest->fence ) != VK_SUCCESS) break;
        oldest->busy = 0;
        if (oldest->seq > c->complete_seq) c->complete_seq = oldest->seq;
    }
    retire_locked( c );
}

static struct output_slot *oldest_busy_slot( struct ps5c_compositor *c )
{
    struct output_slot *oldest = NULL;
    for (uint32_t i = 0; i < c->slot_count; i++)
        if (c->slots[i].busy && (!oldest || c->slots[i].seq < oldest->seq)) oldest = &c->slots[i];
    return oldest;
}

/* Unlocked wait for one slot. */
static void wait_slot_unlocked( struct ps5c_compositor *c, struct output_slot *slot )
{
    VkFence fence = slot->fence;
    pthread_mutex_unlock( &ps5c_lock );
    c->device->p_vkWaitForFences( c->device->device, 1, &fence, VK_TRUE, UINT64_MAX );
    pthread_mutex_lock( &ps5c_lock );
}

/* --- Composition ------------------------------------------------------------ */

/* Latch every chain's newest presented frame once its producer work is done. */
static void latch_locked( struct ps5c_compositor *c )
{
    for (struct ps5c_chain *chain = c->chains; chain; chain = chain->next)
    {
        struct chain_image *img;
        int index = chain->queued;

        if (index < 0) continue;
        img = &chain->images[index];
        if (img->fence_pending && !chain->destroyed)
        {
            VkFence fence = img->producer_fence;
            struct ps5c_device *producer = chain->producer;
            chain->pinned++;
            pthread_mutex_unlock( &ps5c_lock );
            producer->p_vkWaitForFences( producer->device, 1, &fence, VK_TRUE, UINT64_MAX );
            pthread_mutex_lock( &ps5c_lock );
            chain->pinned--;
            pthread_cond_broadcast( &ps5c_done );
            if (chain->queued != index) continue; /* replaced meanwhile */
            img->fence_pending = 0;
        }
        else if (img->fence_pending) continue; /* destroy is waiting for it */
        if (chain->front >= 0) chain->images[chain->front].state = IMAGE_RETIRED;
        img->state = IMAGE_FRONT;
        chain->front = index;
        chain->queued = -1;
        pthread_cond_broadcast( &ps5c_done );
    }
}

static void image_barrier( VkImageMemoryBarrier *barrier, VkImage image, VkImageLayout old_layout,
                           VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst,
                           uint32_t src_family, uint32_t dst_family )
{
    *barrier = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src,
            .dstAccessMask = dst, .oldLayout = old_layout, .newLayout = new_layout,
            .srcQueueFamilyIndex = src_family, .dstQueueFamilyIndex = dst_family, .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}

static VkResult record_and_submit( struct ps5c_compositor *c, struct output_slot *slot, const struct ps5c_plan *plan,
                                   const struct draw_entry *entries, uint32_t *output_index )
{
    struct ps5c_device *d = c->device;
    VkImageMemoryBarrier barriers[PS5C_MAX_WINDOWS + 1];
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &slot->acquired, .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1,
            .pCommandBuffers = &slot->cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &slot->rendered};
    uint32_t family = c->desc.queue_family, index, count = 0;
    VkImage target;
    VkResult res;

    res = c->desc.output.acquire( c->desc.output.context, slot->acquired, &index );
    if (res < 0) return res;
    if (index >= c->desc.output.image_count) return VK_ERROR_UNKNOWN;
    target = c->desc.output.images[index];
    if ((res = d->p_vkBeginCommandBuffer( slot->cmd, &begin ))) return res;

    if (plan->clear)
    {
        VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        image_barrier( &barriers[0], target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED );
        d->p_vkCmdPipelineBarrier( slot->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, NULL, 0, NULL, 1, barriers );
        d->p_vkCmdClearColorImage( slot->cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range );
        image_barrier( &barriers[0], target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED );
        d->p_vkCmdPipelineBarrier( slot->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                   0, 0, NULL, 0, NULL, 1, barriers );
    }
    else
    {
        VkMemoryBarrier order = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT};

        /* Window images arrive from their producers' queues (external). */
        image_barrier( &barriers[count++], target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                       0, VK_ACCESS_SHADER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED );
        for (uint32_t i = 0; i < plan->pass_count; i++)
            image_barrier( &barriers[count++], entries[i].chain->images[entries[i].image].image,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                           VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, family );
        d->p_vkCmdPipelineBarrier( slot->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   0, 0, NULL, 0, NULL, count, barriers );
        d->p_vkCmdBindPipeline( slot->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline );
        d->p_vkCmdBindDescriptorSets( slot->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout, 1, 1,
                                      &c->target_sets[index], 0, NULL );
        for (uint32_t i = 0; i < plan->pass_count; i++)
        {
            const struct ps5c_push *pass = &plan->passes[i];
            /* Later windows overwrite earlier ones: order the writes. */
            if (i) d->p_vkCmdPipelineBarrier( slot->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &order, 0, NULL, 0, NULL );
            d->p_vkCmdBindDescriptorSets( slot->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout, 0, 1,
                                          &entries[i].chain->images[entries[i].image].set, 0, NULL );
            d->p_vkCmdPushConstants( slot->cmd, c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*pass), pass );
            d->p_vkCmdDispatch( slot->cmd, (pass->dst_size[0] + 7) / 8, (pass->dst_size[1] + 7) / 8, 1 );
        }
        count = 0;
        image_barrier( &barriers[count++], target, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_ACCESS_SHADER_WRITE_BIT, 0, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED );
        for (uint32_t i = 0; i < plan->pass_count; i++)
            image_barrier( &barriers[count++], entries[i].chain->images[entries[i].image].image,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_ACCESS_SHADER_READ_BIT, 0, family, VK_QUEUE_FAMILY_EXTERNAL );
        d->p_vkCmdPipelineBarrier( slot->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, count, barriers );
    }
    if ((res = d->p_vkEndCommandBuffer( slot->cmd ))) return res;
    if ((res = d->p_vkResetFences( d->device, 1, &slot->fence ))) return res;
    if ((res = d->p_vkQueueSubmit( c->desc.queue, 1, &submit, slot->fence ))) return res;
    *output_index = index;
    slot->busy = 1;
    res = c->desc.output.present( c->desc.output.context, c->desc.queue, index, slot->rendered );
    return res < 0 ? res : VK_SUCCESS;
}

static void compose_locked( struct ps5c_compositor *c )
{
    struct ps5c_window_state states[PS5C_MAX_WINDOWS];
    struct draw_entry candidates[PS5C_MAX_WINDOWS], entries[PS5C_MAX_WINDOWS];
    uint64_t serials[PS5C_MAX_WINDOWS] = {0};
    uint8_t has_content[PS5C_MAX_WINDOWS];
    struct output_slot *slot;
    struct ps5c_plan plan;
    uint32_t count = 0, output_index = 0;
    uint64_t gen, seq;
    VkResult res;

    /* A free slot first: waiting unlocks, and nothing is captured yet. */
    slot = &c->slots[c->next_slot];
    while (slot->busy)
    {
        wait_slot_unlocked( c, slot );
        poll_slots_locked( c );
        if (c->quit) return;
    }
    gen = c->request_gen;
    c->dirty = 0;
    latch_locked( c );

    /* The scene's windows with their newest presented chain. */
    for (uint32_t i = 0; i < c->scene_count; i++)
    {
        struct ps5c_chain *best = NULL;
        for (struct ps5c_chain *chain = c->chains; chain; chain = chain->next)
            if (chain->key == c->scene[i].key && chain->front >= 0 && (!best || chain->serial > best->serial))
                best = chain;
        states[count] = c->scene[i];
        has_content[count] = !!best;
        candidates[count].chain = best;
        candidates[count].image = best ? best->front : 0;
        count++;
    }
    if (!ps5c_plan_compose( states, has_content, count, c->desc.output.extent.width,
                            c->desc.output.extent.height, &plan ))
    {
        c->composed_gen = gen;
        pthread_cond_broadcast( &ps5c_done );
        return;
    }
    for (uint32_t i = 0; i < plan.pass_count; i++)
    {
        entries[i] = candidates[plan.window[i]];
        serials[i] = entries[i].chain->serial * PS5C_MAX_CHAIN_IMAGES + entries[i].image;
    }

    /* The output already shows exactly this (e.g. a hidden window presented). */
    if (c->have_last && !memcmp( &plan, &c->last_plan, sizeof(plan) ) &&
        !memcmp( serials, c->last_serials, sizeof(serials) ))
    {
        c->stats.skipped++;
        c->composed_gen = gen;
        pthread_cond_broadcast( &ps5c_done );
        return;
    }

    c->next_slot = (c->next_slot + 1) % c->slot_count;
    seq = ++c->submit_seq;
    slot->seq = seq;
    for (uint32_t i = 0; i < plan.pass_count; i++)
    {
        entries[i].chain->images[entries[i].image].last_use = seq;
        entries[i].chain->pinned++;
    }
    if (memcmp( &plan.view, &c->stats.view, sizeof(plan.view) ))
    {
        c->stats.view = plan.view;
        if (c->desc.view_changed && plan.view.valid) c->desc.view_changed( c->desc.view_context, &plan.view );
    }

    pthread_mutex_unlock( &ps5c_lock );
    res = record_and_submit( c, slot, &plan, entries, &output_index );
    pthread_mutex_lock( &ps5c_lock );

    for (uint32_t i = 0; i < plan.pass_count; i++) entries[i].chain->pinned--;
    if (!slot->busy && seq > c->complete_seq) c->complete_seq = seq; /* nothing reached the GPU */
    if (res < 0) c->lost = res;
    else
    {
        c->stats.composes++;
        c->stats.dispatches += plan.pass_count;
        c->stats.clears += plan.clear;
        c->stats.last_dispatches = plan.pass_count;
        c->stats.last_clears = plan.clear;
        c->stats.last_output_index = output_index;
        c->last_plan = plan;
        memcpy( c->last_serials, serials, sizeof(serials) );
        c->have_last = 1;
    }
    c->composed_gen = gen;
    pthread_cond_broadcast( &ps5c_done );
}

static void *compositor_thread( void *arg )
{
    struct ps5c_compositor *c = arg;

    pthread_mutex_lock( &ps5c_lock );
    for (;;)
    {
        poll_slots_locked( c );
        if (c->quit) break;
        if (c->dirty && !c->lost)
        {
            compose_locked( c );
            continue;
        }
        if (oldest_busy_slot( c ))
        {
            wait_slot_unlocked( c, oldest_busy_slot( c ) );
            continue;
        }
        if (c->dirty) c->composed_gen = c->request_gen; /* lost: nothing more to show */
        c->dirty = 0;
        pthread_cond_broadcast( &ps5c_done );
        pthread_cond_wait( &ps5c_work, &ps5c_lock );
    }
    pthread_mutex_unlock( &ps5c_lock );
    return NULL;
}

/* --- Compositor lifetime ---------------------------------------------------- */

static void destroy_compositor_objects( struct ps5c_compositor *c )
{
    struct ps5c_device *d = c->device;
    for (uint32_t i = 0; i < c->slot_count; i++)
    {
        if (c->slots[i].fence) d->p_vkDestroyFence( d->device, c->slots[i].fence, NULL );
        if (c->slots[i].acquired) d->p_vkDestroySemaphore( d->device, c->slots[i].acquired, NULL );
        if (c->slots[i].rendered) d->p_vkDestroySemaphore( d->device, c->slots[i].rendered, NULL );
    }
    if (c->cmd_pool) d->p_vkDestroyCommandPool( d->device, c->cmd_pool, NULL );
    for (uint32_t i = 0; i < c->desc.output.image_count; i++)
        if (c->target_views[i]) d->p_vkDestroyImageView( d->device, c->target_views[i], NULL );
    if (c->pipeline) d->p_vkDestroyPipeline( d->device, c->pipeline, NULL );
    if (c->layout) d->p_vkDestroyPipelineLayout( d->device, c->layout, NULL );
    if (c->pool) d->p_vkDestroyDescriptorPool( d->device, c->pool, NULL );
    if (c->source_layout) d->p_vkDestroyDescriptorSetLayout( d->device, c->source_layout, NULL );
    if (c->target_layout) d->p_vkDestroyDescriptorSetLayout( d->device, c->target_layout, NULL );
    if (c->sampler) d->p_vkDestroySampler( d->device, c->sampler, NULL );
}

static VkResult create_compositor_objects( struct ps5c_compositor *c )
{
    struct ps5c_device *d = c->device;
    const struct ps5c_output *output = &c->desc.output;
    VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkDescriptorSetLayoutBinding source_binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutBinding target_binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1};
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PS5C_MAX_SOURCE_SETS},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, PS5C_MAX_OUTPUT_IMAGES}};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = PS5C_MAX_SOURCE_SETS + PS5C_MAX_OUTPUT_IMAGES, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct ps5c_push)};
    VkDescriptorSetLayout set_layouts[2];
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 2, .pSetLayouts = set_layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(ps5c_compose_spv), .pCode = ps5c_compose_spv};
    VkComputePipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main"}};
    VkCommandPoolCreateInfo cmd_pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->desc.queue_family};
    VkCommandBufferAllocateInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkShaderModule module;
    VkResult res;

    if ((res = d->p_vkCreateSampler( d->device, &sampler, NULL, &c->sampler ))) return res;
    layout_info.pBindings = &source_binding;
    if ((res = d->p_vkCreateDescriptorSetLayout( d->device, &layout_info, NULL, &c->source_layout ))) return res;
    layout_info.pBindings = &target_binding;
    if ((res = d->p_vkCreateDescriptorSetLayout( d->device, &layout_info, NULL, &c->target_layout ))) return res;
    if ((res = d->p_vkCreateDescriptorPool( d->device, &pool_info, NULL, &c->pool ))) return res;
    set_layouts[0] = c->source_layout;
    set_layouts[1] = c->target_layout;
    if ((res = d->p_vkCreatePipelineLayout( d->device, &pipeline_layout, NULL, &c->layout ))) return res;
    if ((res = d->p_vkCreateShaderModule( d->device, &module_info, NULL, &module ))) return res;
    pipeline_info.stage.module = module;
    pipeline_info.layout = c->layout;
    res = d->p_vkCreateComputePipelines( d->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &c->pipeline );
    d->p_vkDestroyShaderModule( d->device, module, NULL );
    if (res) return res;

    for (uint32_t i = 0; i < output->image_count; i++)
    {
        VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT};
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage,
                .image = output->images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = output->format,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = c->pool, .descriptorSetCount = 1, .pSetLayouts = &c->target_layout};
        VkDescriptorImageInfo image_info = {.imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &image_info};

        if ((res = d->p_vkCreateImageView( d->device, &view, NULL, &c->target_views[i] ))) return res;
        if ((res = d->p_vkAllocateDescriptorSets( d->device, &set_info, &c->target_sets[i] ))) return res;
        image_info.imageView = c->target_views[i];
        write.dstSet = c->target_sets[i];
        d->p_vkUpdateDescriptorSets( d->device, 1, &write, 0, NULL );
    }

    if ((res = d->p_vkCreateCommandPool( d->device, &cmd_pool, NULL, &c->cmd_pool ))) return res;
    cmd_info.commandPool = c->cmd_pool;
    c->slot_count = output->image_count;
    for (uint32_t i = 0; i < c->slot_count; i++)
    {
        struct output_slot *slot = &c->slots[i];
        if ((res = d->p_vkAllocateCommandBuffers( d->device, &cmd_info, &slot->cmd ))) return res;
        if ((res = d->p_vkCreateFence( d->device, &fence, NULL, &slot->fence ))) return res;
        if ((res = d->p_vkCreateSemaphore( d->device, &semaphore, NULL, &slot->acquired ))) return res;
        if ((res = d->p_vkCreateSemaphore( d->device, &semaphore, NULL, &slot->rendered ))) return res;
    }
    return VK_SUCCESS;
}

VkResult ps5c_compositor_create( const struct ps5c_compositor_desc *desc, struct ps5c_compositor **out )
{
    struct ps5c_compositor *c;
    VkResult res;

    *out = NULL;
    if (!desc->device || !desc->output.image_count || desc->output.image_count > PS5C_MAX_OUTPUT_IMAGES ||
        !desc->output.acquire || !desc->output.present || !desc->output.extent.width || !desc->output.extent.height)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!(c = calloc( 1, sizeof(*c) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    c->desc = *desc;
    c->device = desc->device;
    if ((res = create_compositor_objects( c )))
    {
        destroy_compositor_objects( c );
        free( c );
        return res;
    }
    if (pthread_create( &c->thread, NULL, compositor_thread, c ))
    {
        destroy_compositor_objects( c );
        free( c );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    c->thread_started = 1;
    *out = c;
    return VK_SUCCESS;
}

void ps5c_compositor_destroy( struct ps5c_compositor *c )
{
    if (!c) return;
    pthread_mutex_lock( &ps5c_lock );
    c->quit = 1;
    pthread_cond_broadcast( &ps5c_work );
    pthread_mutex_unlock( &ps5c_lock );
    if (c->thread_started) pthread_join( c->thread, NULL );
    c->device->p_vkQueueWaitIdle( c->desc.queue );
    pthread_mutex_lock( &ps5c_lock );
    while (c->chains)
    {
        struct ps5c_chain *chain = c->chains;
        c->chains = chain->next;
        /* Producers must have destroyed their chains; anything left keeps
         * its producer objects but loses the shared memory. */
        for (uint32_t i = 0; i < chain->image_count; i++)
            if (!chain->images[i].reclaimed) destroy_compositor_image_locked( c, &chain->images[i] );
        free( chain );
    }
    pthread_mutex_unlock( &ps5c_lock );
    destroy_compositor_objects( c );
    free( c );
}

void ps5c_scene_update( struct ps5c_compositor *c, const struct ps5c_window_state *states, uint32_t count )
{
    if (count > PS5C_MAX_WINDOWS) count = PS5C_MAX_WINDOWS;
    pthread_mutex_lock( &ps5c_lock );
    if (!c->scene_valid || c->scene_count != count || memcmp( c->scene, states, count * sizeof(*states) ))
    {
        memcpy( c->scene, states, count * sizeof(*states) );
        c->scene_count = count;
        c->scene_valid = 1;
        wake_compositor_locked( c );
    }
    pthread_mutex_unlock( &ps5c_lock );
}

uint32_t ps5c_window_keys( struct ps5c_compositor *c, uint64_t *keys, uint32_t capacity )
{
    uint32_t count = 0;
    pthread_mutex_lock( &ps5c_lock );
    for (struct ps5c_chain *chain = c->chains; chain; chain = chain->next)
    {
        uint32_t i;
        for (i = 0; i < count; i++) if (keys[i] == chain->key) break;
        if (i == count && count < capacity) keys[count++] = chain->key;
    }
    pthread_mutex_unlock( &ps5c_lock );
    return count;
}

VkResult ps5c_compositor_flush( struct ps5c_compositor *c )
{
    uint64_t target;
    VkResult res;

    pthread_mutex_lock( &ps5c_lock );
    wake_compositor_locked( c );
    target = c->request_gen;
    while (!c->lost && (c->composed_gen < target || c->complete_seq < c->submit_seq))
    {
        pthread_cond_broadcast( &ps5c_work );
        pthread_cond_wait( &ps5c_done, &ps5c_lock );
    }
    res = c->lost;
    pthread_mutex_unlock( &ps5c_lock );
    return res;
}

void ps5c_get_stats( struct ps5c_compositor *c, struct ps5c_stats *stats )
{
    pthread_mutex_lock( &ps5c_lock );
    *stats = c->stats;
    pthread_mutex_unlock( &ps5c_lock );
}

/* --- Chains ------------------------------------------------------------------ */

static void free_chain_unlinked( struct ps5c_compositor *c, struct ps5c_chain *chain )
{
    for (uint32_t i = 0; i < chain->image_count; i++)
    {
        destroy_producer_image( chain, &chain->images[i] );
        pthread_mutex_lock( &ps5c_lock );
        destroy_compositor_image_locked( c, &chain->images[i] );
        pthread_mutex_unlock( &ps5c_lock );
    }
    for (uint32_t f = 0; f < chain->family_count; f++)
        chain->producer->p_vkDestroyCommandPool( chain->producer->device, chain->pools[f], NULL );
    free( chain );
}

VkResult ps5c_chain_create( struct ps5c_compositor *c, uint64_t window_key, struct ps5c_device *producer,
                            const struct ps5c_chain_desc *desc, struct ps5c_chain **out )
{
    struct ps5c_chain *chain;
    VkResult res = VK_SUCCESS;

    *out = NULL;
    if (!producer || desc->image_count < 2 || desc->image_count > PS5C_MAX_CHAIN_IMAGES ||
        !srgb_format( desc->format ) || !desc->extent.width || !desc->extent.height ||
        desc->view_format_count > PS5C_MAX_VIEW_FORMATS)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < desc->view_format_count; i++)
        if (!srgb_format( desc->view_formats[i] )) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!(chain = calloc( 1, sizeof(*chain) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    chain->compositor = c;
    chain->key = window_key;
    chain->producer = producer;
    chain->image_count = desc->image_count;
    chain->front = chain->queued = -1;

    for (uint32_t i = 0; i < chain->image_count && !res; i++)
        res = create_chain_image( c, chain, desc, &chain->images[i] );
    pthread_mutex_lock( &ps5c_lock );
    for (uint32_t i = 0; i < chain->image_count && !res; i++)
        res = account_chain_image_locked( c, &chain->images[i] );
    if (!res) res = c->lost;
    pthread_mutex_unlock( &ps5c_lock );
    if (res)
    {
        free_chain_unlinked( c, chain );
        return res;
    }
    pthread_mutex_lock( &ps5c_lock );
    chain->serial = ++c->next_serial;
    chain->next = c->chains;
    c->chains = chain;
    c->stats.live_chains++;
    pthread_mutex_unlock( &ps5c_lock );
    *out = chain;
    return VK_SUCCESS;
}

uint32_t ps5c_chain_images( struct ps5c_chain *chain, VkImage *images, uint32_t capacity )
{
    uint32_t count = capacity < chain->image_count ? capacity : chain->image_count;
    for (uint32_t i = 0; i < count; i++) images[i] = chain->images[i].producer_image;
    return chain->image_count;
}

VkResult ps5c_chain_acquire( struct ps5c_chain *chain, uint64_t timeout_ns, uint32_t *index )
{
    struct ps5c_compositor *c = chain->compositor;
    uint64_t waited = 0;
    VkResult res;

    pthread_mutex_lock( &ps5c_lock );
    for (;;)
    {
        if (c->lost)
        {
            res = VK_ERROR_SURFACE_LOST_KHR;
            break;
        }
        if (chain->queued < 0)
        {
            uint32_t i;
            for (i = 0; i < chain->image_count; i++) if (chain->images[i].state == IMAGE_FREE) break;
            if (i < chain->image_count)
            {
                chain->images[i].state = IMAGE_ACQUIRED;
                *index = i;
                res = VK_SUCCESS;
                break;
            }
        }
        if (!timeout_ns)
        {
            res = VK_NOT_READY;
            break;
        }
        if (timeout_ns == UINT64_MAX)
        {
            pthread_cond_wait( &ps5c_done, &ps5c_lock );
            continue;
        }
        if (waited >= timeout_ns)
        {
            res = VK_TIMEOUT;
            break;
        }
        /* Finite timeouts poll at millisecond granularity. */
        pthread_mutex_unlock( &ps5c_lock );
        usleep( 1000 );
        waited += 1000000;
        pthread_mutex_lock( &ps5c_lock );
    }
    pthread_mutex_unlock( &ps5c_lock );
    return res;
}

static int chain_family_slot( struct ps5c_chain *chain, uint32_t family )
{
    VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    uint32_t f;

    for (f = 0; f < chain->family_count; f++) if (chain->families[f] == family) return f;
    if (f == PS5C_MAX_QUEUE_FAMILIES) return -1;
    if (chain->producer->p_vkCreateCommandPool( chain->producer->device, &info, NULL, &chain->pools[f] )) return -1;
    chain->families[f] = family;
    chain->family_count++;
    return f;
}

static VkResult record_release( struct ps5c_chain *chain, struct chain_image *img, int f )
{
    struct ps5c_device *p = chain->producer;
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = chain->pools[f], .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkImageMemoryBarrier barrier;
    VkResult res;

    if ((res = p->p_vkAllocateCommandBuffers( p->device, &alloc, &img->release[f] ))) return res;
    if ((res = p->p_vkBeginCommandBuffer( img->release[f], &begin ))) return res;
    /* Hand the presented image to the compositor's device. */
    image_barrier( &barrier, img->producer_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_ACCESS_MEMORY_WRITE_BIT, 0, chain->families[f], VK_QUEUE_FAMILY_EXTERNAL );
    p->p_vkCmdPipelineBarrier( img->release[f], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier );
    return p->p_vkEndCommandBuffer( img->release[f] );
}

VkResult ps5c_chain_present( struct ps5c_chain *chain, VkQueue queue, uint32_t queue_family, uint32_t index,
                             uint32_t wait_count, const VkSemaphore *waits )
{
    struct ps5c_compositor *c = chain->compositor;
    struct ps5c_device *p = chain->producer;
    VkPipelineStageFlags stages[16];
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = wait_count,
            .pWaitSemaphores = waits, .pWaitDstStageMask = stages, .commandBufferCount = 1};
    struct chain_image *img;
    int f;
    VkResult res;

    if (index >= chain->image_count || wait_count > 16) return VK_ERROR_UNKNOWN;
    img = &chain->images[index];
    pthread_mutex_lock( &ps5c_lock );
    res = img->state != IMAGE_ACQUIRED ? VK_ERROR_UNKNOWN : c->lost;
    pthread_mutex_unlock( &ps5c_lock );
    if (res) return res;

    if ((f = chain_family_slot( chain, queue_family )) < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!img->release[f] && (res = record_release( chain, img, f ))) return res;
    for (uint32_t i = 0; i < wait_count; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    submit.pCommandBuffers = &img->release[f];
    if ((res = p->p_vkResetFences( p->device, 1, &img->producer_fence ))) return res;
    if ((res = p->p_vkQueueSubmit( queue, 1, &submit, img->producer_fence ))) return res;

    pthread_mutex_lock( &ps5c_lock );
    img->fence_pending = 1;
    if (chain->queued >= 0)
    {
        /* An unlatched older frame is dropped once its release completes. */
        chain->images[chain->queued].state = IMAGE_RETIRED;
    }
    img->state = IMAGE_QUEUED;
    chain->queued = index;
    wake_compositor_locked( c );
    pthread_mutex_unlock( &ps5c_lock );
    return VK_SUCCESS;
}

void ps5c_chain_release( struct ps5c_chain *chain, uint32_t index )
{
    if (index >= chain->image_count) return;
    pthread_mutex_lock( &ps5c_lock );
    if (chain->images[index].state == IMAGE_ACQUIRED)
    {
        chain->images[index].state = IMAGE_FREE;
        pthread_cond_broadcast( &ps5c_done );
    }
    pthread_mutex_unlock( &ps5c_lock );
}

void ps5c_chain_destroy( struct ps5c_chain *chain )
{
    struct ps5c_compositor *c;
    struct ps5c_device *p;
    VkFence fences[PS5C_MAX_CHAIN_IMAGES];
    uint32_t count = 0;

    if (!chain) return;
    c = chain->compositor;
    p = chain->producer;
    pthread_mutex_lock( &ps5c_lock );
    chain->destroyed = 1;
    while (chain->pinned) pthread_cond_wait( &ps5c_done, &ps5c_lock );
    for (uint32_t i = 0; i < chain->image_count; i++)
        if (chain->images[i].fence_pending) fences[count++] = chain->images[i].producer_fence;
    pthread_mutex_unlock( &ps5c_lock );

    /* The producer device may go right after this: finish with it now. */
    if (count) p->p_vkWaitForFences( p->device, count, fences, VK_TRUE, UINT64_MAX );
    for (uint32_t i = 0; i < chain->image_count; i++) destroy_producer_image( chain, &chain->images[i] );
    for (uint32_t f = 0; f < chain->family_count; f++) p->p_vkDestroyCommandPool( p->device, chain->pools[f], NULL );
    chain->family_count = 0;

    pthread_mutex_lock( &ps5c_lock );
    for (uint32_t i = 0; i < chain->image_count; i++) chain->images[i].fence_pending = 0;
    chain->producer_gone = 1;
    wake_compositor_locked( c );
    pthread_mutex_unlock( &ps5c_lock );
}
