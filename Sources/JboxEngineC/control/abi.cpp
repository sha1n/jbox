// abi.cpp — implementation of the ABI version entry point.
//
// Lives in control/ (not rt/) because it has no real-time constraints —
// callers use it at startup to check compatibility.
//
// See docs/spec.md § 1.6.

#include "jbox_engine.h"

extern "C" uint32_t jbox_engine_abi_version(void) {
    return JBOX_ENGINE_ABI_VERSION;
}
