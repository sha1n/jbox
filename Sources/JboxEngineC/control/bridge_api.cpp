// bridge_api.cpp — implementations of the public C bridge entry points.
//
// Runs on the control thread (or any non-RT caller). All entry points
// now delegate to the Engine class (control/engine.hpp). In production
// jbox_engine_create instantiates Engine with a CoreAudioBackend; the
// jbox::internal::createEngineWithBackend helper in this file is
// used by tests to substitute a SimulatedBackend.

#include "core_audio_backend.hpp"
#include "engine.hpp"
#include "jbox_engine.h"
#include "log_drainer.hpp"

#include <os/log.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace {
os_log_t bridgeLog() {
    static os_log_t log = os_log_create("com.jbox.app", "bridge");
    return log;
}
}  // namespace

// -----------------------------------------------------------------------------
// Opaque handle
// -----------------------------------------------------------------------------

struct jbox_engine {
    std::unique_ptr<jbox::control::Engine> impl;
};

namespace jbox::internal {

// Test-only helper: construct a jbox_engine_t backed by a custom
// IDeviceBackend (typically SimulatedBackend). Not exposed in the
// public header; tests declare it extern-cpp and call it directly.
// Signature may change without a MAJOR bump.
jbox_engine_t* createEngineWithBackend(
    std::unique_ptr<jbox::control::IDeviceBackend> backend,
    bool spawn_sampler_thread,
    bool spawn_log_drainer) {
    auto* e = new jbox_engine{};
    e->impl = std::make_unique<jbox::control::Engine>(
        std::move(backend), spawn_sampler_thread, spawn_log_drainer);
    return e;
}

// Test hook: swap in a custom sink for the engine's log drainer.
// Returns false if the engine has no drainer (e.g., constructed with
// spawn_log_drainer=false).
bool setLogSink(jbox_engine_t* engine,
                jbox::control::LogDrainer::Sink sink) {
    if (engine == nullptr || engine->impl == nullptr) return false;
    auto* d = engine->impl->logDrainer();
    if (d == nullptr) return false;
    d->setSink(std::move(sink));
    return true;
}

void tickDriftOnce(jbox_engine_t* engine, double dt_seconds) {
    if (engine == nullptr || engine->impl == nullptr) return;
    engine->impl->driftSampler().tickAll(dt_seconds);
}

}  // namespace jbox::internal

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

