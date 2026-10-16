/* Private native-title callbacks, consumed before Wine startup on PS5.
 * The native FSELF owns thread bookkeeping and libc's title TLS context.
 */
#ifndef __WINE_PS5_TITLE_H
#define __WINE_PS5_TITLE_H

#if defined(__PROSPERO__) || defined(WINE_PS5_TITLE_HOST_TEST)
#include <pthread.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#define WINE_PS5_TITLE_RUNTIME_VERSION 12

#define PS5_TITLE_SURFACE_CREATE 1
#define PS5_TITLE_SURFACE_OPEN 2
#define PS5_TITLE_SURFACE_UNLINK 3

struct wine_ps5_display_mode
{
    unsigned int width, height, refresh_millihz;
};

#define WINE_PS5_HDR_EDID_SIZE 256

struct wine_ps5_hdr_luminance
{
    float max_full_frame_luminance;
    float max_luminance;
    float min_luminance;
};

#define WINE_PS5_SDR_WHITE_MIN_NITS 80u
#define WINE_PS5_SDR_WHITE_DEFAULT_NITS 203u
#define WINE_PS5_SDR_WHITE_MAX_NITS 480u

/* DISPLAYCONFIG_SDR_WHITE_LEVEL uses 1000 for 80 cd/m2.  Zero rejects
 * values outside Windows' 80..480-nit SDR content brightness range. */
static inline unsigned int wine_ps5_sdr_white_level_from_nits( unsigned int nits )
{
    if (nits < WINE_PS5_SDR_WHITE_MIN_NITS || nits > WINE_PS5_SDR_WHITE_MAX_NITS) return 0;
    return (nits * 1000u + 40u) / 80u;
}

/* CTA-861.3 encodes the two maxima as 50 * 2^(code / 32).  Keeping the
 * fractional powers here avoids adding libm imports to the title win32u. */
static inline float wine_ps5_hdr_decode_max_luminance( unsigned int code )
{
    static const float fraction[32] =
    {
        1.0f, 1.02189715f, 1.04427378f, 1.06714040f,
        1.09050773f, 1.11438674f, 1.13878863f, 1.16372486f,
        1.18920712f, 1.21524736f, 1.24185781f, 1.26905096f,
        1.29683955f, 1.32523664f, 1.35425555f, 1.38390988f,
        1.41421356f, 1.44518081f, 1.47682615f, 1.50916443f,
        1.54221083f, 1.57598085f, 1.61049033f, 1.64575548f,
        1.68179283f, 1.71861930f, 1.75625216f, 1.79470908f,
        1.83400809f, 1.87416763f, 1.91520656f, 1.95714412f,
    };

    if (!code) return 0.0f;
    if (code > 255) code = 255;
    return 50.0f * (float)(1u << (code >> 5)) * fraction[code & 31];
}

static inline unsigned char wine_ps5_hdr_encode_max_luminance( float value )
{
    unsigned int low = 1, high = 255, middle;
    float low_value, high_value;

    if (!(value > 0.0f)) return 0;
    if (value <= wine_ps5_hdr_decode_max_luminance( low )) return low;
    if (value >= wine_ps5_hdr_decode_max_luminance( high )) return high;
    while (high - low > 1)
    {
        middle = (low + high) / 2;
        if (wine_ps5_hdr_decode_max_luminance( middle ) < value) low = middle;
        else high = middle;
    }
    low_value = wine_ps5_hdr_decode_max_luminance( low );
    high_value = wine_ps5_hdr_decode_max_luminance( high );
    return value - low_value <= high_value - value ? low : high;
}

/* CTA-861.3 minimum = maximum * (code / 255)^2 / 100. */
static inline float wine_ps5_hdr_decode_min_luminance( float maximum, unsigned int code )
{
    if (!code) return 0.0f;
    if (code > 255) code = 255;
    return maximum * (float)(code * code) / 6502500.0f;
}

