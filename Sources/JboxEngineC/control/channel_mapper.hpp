// channel_mapper.hpp — validation of a route's source→destination
// channel mapping against the v1 invariants.
//
// This code is called from the control thread when a route is being
// started or edited. It is NOT called from the audio thread and so
// may allocate and use containers freely.
//
// v1 invariants enforced here:
//   1. The edge list is non-empty.
//   2. Every channel index is non-negative.
//   3. Each source channel appears at most once.
//   4. Each destination channel appears at most once.
//
// Device-level bounds (is channel N present on device X?) are the
// responsibility of the caller; ChannelMapper only enforces invariants
// that depend on the edge list alone. Future versions that permit
// fan-out or fan-in will relax rules (3) and (4) respectively; the
// ChannelEdge struct itself does not change.
//
// See docs/spec.md §§ 3.1.2, 3.1.3.

#ifndef JBOX_CONTROL_CHANNEL_MAPPER_HPP
#define JBOX_CONTROL_CHANNEL_MAPPER_HPP

#include <cstddef>
#include <span>

namespace jbox::control {

struct ChannelEdge {
    int src;
    int dst;

    friend bool operator==(const ChannelEdge&, const ChannelEdge&) = default;
};

enum class ChannelMapperError {
    kOk = 0,
    kEmpty,                 // edge list was empty
    kNegativeChannel,       // some edge had a negative src or dst
    kDuplicateSource,       // same src channel appeared more than once
    kDuplicateDestination,  // same dst channel appeared more than once
};

// Returns a human-readable name for the error code. Safe to call at
// any time; returns a string literal (no allocation).
const char* channelMapperErrorName(ChannelMapperError error) noexcept;

// Validate an edge list. Returns kOk on success, or the first
// violation encountered. Validation order follows the invariant
// numbering above (empty → negative → duplicate-src → duplicate-dst).
ChannelMapperError validate(std::span<const ChannelEdge> edges);

}  // namespace jbox::control

#endif  // JBOX_CONTROL_CHANNEL_MAPPER_HPP
