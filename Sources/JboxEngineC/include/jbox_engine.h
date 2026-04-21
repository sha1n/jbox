/*
 * jbox_engine.h — public C API for the Jbox audio engine.
 *
 * This header is the stable public contract between the engine and any
 * client (Swift UI layer, CLI, future alternative UIs). Changes here are
 * semver-governed:
 *   - Adding functions, enum values, or struct fields (appended) is MINOR.
 *   - Removing or renaming symbols, reordering fields, changing behavior
 *     is MAJOR.
 *
 * All functions are safe to call from any non-RT thread. No function
 * throws (the C ABI has no exceptions). Errors are reported either via
 * a returned error code, or via a jbox_error_t out-parameter for
 * functions whose natural return value is a handle.
 *
 * See docs/spec.md §§ 1.6, 2.11.
 */

#ifndef JBOX_ENGINE_H
#define JBOX_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- */
/*  ABI versioning                                                      */
/* -------------------------------------------------------------------- */

#define JBOX_ENGINE_ABI_VERSION 1u

uint32_t jbox_engine_abi_version(void);

/* -------------------------------------------------------------------- */
/*  Errors                                                              */
/* -------------------------------------------------------------------- */

typedef enum {
    JBOX_OK                      = 0,
    JBOX_ERR_INVALID_ARGUMENT    = 1,   /* caller passed nullptr or bogus value */
    JBOX_ERR_DEVICE_NOT_FOUND    = 2,   /* referenced device UID not present    */
    JBOX_ERR_MAPPING_INVALID     = 3,   /* channel mapping rejected             */
    JBOX_ERR_RESOURCE_EXHAUSTED  = 4,   /* engine-internal limit hit            */
    JBOX_ERR_DEVICE_BUSY         = 5,   /* device unavailable for the requested direction */
    JBOX_ERR_NOT_IMPLEMENTED     = 6,   /* feature stubbed; lands in a later commit      */
    JBOX_ERR_INTERNAL            = 7    /* uncategorised engine failure         */
} jbox_error_code_t;

/*
 * Error details. `message` points to storage owned by the engine,
 * valid until the next engine call on the same thread (callers should
 * copy if they need it later). NULL-terminated. May be NULL even when
 * `code` is non-zero.
 */
typedef struct {
    jbox_error_code_t code;
    const char*       message;
} jbox_error_t;

/* Human-readable name for the error code; always returns a non-NULL
 * static string. */
const char* jbox_error_code_name(jbox_error_code_t code);

/* -------------------------------------------------------------------- */
/*  Devices                                                             */
/* -------------------------------------------------------------------- */

typedef enum {
    JBOX_DEVICE_DIRECTION_NONE   = 0,
    JBOX_DEVICE_DIRECTION_INPUT  = 1u << 0,
    JBOX_DEVICE_DIRECTION_OUTPUT = 1u << 1
} jbox_device_direction_t;

/* Upper bounds chosen to comfortably hold Core Audio UIDs and names.
 * Both are NUL-terminated; excess is truncated. */
#define JBOX_UID_MAX_LEN  256
#define JBOX_NAME_MAX_LEN 256

typedef struct {
    char     uid[JBOX_UID_MAX_LEN];
    char     name[JBOX_NAME_MAX_LEN];
    uint32_t direction;              /* bitmask of jbox_device_direction_t */
    uint32_t input_channel_count;
    uint32_t output_channel_count;
    double   nominal_sample_rate;
    uint32_t buffer_frame_size;
} jbox_device_info_t;

/*
 * A heap-allocated device enumeration snapshot. `devices` is an array
 * of `count` elements (NULL iff count==0). Caller releases via
 * jbox_device_list_free.
 */
typedef struct {
    jbox_device_info_t* devices;
    size_t              count;
} jbox_device_list_t;

/* Release a list returned by jbox_engine_enumerate_devices. Safe with NULL. */
void jbox_device_list_free(jbox_device_list_t* list);

/*
 * Per-channel label for a device in a given direction. Some Core Audio
 * devices (UA Apollo, MOTU, etc.) publish meaningful names like
 * "Monitor L" or "Virtual 3"; simpler devices publish nothing and the
 * name field is an empty string. Callers should fall back to a numeric
 * label ("Ch N") when name[0] == '\0'.
 *
 * Channel count equals the corresponding input_channel_count /
 * output_channel_count from jbox_device_info_t; index 0 corresponds
 * to channel 1 in UI 1-indexed terms.
 */
typedef struct {
    char name[JBOX_NAME_MAX_LEN];
} jbox_channel_info_t;

typedef struct {
    jbox_channel_info_t* channels;
    size_t               count;
} jbox_channel_list_t;

/* Release a list returned by jbox_engine_enumerate_device_channels. Safe with NULL. */
void jbox_channel_list_free(jbox_channel_list_t* list);

/* -------------------------------------------------------------------- */
/*  Routes                                                              */
/* -------------------------------------------------------------------- */

/* Route identifier. 0 == invalid. IDs are unique for the lifetime of
 * the engine instance; removing a route does not recycle its id. */
typedef uint32_t jbox_route_id_t;
#define JBOX_INVALID_ROUTE_ID ((jbox_route_id_t)0u)

/* One edge in a route's channel mapping. Channels are 0-indexed. */
typedef struct {
    uint32_t src;
    uint32_t dst;
} jbox_channel_edge_t;

/*
 * Route creation request. Caller-owned; engine copies the referenced
 * strings and mapping array internally. `name` may be NULL.
 *
 * v1 mapping invariants: non-empty; no duplicate src channel; no
 * duplicate dst channel. (See docs/spec.md § 3.1.)
 */
