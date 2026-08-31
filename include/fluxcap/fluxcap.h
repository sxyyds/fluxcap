#ifndef FLUXCAP_FLUXCAP_H
#define FLUXCAP_FLUXCAP_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(FLUXCAP_SHARED)
#  if defined(FLUXCAP_EXPORTS)
#    define FLUXCAP_API __declspec(dllexport)
#  else
#    define FLUXCAP_API __declspec(dllimport)
#  endif
#else
#  define FLUXCAP_API
#endif

#if defined(_WIN32)
#  define FLUXCAP_CALL __cdecl
#else
#  define FLUXCAP_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FLUXCAP_ABI_VERSION 1u
#define FLUXCAP_WAIT_INFINITE 0xffffffffu

typedef struct fluxcap_session fluxcap_session;

typedef int32_t fluxcap_status;
#define FLUXCAP_STATUS_OK ((fluxcap_status)0)
#define FLUXCAP_STATUS_INVALID_ARGUMENT ((fluxcap_status)1)
#define FLUXCAP_STATUS_OUT_OF_MEMORY ((fluxcap_status)2)
#define FLUXCAP_STATUS_SYSTEM_ERROR ((fluxcap_status)3)
#define FLUXCAP_STATUS_INVALID_STATE ((fluxcap_status)4)
#define FLUXCAP_STATUS_TIMEOUT ((fluxcap_status)5)
#define FLUXCAP_STATUS_NO_BUFFER ((fluxcap_status)6)
#define FLUXCAP_STATUS_NOT_SUPPORTED ((fluxcap_status)7)

typedef uint32_t fluxcap_pixel_format;
#define FLUXCAP_PIXEL_FORMAT_BGRX8 ((fluxcap_pixel_format)1u)

typedef uint32_t fluxcap_config_flags;
#define FLUXCAP_FLAG_INCLUDE_LAYERED_WINDOWS ((fluxcap_config_flags)(1u << 0))
#define FLUXCAP_FLAG_INCLUDE_CURSOR ((fluxcap_config_flags)(1u << 1))
#define FLUXCAP_FLAG_DETECT_DIRTY_REGIONS ((fluxcap_config_flags)(1u << 2))
#define FLUXCAP_FLAG_HIGH_PRIORITY_THREAD ((fluxcap_config_flags)(1u << 3))

typedef uint32_t fluxcap_display_flags;
#define FLUXCAP_DISPLAY_PRIMARY ((fluxcap_display_flags)(1u << 0))

#pragma pack(push, 8)

typedef struct fluxcap_rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} fluxcap_rect;

typedef struct fluxcap_config {
    uint32_t struct_size;
    uint32_t abi_version;
    /* width == height == 0 selects the complete virtual desktop. */
    fluxcap_rect region;
    uint32_t buffer_count;
    uint32_t tile_size;
    /* Zero captures as quickly as the backend can produce CPU-readable frames. */
    uint32_t target_fps;
    uint32_t flags;
} fluxcap_config;

typedef struct fluxcap_display {
    uint32_t struct_size;
    char device_name[128];
    fluxcap_rect bounds;
    fluxcap_rect work_area;
    uint32_t refresh_hz;
    uint32_t flags;
} fluxcap_display;

typedef struct fluxcap_frame {
    const uint8_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    fluxcap_pixel_format format;
    fluxcap_rect desktop_region;
    const fluxcap_rect* dirty_regions;
    uint32_t dirty_region_count;
    uint64_t sequence;
    uint64_t timestamp_qpc;
    uint64_t qpc_frequency;
    uint64_t capture_duration_ns;
    /*
     * dirty_regions transform this exact base sequence into this frame.
     * Zero means the dirty list is a full-frame update valid for any base.
     */
    uint64_t dirty_base_sequence;

    /* Private lease fields. Do not read or modify them. */
    void* _internal_owner;
    uint64_t _internal_token;
    uint32_t _internal_slot;
    uint32_t _internal_reserved;
} fluxcap_frame;

typedef struct fluxcap_stats {
    uint64_t captured_frames;
    uint64_t overwritten_frames;
    /* Capture attempts skipped because every writable slot was unavailable. */
    uint64_t no_buffer_skips;
    uint64_t capture_failures;
    uint64_t total_capture_ns;
    uint64_t last_capture_ns;
} fluxcap_stats;

#pragma pack(pop)

FLUXCAP_API fluxcap_config FLUXCAP_CALL fluxcap_config_default(void);

/*
 * Call with displays == NULL to query the current display count. Before the
 * data call, initialize struct_size in every element supplied by the caller.
 */
FLUXCAP_API fluxcap_status FLUXCAP_CALL fluxcap_enumerate_displays(
    fluxcap_display* displays,
    uint32_t capacity,
    uint32_t* display_count);

FLUXCAP_API fluxcap_status FLUXCAP_CALL fluxcap_create(
    const fluxcap_config* config,
    fluxcap_session** out_session);
/* All C frame leases owned by session must be released before destroy. */
FLUXCAP_API void FLUXCAP_CALL fluxcap_destroy(fluxcap_session* session);

/* Synchronous capture. It is invalid while the asynchronous worker is running. */
/* out_frame must be zero-initialized and must not contain an active lease. */
FLUXCAP_API fluxcap_status FLUXCAP_CALL fluxcap_capture(
    fluxcap_session* session,
    fluxcap_frame* out_frame);

FLUXCAP_API fluxcap_status FLUXCAP_CALL fluxcap_start(fluxcap_session* session);
FLUXCAP_API fluxcap_status FLUXCAP_CALL fluxcap_stop(fluxcap_session* session);

/* Acquires the newest published frame and discards older unleased frames. */
/* out_frame must be zero-initialized and must not contain an active lease. */
FLUXCAP_API fluxcap_status FLUXCAP_CALL fluxcap_acquire_latest(
    fluxcap_session* session,
    uint32_t timeout_ms,
    fluxcap_frame* out_frame);

/* Copying fluxcap_frame does not duplicate its lease; release exactly once. */
FLUXCAP_API fluxcap_status FLUXCAP_CALL fluxcap_release_frame(fluxcap_frame* frame);
FLUXCAP_API fluxcap_status FLUXCAP_CALL fluxcap_get_stats(
    const fluxcap_session* session,
    fluxcap_stats* out_stats);

FLUXCAP_API const char* FLUXCAP_CALL fluxcap_status_string(fluxcap_status status);
FLUXCAP_API const char* FLUXCAP_CALL fluxcap_last_error(void);
FLUXCAP_API const char* FLUXCAP_CALL fluxcap_version_string(void);

#ifdef __cplusplus
}
#endif

#endif
