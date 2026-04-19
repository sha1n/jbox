// bridge_api.cpp — implementations of the public C bridge entry points.
//
// Runs on the control thread (or any non-RT caller). No RT safety
// required. Most entry points are currently stubs returning
// JBOX_ERR_NOT_IMPLEMENTED; they are filled in across later Phase 3
// commits as the engine internals come online. See docs/plan.md
// § Phase 3 for the per-commit schedule.

#include "jbox_engine.h"

#include <cstdlib>

extern "C" {

// -----------------------------------------------------------------------------
// ABI version
// -----------------------------------------------------------------------------

uint32_t jbox_engine_abi_version(void) {
    return JBOX_ENGINE_ABI_VERSION;
}

// -----------------------------------------------------------------------------
// Error / state naming
// -----------------------------------------------------------------------------

const char* jbox_error_code_name(jbox_error_code_t code) {
    switch (code) {
        case JBOX_OK:                     return "ok";
        case JBOX_ERR_INVALID_ARGUMENT:   return "invalid argument";
        case JBOX_ERR_DEVICE_NOT_FOUND:   return "device not found";
        case JBOX_ERR_MAPPING_INVALID:    return "mapping invalid";
        case JBOX_ERR_RESOURCE_EXHAUSTED: return "resource exhausted";
        case JBOX_ERR_DEVICE_BUSY:        return "device busy";
        case JBOX_ERR_NOT_IMPLEMENTED:    return "not implemented";
        case JBOX_ERR_INTERNAL:           return "internal";
    }
    return "unknown";
}

const char* jbox_route_state_name(jbox_route_state_t state) {
    switch (state) {
        case JBOX_ROUTE_STATE_STOPPED:  return "stopped";
        case JBOX_ROUTE_STATE_WAITING:  return "waiting";
        case JBOX_ROUTE_STATE_STARTING: return "starting";
        case JBOX_ROUTE_STATE_RUNNING:  return "running";
        case JBOX_ROUTE_STATE_ERROR:    return "error";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
// Device list memory management
// -----------------------------------------------------------------------------

void jbox_device_list_free(jbox_device_list_t* list) {
    if (list == nullptr) return;
    std::free(list->devices);
    list->devices = nullptr;
    list->count = 0;
    std::free(list);
}

// -----------------------------------------------------------------------------
// Stubs awaiting later commits in Phase 3
// -----------------------------------------------------------------------------

// Shared helper to populate an out-error if the caller requested details.
static void setNotImplemented(jbox_error_t* err) {
    if (err != nullptr) {
        err->code    = JBOX_ERR_NOT_IMPLEMENTED;
        err->message = "not implemented yet (see docs/plan.md Phase 3)";
    }
}

jbox_engine_t* jbox_engine_create(const jbox_engine_config_t* /*config*/,
                                  jbox_error_t*               err) {
    setNotImplemented(err);
    return nullptr;
}

void jbox_engine_destroy(jbox_engine_t* /*engine*/) {
    // No-op. Safe with the NULL returned by the stubbed create.
}

jbox_device_list_t* jbox_engine_enumerate_devices(jbox_engine_t* /*engine*/,
                                                  jbox_error_t*  err) {
    setNotImplemented(err);
    return nullptr;
}

jbox_route_id_t jbox_engine_add_route(jbox_engine_t*             /*engine*/,
                                      const jbox_route_config_t* /*config*/,
                                      jbox_error_t*              err) {
    setNotImplemented(err);
    return JBOX_INVALID_ROUTE_ID;
}

jbox_error_code_t jbox_engine_remove_route(jbox_engine_t*  /*engine*/,
                                           jbox_route_id_t /*route_id*/) {
    return JBOX_ERR_NOT_IMPLEMENTED;
}

jbox_error_code_t jbox_engine_start_route(jbox_engine_t*  /*engine*/,
                                          jbox_route_id_t /*route_id*/) {
    return JBOX_ERR_NOT_IMPLEMENTED;
}

jbox_error_code_t jbox_engine_stop_route(jbox_engine_t*  /*engine*/,
                                         jbox_route_id_t /*route_id*/) {
    return JBOX_ERR_NOT_IMPLEMENTED;
}

jbox_error_code_t jbox_engine_poll_route_status(jbox_engine_t*       /*engine*/,
                                                jbox_route_id_t      /*route_id*/,
                                                jbox_route_status_t* /*out_status*/) {
    return JBOX_ERR_NOT_IMPLEMENTED;
}

}  // extern "C"
