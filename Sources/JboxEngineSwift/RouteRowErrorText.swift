import JboxEngineC

/// Map a route's `(state, lastError)` pair to the diagnostic string the
/// row should render below the route name. `nil` means "no diagnostic
/// text" — the row keeps its primary status glyph + label only.
///
/// Phase 7.6.6 introduces the WAITING-with-error variants. Prior to
/// 7.6.6 the row only rendered text on the hard ERROR state; an
/// initial-WAITING (no devices yet, last_error == JBOX_OK) and a
/// device-loss WAITING (last_error == JBOX_ERR_DEVICE_GONE) looked
/// identical to the user.
public func routeRowErrorText(state: RouteState,
                              lastError: jbox_error_code_t) -> String? {
    switch state {
    case .error:
        return String(cString: jbox_error_code_name(lastError))
    case .waiting:
        guard lastError != JBOX_OK else { return nil }
        switch lastError {
        case JBOX_ERR_DEVICE_GONE:
            return "Device disconnected — waiting for it to return."
        case JBOX_ERR_DEVICE_STALLED:
            return "No audio — device stopped responding."
        case JBOX_ERR_SYSTEM_SUSPENDED:
            return "Recovering from sleep…"
        default:
            return String(cString: jbox_error_code_name(lastError))
        }
    case .running, .starting, .stopped:
        return nil
    }
}
