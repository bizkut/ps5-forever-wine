/* Battle.net maintenance watcher gate, crash classification, and restart budget. */
#ifndef __WINE_BATTLENET_RESTART_H
#define __WINE_BATTLENET_RESTART_H

#include <stdint.h>
#include <wchar.h>

#define BATTLENET_RESTART_LIMIT 3u
#define BATTLENET_RESTART_WINDOW_MS UINT64_C(600000)

static inline int battlenet_watch_space( wchar_t ch )
{
    return ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r' ||
           ch == L'\v' || ch == L'\f';
}

static inline wchar_t battlenet_watch_fold_ascii( wchar_t ch )
{
    return ch >= L'A' && ch <= L'Z' ? ch + (L'a' - L'A') : ch;
}

static inline int battlenet_watch_enabled( const wchar_t *marker, const wchar_t *command,
                                           const wchar_t *launcher )
{
    const wchar_t *start, *end;
    size_t i, length, launcher_length;

    if (!marker || marker[0] != L'1' || marker[1] || !command || !launcher) return 0;
    start = command;
    while (battlenet_watch_space( *start )) start++;
    end = start + wcslen( start );
    while (end > start && battlenet_watch_space( end[-1] )) end--;
    if (end - start >= 2 && start[0] == L'"' && end[-1] == L'"')
    {
        start++;
        end--;
    }
    length = end - start;
    launcher_length = wcslen( launcher );
    if (length != launcher_length) return 0;
    for (i = 0; i < length; ++i)
        if (battlenet_watch_fold_ascii( start[i] ) !=
            battlenet_watch_fold_ascii( launcher[i] )) return 0;
    return 1;
}

struct battlenet_restart_budget
{
    uint64_t times[BATTLENET_RESTART_LIMIT];
    unsigned int count;
};

enum battlenet_restart_action
{
    BATTLENET_RESTART_NONE,
    BATTLENET_RESTART_LAUNCH,
    BATTLENET_RESTART_CLOSE
};

static inline int battlenet_exit_is_crash( uint32_t status )
{
    /* The title uses -1 for TerminateProcess and STATUS_CANCELLED for its
     * Play hand-off. STATUS_FATAL_APP_EXIT is an exception despite its
     * informational NTSTATUS severity. */
    if (status == UINT32_C(0xffffffff) || status == UINT32_C(0xc0000120)) return 0;
    if (status == UINT32_C(0x40000015)) return 1;
    return (status & UINT32_C(0xc0000000)) == UINT32_C(0x80000000) ||
           (status & UINT32_C(0xc0000000)) == UINT32_C(0xc0000000);
}

static inline enum battlenet_restart_action battlenet_restart_decide(
    struct battlenet_restart_budget *budget, uint32_t status, uint64_t now,
    unsigned int *restart_number )
{
    unsigned int i, live = 0;

    if (restart_number) *restart_number = 0;
    if (!budget || !battlenet_exit_is_crash( status )) return BATTLENET_RESTART_NONE;

    for (i = 0; i < budget->count; ++i)
    {
        /* GetTickCount64 is monotonic. If a synthetic caller moves backwards,
         * retain the entry rather than accidentally bypassing the budget. */
        if (now < budget->times[i] || now - budget->times[i] < BATTLENET_RESTART_WINDOW_MS)
            budget->times[live++] = budget->times[i];
    }
    budget->count = live;
    if (live == BATTLENET_RESTART_LIMIT) return BATTLENET_RESTART_CLOSE;

    budget->times[live] = now;
    budget->count = ++live;
    if (restart_number) *restart_number = live;
    return BATTLENET_RESTART_LAUNCH;
}

#endif /* __WINE_BATTLENET_RESTART_H */
