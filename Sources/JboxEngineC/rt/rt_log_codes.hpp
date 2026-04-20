// rt_log_codes.hpp — numeric event codes for RtLogQueue records.
//
// RT callers push RtLogEvent records tagged with one of these codes;
// the drainer translates the code into a human-readable os_log line.
// The code set is a public contract between producers and the drainer,
// but it is an internal engine contract (not part of the C ABI).
//
// Keeping this header plain-constants means it is safe to include from
// RT code: the rt_safety_scan will see only integer literals.

#ifndef JBOX_RT_RT_LOG_CODES_HPP
#define JBOX_RT_RT_LOG_CODES_HPP

#include "rt_log_queue.hpp"

namespace jbox::rt {

// NOTE: values are append-only. Never reuse or renumber an existing
// value; drainers in older binaries may parse newer codes as "unknown".
enum : RtLogCode {
    kLogNone                 = 0,
    kLogUnderrun             = 1,   // value_a = frames_consumed total
    kLogOverrun              = 2,   // value_a = frames_produced total
    kLogChannelMismatch      = 3,   // value_a = expected channels, value_b = got channels
    kLogConverterShort       = 4,   // value_a = frames_requested, value_b = frames_produced

    // Control-thread-originated, queued instead of direct os_log for
    // ordering (so a state change logs in line with the RT events that
    // preceded it). Drainer formats them the same way.
    kLogRouteStarted         = 100, // value_a = source_channel_count, value_b = dest_channel_count
    kLogRouteStopped         = 101,
    kLogRouteWaiting         = 102, // value_a = 1 if source missing, value_b = 1 if dest missing
    kLogRouteError           = 103, // value_a = jbox_error_code_t
};

}  // namespace jbox::rt

#endif  // JBOX_RT_RT_LOG_CODES_HPP
