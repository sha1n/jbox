import Foundation

/// Per-channel peak-hold tracker that sits above `MeterPeaks`.
/// Each `(routeId, side, channel)` slot remembers the highest peak
/// observed recently and decays linearly to zero over
/// `holdDurationSeconds`. An observation that exceeds the current
/// (decayed) value promotes and resets the hold; a lower observation
/// is ignored. Spec § 4.5 — peak-hold tick on the bar meters.
public struct PeakHoldTracker: Sendable {

    public enum Side: Hashable, Sendable {
        case source, dest
    }

    public let holdDurationSeconds: TimeInterval

    private struct Key: Hashable {
        let routeId: UInt32
        let side: Side
        let channel: Int
    }

    private struct Entry {
        var value: Float
        var setAt: TimeInterval
    }

    private var entries: [Key: Entry] = [:]

    public init(holdDurationSeconds: TimeInterval = 1.0) {
        self.holdDurationSeconds = holdDurationSeconds
    }

    public mutating func observe(routeId: UInt32,
                                 side: Side,
                                 channel: Int,
                                 value: Float,
                                 now: TimeInterval) {
        guard value > 0 else { return }
        let key = Key(routeId: routeId, side: side, channel: channel)
        if let existing = entries[key] {
            let current = decayed(entry: existing, now: now)
            if value > current {
                entries[key] = Entry(value: value, setAt: now)
            }
        } else {
            entries[key] = Entry(value: value, setAt: now)
        }
    }

    public func heldValue(routeId: UInt32,
                          side: Side,
                          channel: Int,
                          now: TimeInterval) -> Float {
        let key = Key(routeId: routeId, side: side, channel: channel)
        guard let entry = entries[key] else { return 0 }
        return decayed(entry: entry, now: now)
    }

    public mutating func forget(routeId: UInt32) {
        entries = entries.filter { $0.key.routeId != routeId }
    }

    private func decayed(entry: Entry, now: TimeInterval) -> Float {
        let age = now - entry.setAt
        if age <= 0 { return entry.value }
        if age >= holdDurationSeconds { return 0 }
        let remaining = Float(1.0 - age / holdDurationSeconds)
        return entry.value * remaining
    }
}