static inline unsigned char wine_ps5_hdr_encode_min_luminance( float minimum, float maximum )
{
    unsigned int low = 1, high = 255, middle;
    float target, low_distance, high_distance;

    if (!(minimum > 0.0f) || !(maximum > 0.0f)) return 0;
    target = minimum / maximum * 6502500.0f;
    if (target <= 1.0f) return 1;
    if (target >= 65025.0f) return 255;
    while (high - low > 1)
    {
        middle = (low + high) / 2;
        if ((float)(middle * middle) < target) low = middle;
        else high = middle;
    }
    low_distance = target - (float)(low * low);
    high_distance = (float)(high * high) - target;
    return low_distance <= high_distance ? low : high;
}

static inline void wine_ps5_hdr_edid_checksum( unsigned char *block )
{
    unsigned int i;
    unsigned char sum = 0;

    for (i = 0; i < 127; i++) sum += block[i];
    block[127] = (unsigned char)(0 - sum);
}

/* Build one synthetic monitor EDID.  The console API exposes calibrated
 * luminance but no panel chromaticities, so advertise the HDR10 signalling
 * gamut (BT.2020 with D65) rather than inventing panel-specific primaries. */
static inline unsigned int wine_ps5_build_hdr_edid( unsigned char *edid,
                                                    const struct wine_ps5_hdr_luminance *luminance,
                                                    unsigned int width, unsigned int height )
{
    static const unsigned char header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    static const char name[13] = {'P','S','5',' ','H','D','R',' ','T','V','\n',' ',' '};
    unsigned char *cta, *timing;
    unsigned char max_code, max_full_frame_code, min_code;
    unsigned int i, hblank, vblank, hsync_offset, hsync_width;
    uint64_t pixel_clock;
    float encoded_max;

    if (!edid || !luminance || !width || !height || width > 4095 || height > 4095 ||
        !(luminance->max_luminance > 0.0f) ||
        !(luminance->max_full_frame_luminance > 0.0f) ||
        !(luminance->min_luminance >= 0.0f) ||
        luminance->max_luminance > wine_ps5_hdr_decode_max_luminance( 255 ) ||
        luminance->max_full_frame_luminance > luminance->max_luminance ||
        luminance->min_luminance > luminance->max_luminance / 100.0f)
        return 0;

    hblank = (width + 4) / 5;
    if (hblank < 80) hblank = 80;
    vblank = 45;
    hsync_offset = hblank / 3;
    hsync_width = hblank / 6;
    pixel_clock = ((uint64_t)(width + hblank) * (height + vblank) * 60 + 5000) / 10000;
    if (hblank > 4095 || pixel_clock > 65535) return 0;

    max_code = wine_ps5_hdr_encode_max_luminance( luminance->max_luminance );
    max_full_frame_code = wine_ps5_hdr_encode_max_luminance( luminance->max_full_frame_luminance );
    encoded_max = wine_ps5_hdr_decode_max_luminance( max_code );
    min_code = wine_ps5_hdr_encode_min_luminance( luminance->min_luminance, encoded_max );

    memset( edid, 0, WINE_PS5_HDR_EDID_SIZE );
    memcpy( edid, header, sizeof(header) );
    edid[8] = 0x5e; edid[9] = 0x13;  /* WPS: synthetic Wine PS5 monitor. */
    edid[10] = 0x01;
    edid[16] = 1; edid[17] = 36;     /* Week 1, 2026. */
    edid[18] = 1; edid[19] = 4;
    edid[20] = 0xb2;                 /* Digital, 10 bits/component, HDMI-a. */
    edid[23] = 120;                  /* Gamma 2.20. */
    edid[24] = 0x02;                 /* First descriptor is the preferred timing. */
    edid[25] = 0x78; edid[26] = 0xb1;
    edid[27] = 0xb5; edid[28] = 0x4a; /* BT.2020 red. */
    edid[29] = 0x2b; edid[30] = 0xcc; /* BT.2020 green. */
    edid[31] = 0x21; edid[32] = 0x0b; /* BT.2020 blue. */
    edid[33] = 0x50; edid[34] = 0x54; /* D65 white point. */
    for (i = 38; i < 54; i++) edid[i] = 0x01;

    timing = edid + 54;
    timing[0] = pixel_clock;
    timing[1] = pixel_clock >> 8;
    timing[2] = width;
    timing[3] = hblank;
    timing[4] = ((width >> 8) << 4) | (hblank >> 8);
    timing[5] = height;
    timing[6] = vblank;
    timing[7] = ((height >> 8) << 4) | (vblank >> 8);
    timing[8] = hsync_offset;
    timing[9] = hsync_width;
    timing[10] = 0x35;               /* Vertical sync offset 3, width 5. */
    timing[11] = ((hsync_offset >> 8) << 6) | ((hsync_width >> 8) << 4);
    timing[17] = 0x1e;               /* Digital separate sync, positive H/V. */
    edid[75] = 0xfc;
    memcpy( edid + 77, name, sizeof(name) );
    edid[93] = edid[111] = 0x10;
    edid[126] = 1;
    wine_ps5_hdr_edid_checksum( edid );

    cta = edid + 128;
    cta[0] = 0x02; cta[1] = 0x03; cta[2] = 15; cta[3] = 0x80;
    cta[4] = 0xe3; cta[5] = 0x05; cta[6] = 0x80; cta[7] = 0x00;
    cta[8] = 0xe6; cta[9] = 0x06; cta[10] = 0x05; cta[11] = 0x01;
    cta[12] = max_code;
    cta[13] = max_full_frame_code;
    cta[14] = min_code;
    wine_ps5_hdr_edid_checksum( cta );
    return WINE_PS5_HDR_EDID_SIZE;
}

