// channel_mapper.cpp — edge-list validation (control thread).

#include "channel_mapper.hpp"

#include <unordered_set>

namespace jbox::control {

const char* channelMapperErrorName(ChannelMapperError error) noexcept {
    switch (error) {
        case ChannelMapperError::kOk:                   return "ok";
        case ChannelMapperError::kEmpty:                return "empty edge list";
        case ChannelMapperError::kNegativeChannel:      return "negative channel index";
        case ChannelMapperError::kDuplicateSource:      return "duplicate source channel";
        case ChannelMapperError::kDuplicateDestination: return "duplicate destination channel";
    }
    return "unknown";
}

ChannelMapperError validate(std::span<const ChannelEdge> edges) {
    if (edges.empty()) {
        return ChannelMapperError::kEmpty;
    }

    // Reserve ahead of insertions; expected set size equals the edge
    // count (one src and one dst per edge under v1 rules).
    std::unordered_set<int> src_seen;
    std::unordered_set<int> dst_seen;
    src_seen.reserve(edges.size());
    dst_seen.reserve(edges.size());

    for (const auto& edge : edges) {
        if (edge.src < 0 || edge.dst < 0) {
            return ChannelMapperError::kNegativeChannel;
        }
        if (!src_seen.insert(edge.src).second) {
            return ChannelMapperError::kDuplicateSource;
        }
        if (!dst_seen.insert(edge.dst).second) {
            return ChannelMapperError::kDuplicateDestination;
        }
    }

    return ChannelMapperError::kOk;
}

}  // namespace jbox::control
