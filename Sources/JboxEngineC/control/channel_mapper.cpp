// channel_mapper.cpp — edge-list validation (control thread).

#include "channel_mapper.hpp"

#include <unordered_set>

namespace jbox::control {

const char* channelMapperErrorName(ChannelMapperError error) noexcept {
    switch (error) {
        case ChannelMapperError::kOk:                   return "ok";
        case ChannelMapperError::kEmpty:                return "empty edge list";
        case ChannelMapperError::kNegativeChannel:      return "negative channel index";
        case ChannelMapperError::kDuplicateDestination: return "duplicate destination channel";
    }
    return "unknown";
}

ChannelMapperError validate(std::span<const ChannelEdge> edges) {
    if (edges.empty()) {
        return ChannelMapperError::kEmpty;
    }

    // Only destination channels must be unique — fan-out (repeated
    // source) is permitted (see channel_mapper.hpp).
    std::unordered_set<int> dst_seen;
    dst_seen.reserve(edges.size());

    for (const auto& edge : edges) {
        if (edge.src < 0 || edge.dst < 0) {
            return ChannelMapperError::kNegativeChannel;
        }
        if (!dst_seen.insert(edge.dst).second) {
            return ChannelMapperError::kDuplicateDestination;
        }
    }

    return ChannelMapperError::kOk;
}

}  // namespace jbox::control
