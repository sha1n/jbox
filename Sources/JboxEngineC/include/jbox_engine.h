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

/* ABI history:
 *   1  initial Phase 3 contract.
 *   2  MINOR — appended `estimated_latency_us` to jbox_route_status_t.
 *   3  MINOR — appended `low_latency` to jbox_route_config_t.
 *   4  MINOR — added jbox_route_latency_components_t and
 *              jbox_engine_poll_route_latency_components.
 *   5  MINOR — `low_latency` renamed to `latency_mode`; the field
 *              now carries a tier (0=off, 1=low, 2=performance).
 *              Zero-initialised callers keep the safe default.
 *   6  MINOR — appended `buffer_frames` to jbox_route_config_t (0
 *              means use the tier default; non-zero overrides the
 *              target HAL buffer size the fast path requests).
 *   7  MINOR — added jbox_engine_rename_route for non-disruptive
 *              renames of existing routes.
 *   8  MINOR — added jbox_engine_set_resampler_quality /
 *              jbox_engine_resampler_quality so the Swift
 *              Preferences window can push an engine-wide SRC
 *              quality preset. Applies to newly-started routes.
 *   9  MINOR — appended `share_device` to jbox_route_config_t and
 *              `status_flags` to jbox_route_status_t; introduced
 *              JBOX_ROUTE_STATUS_SHARE_DOWNGRADE for the Performance →
 *              Low demotion surface (spec § 2.7 "Device sharing").
 */
#define JBOX_ENGINE_ABI_VERSION 9u

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
 * Mapping invariants (see docs/spec.md § 3.1.3): non-empty; no
 * duplicate dst channel. Duplicate src is permitted — it produces
 * fan-out, replicating the source sample into every mapped
 * destination slot.
 *
 * `latency_mode` (ABI v5+) selects one of three ring-buffer and
 * drift-setpoint presets (see docs/spec.md § 2.3):
 *   0 — Off (default). Safe 8× / 4096-floor ring; target fill ring/2.
 *   1 — Low. 3× / 512-floor ring; target fill ring/2. Bursty-USB risk.
 *   2 — Performance. 2× / 256-floor ring; target fill ring/4. Lowest
 *       latency; high underrun risk on bursty sources.
 * Zero-initialised callers keep the safe default. ABI v3 and v4
 * used the name `low_latency` for values 0 and 1; the field's
 * storage is unchanged.
 *
 * `buffer_frames` (ABI v6+) overrides the HAL buffer-frame-size
 * target the Performance-mode direct-monitor fast path asks the
 * backend for. 0 means "use the tier default" (currently 64 frames
 * for Performance; Off / Low do not touch the buffer regardless).
 * Non-zero values are clamped by the HAL into the device's
 * supported range (`supportedBufferFrameSizeRange`) — callers
 * typically surface that range in their UI so users only choose
 * values the device can honour.
 *
 * `share_device` (ABI v9+) opts the route out of Jbox's default
 * hog-mode policy: when non-zero, the engine will not call
 * `claimExclusive` on the route's device(s), and the route runs on
 * Core Audio's shared-client path at whatever HAL buffer size the
 * device happens to have. Performance-tier routes flagged
 * `share_device = 1` are silently demoted to Low latency (the fast
 * path needs exclusivity; the ring/4 setpoint can't be defended
 * without it) and surface the `JBOX_ROUTE_STATUS_SHARE_DOWNGRADE`
 * bit via `jbox_engine_poll_route_status`. Zero-initialised callers
 * keep today's exclusive behaviour.
 */
typedef struct {
    const char*                source_uid;
    const char*                dest_uid;
    const jbox_channel_edge_t* mapping;
    size_t                     mapping_count;
    const char*                name;
    uint32_t                   latency_mode;
    uint32_t                   buffer_frames;
    uint8_t                    share_device;
} jbox_route_config_t;

typedef enum {
    JBOX_ROUTE_STATE_STOPPED  = 0,
    JBOX_ROUTE_STATE_WAITING  = 1,
    JBOX_ROUTE_STATE_STARTING = 2,
    JBOX_ROUTE_STATE_RUNNING  = 3,
    JBOX_ROUTE_STATE_ERROR    = 4
} jbox_route_state_t;

const char* jbox_route_state_name(jbox_route_state_t state);

/* Bitfield constants for `jbox_route_status_t::status_flags` (ABI v9+). */
#define JBOX_ROUTE_STATUS_SHARE_DOWNGRADE 0x00000001u

/* Snapshot of a route's runtime state. Filled in by poll_route_status.
 *
 * Field additions are ABI-MINOR per the header comment above. Existing
 * callers that zero-initialise the struct stay compatible. */
