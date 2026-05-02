import Testing
import JboxEngineC
@testable import JboxEngineSwift

@Suite("RouteRowErrorText")
struct RouteRowErrorTextTests {
    @Test("returns nil for WAITING with JBOX_OK (initial waiting)")
    func waitingWithoutErrorReturnsNil() {
        #expect(routeRowErrorText(state: .waiting, lastError: JBOX_OK) == nil)
    }

    @Test("returns the device-disconnected line for WAITING with DEVICE_GONE")
    func waitingWithDeviceGone() {
        let text = routeRowErrorText(state: .waiting, lastError: JBOX_ERR_DEVICE_GONE)
        #expect(text == "Device disconnected — waiting for it to return.")
    }

    @Test("returns the stalled line for WAITING with DEVICE_STALLED")
    func waitingWithDeviceStalled() {
        let text = routeRowErrorText(state: .waiting, lastError: JBOX_ERR_DEVICE_STALLED)
        #expect(text == "No audio — device stopped responding.")
    }

    @Test("returns the sleep-recovery line for WAITING with SYSTEM_SUSPENDED")
    func waitingWithSystemSuspended() {
        let text = routeRowErrorText(state: .waiting, lastError: JBOX_ERR_SYSTEM_SUSPENDED)
        #expect(text == "Recovering from sleep…")
    }

    @Test("returns jbox_error_code_name for ERROR state")
    func errorStateReturnsCodeName() {
        let text = routeRowErrorText(state: .error, lastError: JBOX_ERR_MAPPING_INVALID)
        #expect(text == "mapping invalid")
    }

    @Test("returns nil for RUNNING regardless of last_error")
    func runningReturnsNil() {
        #expect(routeRowErrorText(state: .running, lastError: JBOX_OK) == nil)
        // Defensive: even if last_error somehow lingered, RUNNING shouldn't surface text.
        #expect(routeRowErrorText(state: .running, lastError: JBOX_ERR_DEVICE_GONE) == nil)
    }

    @Test("returns nil for STOPPED regardless of last_error")
    func stoppedReturnsNil() {
        #expect(routeRowErrorText(state: .stopped, lastError: JBOX_OK) == nil)
    }
}
