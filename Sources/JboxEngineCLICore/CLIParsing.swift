// CLI argument parsing for JboxEngineCLI.
//
// Lives in its own library target so the parsing logic can be unit-
// tested without spinning up the executable. The CLI's `main.swift`
// is a thin shell that imports this module and dispatches on the
// parsed CLICommand.

import Foundation
import JboxEngineSwift

public struct ParsedRoute: Equatable {
    public let sourceUID: String
    public let destUID: String
    public let mapping: [ChannelEdge]

    public init(sourceUID: String, destUID: String, mapping: [ChannelEdge]) {
        self.sourceUID = sourceUID
        self.destUID = destUID
        self.mapping = mapping
    }
}

public enum CLICommand: Equatable {
    case version
    case listDevices
    case runRoute(ParsedRoute)
    case help
}

public func usage() -> String {
    return """
Usage:
  JboxEngineCLI [--version | -V]
  JboxEngineCLI [--list-devices | -l]
  JboxEngineCLI --route <src-uid>@<src-chs>-><dst-uid>@<dst-chs>

Channels are 1-indexed in the CLI (converted to 0-indexed for the engine).
Example:
  JboxEngineCLI --route 'AppleHDA:0@1,2->com.apple.audio.CoreAudio:7@3,4'

Options:
  --version, -V        Print the engine ABI version and exit.
  --list-devices, -l   Print all Core Audio devices visible to the engine.
  --route <spec>       Configure a route from spec and start it. Runs until
                       SIGINT (Ctrl-C), polling status every second.
  --help, -h           Show this help.
"""
}

public func parseChannelList(_ s: String) throws -> [UInt32] {
    let parts = s.split(separator: ",", omittingEmptySubsequences: true)
    var out: [UInt32] = []
    for p in parts {
        guard let v = UInt32(p.trimmingCharacters(in: .whitespaces)) else {
            throw JboxError(code: JBOX_ERR_INVALID_ARGUMENT,
                            message: "invalid channel: '\(p)'")
        }
        if v < 1 {
            throw JboxError(code: JBOX_ERR_INVALID_ARGUMENT,
                            message: "channel numbers are 1-indexed; got \(v)")
        }
        out.append(v - 1)
    }
    return out
}

public func parseRouteSpec(_ spec: String) throws -> ParsedRoute {
    guard let arrow = spec.range(of: "->") else {
        throw JboxError(code: JBOX_ERR_INVALID_ARGUMENT,
                        message: "route spec missing '->': '\(spec)'")
    }
    let srcSpec = String(spec[..<arrow.lowerBound])
    let dstSpec = String(spec[arrow.upperBound...])

    // Last '@' so UIDs may contain '@'.
    func splitSide(_ side: String) throws -> (String, String) {
        guard let at = side.range(of: "@", options: .backwards) else {
            throw JboxError(code: JBOX_ERR_INVALID_ARGUMENT,
                            message: "side missing '@': '\(side)'")
        }
        return (String(side[..<at.lowerBound]), String(side[at.upperBound...]))
    }

    let (srcUID, srcChs) = try splitSide(srcSpec)
    let (dstUID, dstChs) = try splitSide(dstSpec)

    let srcChannels = try parseChannelList(srcChs)
    let dstChannels = try parseChannelList(dstChs)

    if srcChannels.count != dstChannels.count {
        throw JboxError(code: JBOX_ERR_MAPPING_INVALID,
                        message: "source and destination channel counts differ")
    }

    let mapping = zip(srcChannels, dstChannels).map {
        ChannelEdge(src: $0.0, dst: $0.1)
    }
    return ParsedRoute(sourceUID: srcUID, destUID: dstUID, mapping: mapping)
}

// `args` is the raw argv (including argv[0]); the leading program name
// is dropped here. Callers that pass already-trimmed args still get the
// expected behaviour because the dropFirst on an empty array is a no-op.
public func parseCLI(_ args: [String]) -> Result<CLICommand, JboxError> {
    let a = Array(args.dropFirst())
    if a.isEmpty { return .success(.help) }

    let arg = a[0]
    switch arg {
    case "--help", "-h":             return .success(.help)
    case "--version", "-V":          return .success(.version)
    case "--list-devices", "-l":     return .success(.listDevices)
    case "--route":
        guard a.count >= 2 else {
            return .failure(JboxError(code: JBOX_ERR_INVALID_ARGUMENT,
                                      message: "--route requires a spec argument"))
        }
        do {
            let parsed = try parseRouteSpec(a[1])
            return .success(.runRoute(parsed))
        } catch let e as JboxError {
            return .failure(e)
        } catch {
            return .failure(JboxError(code: JBOX_ERR_INVALID_ARGUMENT,
                                      message: "\(error)"))
        }
    default:
        return .failure(JboxError(code: JBOX_ERR_INVALID_ARGUMENT,
                                  message: "unknown argument: \(arg)"))
    }
}