struct wine_ps5_title_spawn
{
    unsigned int size;          /* sizeof(struct wine_ps5_title_spawn) */
    unsigned int windows_pid;   /* new_process reply->pid */
    int server_socket;          /* callee sends a copy; caller closes its own */
    char **argv;                /* "wine", then the split Windows command line, NULL */
    const char *unix_cwd;       /* may be NULL */
};
struct wine_ps5_title_game_launch
{
    unsigned int size;
    char **argv;                /* decoded Windows command: executable first */
    const char *windows_cwd;    /* UTF-8 DOS cwd, may be NULL */
    const char *unix_cwd;       /* translated cwd, may be NULL */
};


struct wine_ps5_title_runtime
{
    /* Installed by Wine's loader: publish one terminal status and return to
     * ntdll, which acknowledges the last loader instruction before ending
     * the requester. Only the supervisor may invoke the SDK exit route. */
    void (*exit)( int );
    int (*create_thread)( pthread_t *, const pthread_attr_t *, void *(*)( void * ), void * );
    int (*join_thread)( pthread_t, void ** );
    int (*create_key)( pthread_key_t *, void (*)( void * ));
    void *(*get_specific)( pthread_key_t );
    int (*set_specific)( pthread_key_t, const void * );
    /* Native title's one RADV, never a second copy loaded by payload dlopen. */
    void *(*vulkan_proc)( const char * );
    int (*delete_key)( pthread_key_t );
    /* Owned by the native title through joined Wine/server shutdown. Wine
     * imports this fd as a real Windows stdout handle; native fd1 is untouched. */
    int stdout_fd;
    /* Parent role only (NULL in a child): the native supervisor. */
    int (*spawn_process)( const struct wine_ps5_title_spawn *spawn ); /* 0 or errno */
    int (*signal_child_thread)( int unix_pid, unsigned int thread_id, void *teb, int sig ); /* 0 or -1/errno */
    int (*terminate_child)( int unix_pid ); /* 0 or -1/errno */
    /* Child role only (-1 otherwise): connected socket of the parent's server. */
    int server_socket;
    /* B3 desktop (NULL when the title has no desktop, e.g. headless or G3):
     * a new connected SOCK_STREAM descriptor to the title's desktop hub,
     * or -1 with errno set. Each call is a new hub peer. */
    int (*desktop_connect)( void );
    /* Window surface shared memory, done by the title parent for every
     * process: op PS5_TITLE_SURFACE_CREATE creates name exclusively
     * (O_RDWR|O_CREAT|O_EXCL, mode 0666) sized to size bytes and returns its
     * fd; size includes both the draw and published planes (see ps5_desktop.h).
     * PS5_TITLE_SURFACE_OPEN opens an existing name O_RDWR and returns its fd
     * (size is the minimum total object size); PS5_TITLE_SURFACE_UNLINK
     * unlinks name and returns 0. -1 with errno on failure. name is a
     * "/wine-ps5-..." object name shorter than PS5_DESKTOP_SHM_NAME (64). */
    int (*desktop_surface)( const char *name, uint64_t size, int op );
    /* B2 (NULL when unsupported): start a Windows program in the payload Wine
     * domain on this title's prefix. cmdline is the UTF-8 Windows command line
     * (program first), cwd the UTF-8 Windows current directory or NULL.
     * Returns 0 once the payload loader was handed the request, else errno. */
    int (*payload_spawn)( const char *cmdline, const char *cwd );
    /* Title URL hand-off (NULL when unsupported): open an HTTP(S) UTF-8 URL
     * in the console browser without creating a Windows process.
     * Returns 0 on success, else errno. */
    int (*open_url)( const char *url );
    /* Parent-only maintenance capability. The callback copies the bounded
     * command synchronously and requests clean title shutdown for relaunch.
     * Returns 0 when accepted, otherwise errno. Argument values are private. */
    int (*game_launch)( const struct wine_ps5_title_game_launch *launch );
    /* Parent-only title audio. A PsmKit child leaves these NULL and the
     * Wine driver provides a clocked silent endpoint. output consumes one
     * 48-kHz signed-16 stereo grain and is allowed to block for that grain. */
    int (*audio_open)( unsigned int rate, unsigned int grain );
    int (*audio_output)( const int16_t *frames );
    void (*audio_close)( void );
    /* Parent GPU role only. Copies the native title's startup-time calibrated
     * HDR luminance; returns zero on success. */
    int (*get_hdr_luminance)( struct wine_ps5_hdr_luminance *luminance );
    unsigned int version;
};

