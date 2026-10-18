/* PS5 title request diagnostics. No wire-layout change: only explicitly
 * checked requests carry a timestamp in otherwise unused generic union bytes. */
#ifndef __WINE_PS5_REQUEST_PROFILE_H
#define __WINE_PS5_REQUEST_PROFILE_H

#include <stdint.h>
#include <string.h>

static inline int wine_ps5_request_profile_supported( int req )
{
    _Static_assert( sizeof(union generic_request) == 64, "request diagnostic tail" );
    _Static_assert( sizeof(struct select_request) <= 48, "select diagnostic tail" );
    _Static_assert( sizeof(struct event_op_request) <= 48, "event diagnostic tail" );
    _Static_assert( sizeof(struct remove_completion_request) <= 48, "completion diagnostic tail" );
    _Static_assert( sizeof(struct add_completion_request) <= 48, "completion diagnostic tail" );
    _Static_assert( sizeof(struct set_queue_mask_request) <= 48, "queue diagnostic tail" );
    return req == REQ_select || req == REQ_event_op || req == REQ_remove_completion ||
           req == REQ_add_completion || req == REQ_set_queue_mask;
}

static inline void wine_ps5_request_profile_stamp( union generic_request *req, uint64_t ns )
{
    const uint64_t stamp[2] = { ns, UINT64_C(0x505335525154494d) };
    if (wine_ps5_request_profile_supported( req->request_header.req ))
        memcpy( (char *)req + 48, stamp, sizeof(stamp) );
}

static inline uint64_t wine_ps5_request_profile_started( const union generic_request *req )
{
    uint64_t stamp[2];
    if (!wine_ps5_request_profile_supported( req->request_header.req )) return 0;
    memcpy( stamp, (const char *)req + 48, sizeof(stamp) );
    return stamp[1] == UINT64_C(0x505335525154494d) ? stamp[0] : 0;
}

#endif
