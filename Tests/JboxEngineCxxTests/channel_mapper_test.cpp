// channel_mapper_test.cpp — unit tests for ChannelMapper validation.

#include <catch_amalgamated.hpp>

#include "channel_mapper.hpp"

#include <array>
#include <string_view>
#include <vector>

using jbox::control::ChannelEdge;
using jbox::control::ChannelMapperError;
using jbox::control::channelMapperErrorName;
using jbox::control::validate;

TEST_CASE("ChannelMapper: empty edge list is rejected", "[channel_mapper]") {
    std::vector<ChannelEdge> edges;
    REQUIRE(validate(edges) == ChannelMapperError::kEmpty);
}

TEST_CASE("ChannelMapper: single valid edge passes", "[channel_mapper]") {
    std::vector<ChannelEdge> edges{{0, 0}};
    REQUIRE(validate(edges) == ChannelMapperError::kOk);
}

TEST_CASE("ChannelMapper: stereo pair passes", "[channel_mapper]") {
    std::vector<ChannelEdge> edges{{0, 4}, {1, 5}};
    REQUIRE(validate(edges) == ChannelMapperError::kOk);
}

TEST_CASE("ChannelMapper: non-contiguous mapping passes", "[channel_mapper]") {
    // v1 supports arbitrary 1:1 mapping — edges don't have to be
    // contiguous ranges. See docs/spec.md § 3.1.
    std::vector<ChannelEdge> edges{{2, 11}, {7, 3}, {4, 15}};
    REQUIRE(validate(edges) == ChannelMapperError::kOk);
}

TEST_CASE("ChannelMapper: negative source channel is rejected", "[channel_mapper]") {
    std::vector<ChannelEdge> edges{{-1, 0}};
    REQUIRE(validate(edges) == ChannelMapperError::kNegativeChannel);
}

TEST_CASE("ChannelMapper: negative destination channel is rejected", "[channel_mapper]") {
    std::vector<ChannelEdge> edges{{0, -1}};
    REQUIRE(validate(edges) == ChannelMapperError::kNegativeChannel);
}

TEST_CASE("ChannelMapper: duplicate source is allowed (fan-out)",
          "[channel_mapper][fan_out]") {
    // Phase 6 refinement #1: fan-out is supported. The same source
    // channel may feed multiple destinations — the hot-path loop
    // iterates per output slot, so a shared src simply replicates the
    // sample into each mapped dst.
    std::vector<ChannelEdge> edges{{1, 3}, {1, 4}};
    REQUIRE(validate(edges) == ChannelMapperError::kOk);
}

TEST_CASE("ChannelMapper: one source feeding three destinations",
          "[channel_mapper][fan_out]") {
    // Broader fan-out: one-to-many with N = 3.
    std::vector<ChannelEdge> edges{{0, 0}, {0, 1}, {0, 2}};
    REQUIRE(validate(edges) == ChannelMapperError::kOk);
}

TEST_CASE("ChannelMapper: duplicate destination is rejected", "[channel_mapper]") {
    // Two src channels mapped to the same dst would be fan-in (summing);
    // explicitly out of scope per docs/spec.md Appendix A.
    std::vector<ChannelEdge> edges{{1, 3}, {2, 3}};
    REQUIRE(validate(edges) == ChannelMapperError::kDuplicateDestination);
}

TEST_CASE("ChannelMapper: empty takes precedence over other errors", "[channel_mapper]") {
    // If the list is empty, we should get kEmpty regardless of what
    // a hypothetical caller might expect. Documents the validation order.
    std::vector<ChannelEdge> edges;
    REQUIRE(validate(edges) == ChannelMapperError::kEmpty);
}

TEST_CASE("ChannelMapper: negative takes precedence over duplicate", "[channel_mapper]") {
    // First edge has a negative src; duplicate comes later. Negative
    // should be reported first.
    std::vector<ChannelEdge> edges{{-1, 3}, {0, 3}};
    REQUIRE(validate(edges) == ChannelMapperError::kNegativeChannel);
}

TEST_CASE("ChannelMapper: fan-out mixed with duplicate-dst reports dst",
          "[channel_mapper][fan_out]") {
    // The {1,3},{1,4} pair is valid fan-out; the trailing {2,3}
    // collides with the first edge's dst. Duplicate-dst wins.
    std::vector<ChannelEdge> edges{{1, 3}, {1, 4}, {2, 3}};
    REQUIRE(validate(edges) == ChannelMapperError::kDuplicateDestination);
}

TEST_CASE("ChannelMapper: 32-channel fully-populated mapping passes", "[channel_mapper]") {
    // Stress: a dense 32-channel mapping (e.g., routing all 32 V31
    // outputs). No duplicates; should validate cleanly.
    std::vector<ChannelEdge> edges;
    edges.reserve(32);
    for (int i = 0; i < 32; ++i) {
        edges.push_back({i, i});
    }
    REQUIRE(validate(edges) == ChannelMapperError::kOk);
}

TEST_CASE("ChannelMapper: accepts std::array via span", "[channel_mapper]") {
    // Verify the span API works with non-vector containers.
    std::array<ChannelEdge, 2> edges{ChannelEdge{0, 5}, ChannelEdge{1, 6}};
    REQUIRE(validate(edges) == ChannelMapperError::kOk);
}

TEST_CASE("ChannelMapper: error names are non-empty for all codes", "[channel_mapper]") {
    REQUIRE(std::string_view{channelMapperErrorName(ChannelMapperError::kOk)} == "ok");
    REQUIRE(std::string_view{channelMapperErrorName(ChannelMapperError::kEmpty)}.size() > 0);
    REQUIRE(std::string_view{channelMapperErrorName(ChannelMapperError::kNegativeChannel)}.size() > 0);
    REQUIRE(std::string_view{channelMapperErrorName(ChannelMapperError::kDuplicateDestination)}.size() > 0);
}

TEST_CASE("ChannelEdge: equality is structural", "[channel_mapper]") {
    REQUIRE(ChannelEdge{1, 2} == ChannelEdge{1, 2});
    REQUIRE_FALSE(ChannelEdge{1, 2} == ChannelEdge{1, 3});
    REQUIRE_FALSE(ChannelEdge{1, 2} == ChannelEdge{0, 2});
}