static inline int wine_ps5_title_stdout_valid( const struct wine_ps5_title_runtime *runtime )
{
    int flags;
    if (!runtime || runtime->version != WINE_PS5_TITLE_RUNTIME_VERSION || runtime->stdout_fd < 0)
        return 0;
    flags = fcntl( runtime->stdout_fd, F_GETFL );
    return flags >= 0 && (flags & O_ACCMODE) != O_RDONLY;
}

static inline int wine_ps5_title_stdout_fd( const struct wine_ps5_title_runtime *runtime )
{
    return runtime ? runtime->stdout_fd : 1;
}

/* True only after the loader installed the validated native runtime. */
extern int __wine_ps5_is_title(void);
extern int __wine_ps5_get_stdout_fd(void);
extern int __wine_ps5_desktop_available(void);
extern int __wine_ps5_desktop_connect(void);
extern int __wine_ps5_desktop_surface( const char *name, uint64_t size, int op );
extern void *__wine_ps5_get_vulkan_proc( const char *name );
extern int __wine_ps5_get_hdr_luminance( struct wine_ps5_hdr_luminance *luminance );
extern int __wine_ps5_audio_available(void);
extern int __wine_ps5_audio_open( unsigned int rate, unsigned int grain );
extern int __wine_ps5_audio_output( const int16_t *frames );
extern void __wine_ps5_audio_close(void);
/* Publish a native blocking scope and return its previous state for restoration. */
extern int ps5_title_profile_set_blocking( int blocking );
extern int __wine_ps5_key_create( pthread_key_t *key, void (*destructor)( void * ));
extern int __wine_ps5_key_delete( pthread_key_t key );
extern void *__wine_ps5_get_specific( pthread_key_t key );
extern int __wine_ps5_set_specific( pthread_key_t key, const void *value );
extern int __wine_ps5_signal_thread( unsigned int thread_id, void *teb, int sig );
#endif

/* A successful DLL load is insufficient if the Unix driver's ABI check
 * rejected its registration. Shared with the portable startup regressions. */
static inline int wine_ps5_title_driver_loaded( unsigned int status, int installed )
{
    return !status && installed;
}

#endif /* __WINE_PS5_TITLE_H */
