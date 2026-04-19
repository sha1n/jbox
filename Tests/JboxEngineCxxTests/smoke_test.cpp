// smoke_test.cpp — verifies the C++ test harness itself works end-to-end.
//
// This test will be deleted or kept as a trivial sanity check once real
// primitive tests land in later Phase 2 commits.

#include <catch_amalgamated.hpp>

#include "jbox_engine.h"

TEST_CASE("engine ABI version is accessible from C++", "[smoke]") {
    REQUIRE(jbox_engine_abi_version() == JBOX_ENGINE_ABI_VERSION);
    REQUIRE(jbox_engine_abi_version() > 0);
}
