// power_state_watcher.cpp — PowerStateWatcher impl.

#include "power_state_watcher.hpp"

#include <utility>

namespace jbox::control {

PowerStateWatcher::PowerStateWatcher(IPowerEventSource& source)
    : source_(source) {
    source_.setPowerStateListener(&PowerStateWatcher::onSourceEvent, this);
}

PowerStateWatcher::~PowerStateWatcher() {
    source_.setPowerStateListener(nullptr, nullptr);
}

void PowerStateWatcher::setSleepHandler(SleepHandler handler) {
    sleep_handler_ = std::move(handler);
}

void PowerStateWatcher::onSourceEvent(const PowerStateEvent& ev,
                                      void* user_data) {
    auto* self = static_cast<PowerStateWatcher*>(user_data);
    if (self == nullptr) return;
    self->onEvent(ev);
}

void PowerStateWatcher::onEvent(const PowerStateEvent& ev) {
    switch (ev.kind) {
        case PowerStateEvent::kWillSleep: {
            // Synchronous: run the handler (if any), then ack to the
            // source no matter what. macOS waits for the ack; failing
            // to send it stalls the sleep transition.
            if (sleep_handler_) sleep_handler_();
            source_.acknowledgeSleep();
            break;
        }
        case PowerStateEvent::kPoweredOn: {
            std::lock_guard<std::mutex> lock(mutex_);
            wake_events_.push_back(ev);
            break;
        }
    }
}

std::vector<PowerStateEvent> PowerStateWatcher::drain() {
    std::vector<PowerStateEvent> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(wake_events_.size());
    while (!wake_events_.empty()) {
        out.push_back(wake_events_.front());
        wake_events_.pop_front();
    }
    return out;
}

bool PowerStateWatcher::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wake_events_.empty();
}

}  // namespace jbox::control