typedef struct {
    jbox_route_state_t state;
    jbox_error_code_t  last_error;    /* JBOX_OK unless state == ERROR */
    uint64_t           frames_produced;
    uint64_t           frames_consumed;
    uint64_t           underrun_count;
    uint64_t           overrun_count;
    /* Added in ABI v2: end-to-end estimate, computed once at startRoute
     * per docs/spec.md § 2.12. 0 for non-running routes or when the
     * sample rate is unknown. Not updated after the route starts; stop +
     * start refreshes the value. */
    uint64_t           estimated_latency_us;
    /* Added in ABI v9: bitmap of per-route status flags. Currently only
     * JBOX_ROUTE_STATUS_SHARE_DOWNGRADE is defined — set when a
     * Performance-tier route was silently demoted because its
     * `share_device` flag is set. Zero for non-running routes. */
    uint32_t           status_flags;
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

/*
 * Rename a route (ABI v7+). Non-disruptive: safe in any state, and a
 * running route keeps flowing audio uninterrupted. The engine copies
 * the string. Passing NULL clears the stored name (equivalent to an
 * empty string). Returns JBOX_ERR_INVALID_ARGUMENT for NULL engine or
 * unknown `route_id`.
 *
 * Note: the engine does not currently surface the stored name back
 * through the C ABI — callers own the authoritative user-visible
 * name. This entry exists so that future engine-side logging can
 * identify routes by user-chosen names, and so that a persistence
 * layer can push names through at restore time.
 */
jbox_error_code_t jbox_engine_rename_route(jbox_engine_t*  engine,
                                           jbox_route_id_t route_id,
                                           const char*     new_name);

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

/*
 * Per-route latency component breakdown (ABI v4+).
 *
 * All frame counts are expressed at the sample rate of the side they
 * belong to: the `src_*` fields are in frames at `src_sample_rate_hz`,
 * the `dst_*` fields and `converter_prime_frames` are at
 * `dst_sample_rate_hz`. `ring_target_fill_frames` is counted at the
 * source rate (the ring is written by the source IOProc).
 *
 * `total_us` matches the value surfaced through
 * `jbox_route_status_t::estimated_latency_us` — exposed here for
 * callers that only need one call to render the diagnostics view.
 *
 * All fields are 0 when the route is not currently running.
 */
typedef struct {
    uint32_t src_hal_latency_frames;
    uint32_t src_safety_offset_frames;
    uint32_t src_buffer_frames;
    uint32_t ring_target_fill_frames;
    uint32_t converter_prime_frames;
    uint32_t dst_buffer_frames;
    uint32_t dst_safety_offset_frames;
    uint32_t dst_hal_latency_frames;
    double   src_sample_rate_hz;
    double   dst_sample_rate_hz;
    uint64_t total_us;
} jbox_route_latency_components_t;

/* Fill in the latency component breakdown for the given route. */
jbox_error_code_t jbox_engine_poll_route_latency_components(
    jbox_engine_t*                   engine,
    jbox_route_id_t                  route_id,
    jbox_route_latency_components_t* out_components);

/* -------------------------------------------------------------------- */
/*  Resampler quality (engine-wide)                                     */
/* -------------------------------------------------------------------- */

/*
 * Engine-wide sample-rate-conversion quality preset (ABI v8+). Applied
 * when a new route's converter is constructed at `startRoute` — routes
 * already running keep the preset their converter was built with until
 * stopped and started again.
 *
 *   0 — Mastering. `_Complexity_Mastering` + `Quality_Max`. Default.
 *       Highest-fidelity preset; what the engine has used since v1.
 *   1 — High Quality. `_Complexity_Normal` + `Quality_High`. Still
 *       well above the Core Audio default; noticeably cheaper on
 *       high-channel-count / multi-route sessions.
 *
 * Unknown values are clamped to Mastering. See docs/spec.md § 2.5 and
 * § 4.6.
 */
typedef enum {
    JBOX_RESAMPLER_QUALITY_MASTERING    = 0,
    JBOX_RESAMPLER_QUALITY_HIGH_QUALITY = 1
} jbox_resampler_quality_t;

/* Set the engine-wide resampler quality preset. Non-NULL engine
 * required; otherwise returns JBOX_ERR_INVALID_ARGUMENT. Unknown
 * values are clamped to Mastering without error. */
jbox_error_code_t jbox_engine_set_resampler_quality(
    jbox_engine_t*           engine,
    jbox_resampler_quality_t quality);

/* Read the current engine-wide resampler quality preset. Returns
 * JBOX_RESAMPLER_QUALITY_MASTERING on NULL engine (the default). */
jbox_resampler_quality_t jbox_engine_resampler_quality(
    jbox_engine_t* engine);

/*
 * Supported HAL buffer-frame-size range for the device identified by
 * `uid` (ABI v6+). Both `out_min` and `out_max` are filled with 0
 * when the device is unknown or the HAL does not expose the property.
 * For an aggregate device the reported range is the intersection of
 * every active sub-device's range, so every value the UI surfaces is
 * accepted by every member simultaneously.
 */
jbox_error_code_t jbox_engine_supported_buffer_frame_size_range(
    jbox_engine_t* engine,
    const char*    uid,
    uint32_t*      out_min,
    uint32_t*      out_max);

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
