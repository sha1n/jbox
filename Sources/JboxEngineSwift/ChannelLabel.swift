import Foundation

/// Formatting helpers for per-channel UI labels.
///
/// Core Audio exposes channel names through
/// `kAudioObjectPropertyElementName`; devices like the UA Apollo
/// publish `"Monitor L"`, simpler devices publish nothing. The UI
/// wants to show `"Ch N · <name>"` when a name is present and
/// `"Ch N"` when it isn't.
///
/// Extracted to its own file so SwiftUI views and unit tests share one
/// implementation — the formatting logic is pure and deterministic,
/// easy to test; the pickers that consume it are not.
public enum ChannelLabel {

    /// Render a label for the channel at `index` (0-indexed internally,
    /// rendered 1-indexed in the UI per spec § 2.3), given the array of
    /// driver-published channel names for the device.
    ///
    /// Rules:
    /// - `"Ch \(index + 1)"` is always the base label.
    /// - If `index` is in bounds and `names[index]` contains any
    ///   non-whitespace characters, the trimmed name is appended
    ///   after `" · "`.
    /// - Out-of-bounds indices and whitespace-only names both fall
    ///   back silently to the numeric label.
    public static func format(index: Int, names: [String]) -> String {
        let base = "Ch \(index + 1)"
        guard index >= 0, index < names.count else { return base }
        let trimmed = names[index].trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? base : "\(base) · \(trimmed)"
    }
}
