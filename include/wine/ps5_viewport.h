/* RuneScape title presentation geometry shared by the existing GPU blit and
 * its input adapter. Coordinates are pixels, not normalized Vulkan UVs. */
#ifndef WINE_PS5_VIEWPORT_H
#define WINE_PS5_VIEWPORT_H

#include <stdint.h>

struct wine_ps5_viewport
{
    uint32_t image_width, image_height;
    uint32_t output_width, output_height;
    uint32_t left, top, width, height;
};

struct wine_ps5_viewport_map
{
    struct wine_ps5_viewport viewport;
    int32_t origin_x, origin_y;
    uint32_t client_width, client_height;
};

static inline int wine_ps5_viewport_fit( struct wine_ps5_viewport *view, uint32_t image_width,
        uint32_t image_height, uint32_t output_width, uint32_t output_height )
{
    uint32_t width, height;

    if (!image_width || !image_height || !output_width || !output_height ||
        image_width > INT32_MAX || image_height > INT32_MAX ||
        output_width > INT32_MAX || output_height > INT32_MAX) return 0;
    if ((uint64_t)image_width * output_height > (uint64_t)image_height * output_width)
    {
        width = output_width;
        height = (uint64_t)image_height * output_width / image_width;
        if (!height) height = 1;
    }
    else
    {
        height = output_height;
        width = (uint64_t)image_width * output_height / image_height;
        if (!width) width = 1;
    }
    *view = (struct wine_ps5_viewport){image_width, image_height, output_width, output_height,
            (output_width - width) / 2, (output_height - height) / 2, width, height};
    return 1;
}

/* Preserve both pixel endpoints, clamp black bars/warps to the client edge,
 * and use 64-bit products so ordinary desktop offsets cannot overflow. */
static inline uint32_t wine_ps5_viewport_axis( int64_t position, uint32_t from, uint32_t to )
{
    if (to <= 1) return 0;
    if (from <= 1) return (to - 1) / 2;
    if (position < 0) position = 0;
    if ((uint64_t)position >= from) position = from - 1;
    return ((uint64_t)position * (to - 1) + (from - 1) / 2) / (from - 1);
}

static inline int32_t wine_ps5_viewport_add( int32_t origin, uint32_t offset )
{
    int64_t position = (int64_t)origin + offset;
    return position > INT32_MAX ? INT32_MAX : (int32_t)position;
}

static inline void wine_ps5_viewport_to_output( const struct wine_ps5_viewport_map *map,
        int32_t x, int32_t y, int32_t *output_x, int32_t *output_y )
{
    *output_x = map->viewport.left + wine_ps5_viewport_axis( (int64_t)x - map->origin_x,
            map->client_width, map->viewport.width );
    *output_y = map->viewport.top + wine_ps5_viewport_axis( (int64_t)y - map->origin_y,
            map->client_height, map->viewport.height );
}

static inline void wine_ps5_viewport_to_client( const struct wine_ps5_viewport_map *map,
        int32_t x, int32_t y, int32_t *client_x, int32_t *client_y )
{
    *client_x = wine_ps5_viewport_add( map->origin_x, wine_ps5_viewport_axis(
            (int64_t)x - map->viewport.left, map->viewport.width, map->client_width ) );
    *client_y = wine_ps5_viewport_add( map->origin_y, wine_ps5_viewport_axis(
            (int64_t)y - map->viewport.top, map->viewport.height, map->client_height ) );
}

#endif
