/* Metadata-only RuneLite evidence policy. No command lines, window text,
 * arbitrary thread descriptions, environment values or image paths escape. */
#ifndef __WINE_PS5_RUNELITE_DIAG_H
#define __WINE_PS5_RUNELITE_DIAG_H

#include <stddef.h>
#include <stdint.h>

static inline int wine_ps5_runelite_enabled(const char *marker)
{
#if defined(__PROSPERO__) && defined(WINE_PS5_RUNESCAPE)
    return marker && marker[0] == '1' && !marker[1];
#else
    (void)marker;
    return 0;
#endif
}

#define WINE_PS5_RUNELITE_ENV_MAX_BYTES (128u * 1024u)

/* EnvironmentSize is a byte bound, not permission to scan until an arbitrary
 * NUL. Reject incomplete blocks and duplicate keys instead of guessing. */
static inline int wine_ps5_runelite_pe_marker(const uint16_t *environment, size_t bytes)
{
#if defined(__PROSPERO__) && defined(WINE_PS5_RUNESCAPE)
    static const char key[] = "WINEPS5_RUNELITE_DIAG";
    size_t units, at = 0;
    int seen = 0, enabled = 0;
    if (!environment || bytes < 2 * sizeof(uint16_t) || bytes % sizeof(uint16_t) ||
        bytes > WINE_PS5_RUNELITE_ENV_MAX_BYTES) return 0;
    units = bytes / sizeof(uint16_t);
    while (at < units) {
        size_t end = at, i;
        int matches = 1;
        if (!environment[at]) return at ? enabled : 0;
        while (end < units && environment[end]) end++;
        if (end == units) return 0;
        if (end - at >= sizeof(key) && environment[at + sizeof(key) - 1] == '=') {
            for (i = 0; i < sizeof(key) - 1; i++) {
                uint16_t ch = environment[at + i];
                if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                if (ch != (unsigned char)key[i]) { matches = 0;break; }
            }
            if (matches) {
                if (seen++) return 0;
                enabled = end - at == sizeof(key) + 1 && environment[end - 1] == '1';
            }
        }
        at = end + 1;
    }
    return 0;
#else
    (void)environment;(void)bytes;
    return 0;
#endif
}

static inline int wine_ps5_runelite_process_enabled(const char *unix_marker,
                                                   const uint16_t *environment, size_t bytes)
{
    /* An explicit Unix zero must override a possibly inherited PE marker. */
    return unix_marker ? wine_ps5_runelite_enabled(unix_marker) :
                         wine_ps5_runelite_pe_marker(environment, bytes);
}

static inline int wine_ps5_runelite_equal(const uint16_t *text, size_t units, const char *known)
{
    size_t i;
    if (!text) return 0;
    for (i = 0; i < units && known[i]; i++) if (text[i] != (unsigned char)known[i]) return 0;
    return i == units && !known[i];
}

static inline int wine_ps5_runelite_jvm_image(const uint16_t *name, size_t units)
{
    static const char image[] = "jvm.dll";
    size_t first, i;
    if (!name || units < sizeof(image) - 1) return 0;
    first = units - (sizeof(image) - 1);
    if (first && name[first - 1] != '/' && name[first - 1] != '\\') return 0;
    for (i = 0; image[i]; i++) {
        uint16_t ch = name[first + i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != (unsigned char)image[i]) return 0;
    }
    for (i = 0; i < first; i++) if (!name[i]) return 0;
    return 1;
}

static inline int wine_ps5_runelite_compiler_name(const uint16_t *name, size_t units,
                                                 unsigned *tier, unsigned *index)
{
    static const char suffix[] = " CompilerThread";
    size_t first = 2 + sizeof(suffix) - 1, i;
    unsigned number = 0;
    if (!name || !tier || !index || units <= first || units > first + 5 ||
        name[0] != 'C' || (name[1] != '1' && name[1] != '2')) return 0;
    if (!wine_ps5_runelite_equal(name + 2, sizeof(suffix) - 1, suffix)) return 0;
    for (i = first; i < units; i++) {
        if (name[i] < '0' || name[i] > '9') return 0;
        number = number * 10 + name[i] - '0';
    }
    if (number > 65535) return 0;
    *tier = name[1] - '0';*index = number;
    return 1;
}

static inline const char *wine_ps5_runelite_awt_class(const uint16_t *name, size_t units)
{
    static const char *const classes[] = {
        "SunAwtFrame", "SunAwtDialog", "SunAwtCanvas", "SunAwtWindow",
        "SunAwtHidden", "SunAwtToolkit", "SunAwtLightweightFrame"
    };
    size_t i;
    for (i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
        if (wine_ps5_runelite_equal(name, units, classes[i])) return classes[i];
    return NULL;
}

static inline unsigned wine_ps5_runelite_sample(volatile uint32_t *count, unsigned limit)
{
    uint32_t value = __atomic_load_n(count, __ATOMIC_RELAXED);
    while (value < limit)
        if (__atomic_compare_exchange_n(count, &value, value + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return value + 1;
    return 0;
}

#endif