typedef struct {
    const char*                source_uid;
    const char*                dest_uid;
    const jbox_channel_edge_t* mapping;
    size_t                     mapping_count;
    const char*                name;
} jbox_route_config_t;

typedef enum {
    JBOX_ROUTE_STATE_STOPPED  = 0,
    JBOX_ROUTE_STATE_WAITING  = 1,
    JBOX_ROUTE_STATE_STARTING = 2,
    JBOX_ROUTE_STATE_RUNNING  = 3,
    JBOX_ROUTE_STATE_ERROR    = 4
} jbox_route_state_t;

const char* jbox_route_state_name(jbox_route_state_t state);

/* Snapshot of a route's runtime state. Filled in by poll_route_status. */
typedef struct {
    jbox_route_state_t state;
    jbox_error_code_t  last_error;    /* JBOX_OK unless state == ERROR */
    uint64_t           frames_produced;
    uint64_t           frames_consumed;
    uint64_t           underrun_count;
    uint64_t           overrun_count;
} jbox_route_status_t;

/* -------------------------------------------------------------------- */
/*  Engine lifecycle                                                    */
/* -------------------------------------------------------------------- */

/*
 * Engine creation options. Zero-initialise before populating; unknown
 * fields are reserved for future use and must be zero for forward
 * compatibility.
 */
typedef struct {
    uint32_t reserved;
} jbox_engine_config_t;

/* Opaque engine instance. */
typedef struct jbox_engine jbox_engine_t;

/* Create a new engine. Returns NULL on failure (and populates *err if
 * err != NULL). Caller owns the returned handle and must release via
 * jbox_engine_destroy. */
jbox_engine_t* jbox_engine_create(const jbox_engine_config_t* config,
                                  jbox_error_t*               err);

/* Destroy an engine, stopping any running routes. Safe with NULL. */
void jbox_engine_destroy(jbox_engine_t* engine);

/* -------------------------------------------------------------------- */
/*  Engine operations                                                   */
/* -------------------------------------------------------------------- */

/* Snapshot of currently-visible audio devices. Returns NULL on
 * failure. Caller releases the list via jbox_device_list_free. */
jbox_device_list_t* jbox_engine_enumerate_devices(jbox_engine_t* engine,
                                                  jbox_error_t*  err);

/*
 * Per-channel labels for `uid` in `direction`. `direction` must be
 * exactly JBOX_DEVICE_DIRECTION_INPUT or JBOX_DEVICE_DIRECTION_OUTPUT.
 * Returns a list whose `count` equals the device's channel count in
 * that direction (possibly zero). Entries whose driver did not publish
 * a name have an empty `name` string. Returns NULL on failure (e.g.,
 * invalid direction, out of memory); *err is populated when non-NULL.
 * Caller releases the list via jbox_channel_list_free.
 */
jbox_channel_list_t* jbox_engine_enumerate_device_channels(
    jbox_engine_t*          engine,
    const char*             uid,
    jbox_device_direction_t direction,
    jbox_error_t*           err);

/* Add a route. Returns the new route id (>= 1) on success, or
 * JBOX_INVALID_ROUTE_ID on failure (with *err populated). The route
 * is in state STOPPED on return. */
jbox_route_id_t jbox_engine_add_route(jbox_engine_t*             engine,
                                      const jbox_route_config_t* config,
                                      jbox_error_t*              err);

/* Remove a route. If the route is running it is stopped first. */
jbox_error_code_t jbox_engine_remove_route(jbox_engine_t*  engine,
                                           jbox_route_id_t route_id);

/* Request that the route start audio flow. Transitions to WAITING if
 * either referenced device is not currently present; otherwise to
 * STARTING and (quickly) RUNNING. Safe to call on an already-running
 * route (no-op). */
jbox_error_code_t jbox_engine_start_route(jbox_engine_t*  engine,
                                          jbox_route_id_t route_id);

/* Stop a route, transitioning it to STOPPED. Safe to call on a
 * stopped route (no-op). */
jbox_error_code_t jbox_engine_stop_route(jbox_engine_t*  engine,
                                         jbox_route_id_t route_id);

/* Fill in a status snapshot for the given route. */
jbox_error_code_t jbox_engine_poll_route_status(jbox_engine_t*       engine,
                                                jbox_route_id_t      route_id,
                                                jbox_route_status_t* out_status);

/* -------------------------------------------------------------------- */
/*  Metering                                                            */
/* -------------------------------------------------------------------- */

/*
 * Which side of a route to meter. Source = pre-ring-buffer peaks of
 * the mapped source channels; dest = post-converter peaks of the
 * mapped destination channels. Indexed in route-internal order (0..N-1
 * where N is the route's mapping count), not device channel indices.
 */
typedef enum {
    JBOX_METER_SIDE_SOURCE = 0,
    JBOX_METER_SIDE_DEST   = 1
} jbox_meter_side_t;

/*
 * Drain peak meters for a route. Fills up to `max_channels` linear
 * peak-amplitude values (|sample|, range [0.0, 1.0] for well-formed
 * audio) into `out_peaks`. Returns the number of values written.
 *
 * Read-and-reset semantics: each call returns the peak since the
 * previous call and atomically resets the stored peak to zero.
 *
 * Returns 0 (without touching `out_peaks`) when:
 *   - `engine` or `out_peaks` is NULL, or `max_channels` is 0
 *   - `route_id` is unknown
 *   - the route is not RUNNING
 *   - `side` is neither JBOX_METER_SIDE_SOURCE nor JBOX_METER_SIDE_DEST
 */
size_t jbox_engine_poll_meters(jbox_engine_t*    engine,
                               jbox_route_id_t   route_id,
                               jbox_meter_side_t side,
                               float*            out_peaks,
                               size_t            max_channels);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* JBOX_ENGINE_H */