namespace {

void setError(jbox_error_t* err, jbox_error_code_t code, const char* message) {
    if (err != nullptr) {
        err->code = code;
        err->message = message;
    }
}

// Convert a C-side route config into the C++ form expected by
// RouteManager. Safely handles NULL string fields (treated as empty).
jbox::control::RouteManager::RouteConfig convertRouteConfig(
    const jbox_route_config_t& cfg) {
    jbox::control::RouteManager::RouteConfig out;
    out.source_uid = (cfg.source_uid != nullptr) ? cfg.source_uid : "";
    out.dest_uid   = (cfg.dest_uid   != nullptr) ? cfg.dest_uid   : "";
    out.name       = (cfg.name       != nullptr) ? cfg.name       : "";
    out.mapping.reserve(cfg.mapping_count);
    for (std::size_t i = 0; i < cfg.mapping_count; ++i) {
        out.mapping.push_back({
            static_cast<int>(cfg.mapping[i].src),
            static_cast<int>(cfg.mapping[i].dst),
        });
    }
    return out;
}

// Safe bounded string copy; always NUL-terminates.
void copyFixed(char* dst, std::size_t dst_capacity, const std::string& src) {
    if (dst_capacity == 0) return;
    const std::size_t n =
        (src.size() < dst_capacity - 1) ? src.size() : dst_capacity - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

}  // namespace

// -----------------------------------------------------------------------------
// C API implementations
// -----------------------------------------------------------------------------

extern "C" {

uint32_t jbox_engine_abi_version(void) {
    return JBOX_ENGINE_ABI_VERSION;
}

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

void jbox_device_list_free(jbox_device_list_t* list) {
    if (list == nullptr) return;
    std::free(list->devices);
    list->devices = nullptr;
    list->count = 0;
    std::free(list);
}

// ---- Engine lifecycle -------------------------------------------------------

jbox_engine_t* jbox_engine_create(const jbox_engine_config_t* /*config*/,
                                  jbox_error_t* err) {
    try {
        auto backend = std::make_unique<jbox::control::CoreAudioBackend>();
        auto* e = jbox::internal::createEngineWithBackend(
            std::move(backend),
            /*spawn_sampler_thread=*/true,
            /*spawn_log_drainer=*/true);
        os_log(bridgeLog(), "engine created abi=%u", JBOX_ENGINE_ABI_VERSION);
        return e;
    } catch (const std::bad_alloc&) {
        os_log_error(bridgeLog(), "engine create failed: out of memory");
        setError(err, JBOX_ERR_RESOURCE_EXHAUSTED, "out of memory");
        return nullptr;
    } catch (...) {
        os_log_error(bridgeLog(), "engine create failed: unknown exception");
        setError(err, JBOX_ERR_INTERNAL, "engine construction failed");
        return nullptr;
    }
}

void jbox_engine_destroy(jbox_engine_t* engine) {
    if (engine != nullptr) {
        os_log(bridgeLog(), "engine destroy");
    }
    delete engine;
}

// ---- Engine operations ------------------------------------------------------

jbox_device_list_t* jbox_engine_enumerate_devices(jbox_engine_t* engine,
                                                  jbox_error_t* err) {
    if (engine == nullptr) {
        setError(err, JBOX_ERR_INVALID_ARGUMENT, "engine is null");
        return nullptr;
    }
    try {
        const auto& snapshot = engine->impl->enumerateDevices();

        auto* list = static_cast<jbox_device_list_t*>(
            std::malloc(sizeof(jbox_device_list_t)));
        if (list == nullptr) {
            setError(err, JBOX_ERR_RESOURCE_EXHAUSTED, "out of memory");
            return nullptr;
        }
        list->count = snapshot.size();
        list->devices = nullptr;
        if (!snapshot.empty()) {
            list->devices = static_cast<jbox_device_info_t*>(
                std::malloc(snapshot.size() * sizeof(jbox_device_info_t)));
            if (list->devices == nullptr) {
                std::free(list);
                setError(err, JBOX_ERR_RESOURCE_EXHAUSTED, "out of memory");
                return nullptr;
            }
        }
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            const auto& src = snapshot[i];
            auto& dst = list->devices[i];
            copyFixed(dst.uid,  JBOX_UID_MAX_LEN,  src.uid);
            copyFixed(dst.name, JBOX_NAME_MAX_LEN, src.name);
            dst.direction            = src.direction;
            dst.input_channel_count  = src.input_channel_count;
            dst.output_channel_count = src.output_channel_count;
            dst.nominal_sample_rate  = src.nominal_sample_rate;
            dst.buffer_frame_size    = src.buffer_frame_size;
        }
        return list;
    } catch (const std::bad_alloc&) {
        setError(err, JBOX_ERR_RESOURCE_EXHAUSTED, "out of memory");
        return nullptr;
    } catch (...) {
        setError(err, JBOX_ERR_INTERNAL, "enumerate failed");
        return nullptr;
    }
}

jbox_route_id_t jbox_engine_add_route(jbox_engine_t* engine,
                                      const jbox_route_config_t* config,
                                      jbox_error_t* err) {
    if (engine == nullptr || config == nullptr) {
        setError(err, JBOX_ERR_INVALID_ARGUMENT, "null argument");
        return JBOX_INVALID_ROUTE_ID;
    }
    if (config->mapping_count > 0 && config->mapping == nullptr) {
        setError(err, JBOX_ERR_INVALID_ARGUMENT, "mapping pointer is null");
        return JBOX_INVALID_ROUTE_ID;
    }
    try {
        auto cpp_cfg = convertRouteConfig(*config);
        auto id = engine->impl->addRoute(cpp_cfg, err);
        if (id == JBOX_INVALID_ROUTE_ID) {
            os_log_error(bridgeLog(),
                         "add_route rejected: src=%{public}s dst=%{public}s code=%d",
                         cpp_cfg.source_uid.c_str(),
                         cpp_cfg.dest_uid.c_str(),
                         err != nullptr ? static_cast<int>(err->code) : -1);
        } else {
            os_log(bridgeLog(),
                   "add_route ok: id=%u src=%{public}s dst=%{public}s channels=%zu",
                   id, cpp_cfg.source_uid.c_str(), cpp_cfg.dest_uid.c_str(),
                   cpp_cfg.mapping.size());
        }
        return id;
    } catch (const std::bad_alloc&) {
        setError(err, JBOX_ERR_RESOURCE_EXHAUSTED, "out of memory");
        return JBOX_INVALID_ROUTE_ID;
    } catch (...) {
        setError(err, JBOX_ERR_INTERNAL, "add_route failed");
        return JBOX_INVALID_ROUTE_ID;
    }
}

jbox_error_code_t jbox_engine_remove_route(jbox_engine_t* engine,
                                           jbox_route_id_t route_id) {
    if (engine == nullptr) return JBOX_ERR_INVALID_ARGUMENT;
    try {
        return engine->impl->removeRoute(route_id);
    } catch (...) {
        return JBOX_ERR_INTERNAL;
    }
}

jbox_error_code_t jbox_engine_start_route(jbox_engine_t* engine,
                                          jbox_route_id_t route_id) {
    if (engine == nullptr) return JBOX_ERR_INVALID_ARGUMENT;
    try {
        return engine->impl->startRoute(route_id);
    } catch (...) {
        return JBOX_ERR_INTERNAL;
    }
}

jbox_error_code_t jbox_engine_stop_route(jbox_engine_t* engine,
                                         jbox_route_id_t route_id) {
    if (engine == nullptr) return JBOX_ERR_INVALID_ARGUMENT;
    try {
        return engine->impl->stopRoute(route_id);
    } catch (...) {
        return JBOX_ERR_INTERNAL;
    }
}

jbox_error_code_t jbox_engine_poll_route_status(jbox_engine_t* engine,
                                                jbox_route_id_t route_id,
                                                jbox_route_status_t* out_status) {
    if (engine == nullptr || out_status == nullptr) return JBOX_ERR_INVALID_ARGUMENT;
    try {
        return engine->impl->pollStatus(route_id, out_status);
    } catch (...) {
        return JBOX_ERR_INTERNAL;
    }
}

}  // extern "C"
