import Foundation
import JboxEngineSwift
import Darwin

// -----------------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------------

struct ParsedRoute {
    let sourceUID: String
    let destUID: String
    let mapping: [ChannelEdge]
}

enum CLICommand {
    case version
    case listDevices
    case runRoute(ParsedRoute)
    case help
}

func usage() -> String {
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

func parseChannelList(_ s: String) throws -> [UInt32] {
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

func parseRouteSpec(_ spec: String) throws -> ParsedRoute {
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

func parseCLI(_ args: [String]) -> Result<CLICommand, JboxError> {
    let a = Array(args.dropFirst())
    if a.isEmpty { return .success(.help) }

    let i = 0
    while i < a.count {
        let arg = a[i]
        switch arg {
        case "--help", "-h":             return .success(.help)
        case "--version", "-V":          return .success(.version)
        case "--list-devices", "-l":     return .success(.listDevices)
        case "--route":
            guard i + 1 < a.count else {
                return .failure(JboxError(code: JBOX_ERR_INVALID_ARGUMENT,
                                          message: "--route requires a spec argument"))
            }
            do {
                let parsed = try parseRouteSpec(a[i + 1])
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
    return .success(.help)
}

// -----------------------------------------------------------------------------
// SIGINT handling
// -----------------------------------------------------------------------------

// Global sig_atomic_t flag toggled by the SIGINT handler. Accessed
// from signal handler context (async-signal-safe write) and from the
// main polling loop (read). The @unchecked is fine for a signal flag.
nonisolated(unsafe) var gShouldExit: sig_atomic_t = 0

func installSigintHandler() {
    // Use a plain C signal handler. Setting a sig_atomic_t is the
    // only safe operation inside it.
    signal(SIGINT) { _ in
        gShouldExit = 1
    }
}

// -----------------------------------------------------------------------------
// Command handlers
// -----------------------------------------------------------------------------

func printVersion() {
    print("Jbox Engine ABI version: \(JboxEngine.abiVersion)")
}

func printDevices() throws {
    let engine = try Engine()
    let devices = try engine.enumerateDevices()
    if devices.isEmpty {
        print("No audio devices found.")
        return
    }
    // DIR column: 'I' for input, 'O' for output, '-' for not-present in that direction.
    let header = "DIR   IN   OUT  SR         BUF    UID (NAME)"
    print(header)
    print(String(repeating: "-", count: header.count))
    for d in devices {
        let dir = (d.directionInput  ? "I" : "-") + (d.directionOutput ? "O" : "-")
        let line = String(
            format: "%-5@ %-4u %-4u %-10.0f %-5u  %@ (%@)",
            dir as NSString, d.inputChannelCount, d.outputChannelCount,
            d.nominalSampleRate, d.bufferFrameSize,
            d.uid as NSString, d.name as NSString)
        print(line)
    }
}

func runRoute(_ route: ParsedRoute) throws {
    let engine = try Engine()
    let routeId = try engine.addRoute(
        sourceUID: route.sourceUID,
        destUID: route.destUID,
        mapping: route.mapping,
        name: "cli-route")

    print("Route \(routeId) configured: \(route.sourceUID) -> \(route.destUID)")
    try engine.startRoute(routeId)
    let initial = try engine.pollStatus(routeId)
    print("State: \(initial.state)")

    installSigintHandler()

    // Polling loop on the main thread. Prints status once a second;
    // exits when SIGINT flips the flag. Ctrl-C is the expected way
    // to stop — no TTY interaction beyond that.
    while gShouldExit == 0 {
        do {
            let st = try engine.pollStatus(routeId)
            FileHandle.standardError.write(
                "state=\(st.state) produced=\(st.framesProduced) consumed=\(st.framesConsumed) underruns=\(st.underrunCount) overruns=\(st.overrunCount)\n"
                    .data(using: .utf8) ?? Data()
            )
        } catch {
            FileHandle.standardError.write(
                "poll error: \(error)\n".data(using: .utf8) ?? Data()
            )
            break
        }
        // Sleep in small increments so SIGINT is responsive.
        for _ in 0..<10 {
            if gShouldExit != 0 { break }
            Thread.sleep(forTimeInterval: 0.1)
        }
    }

    print("")
    print("Stopping route \(routeId)...")
    try engine.stopRoute(routeId)
    try engine.removeRoute(routeId)
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------

let parseResult = parseCLI(CommandLine.arguments)

switch parseResult {
case .failure(let err):
    FileHandle.standardError.write("error: \(err)\n".data(using: .utf8) ?? Data())
    FileHandle.standardError.write(usage().data(using: .utf8) ?? Data())
    exit(2)
case .success(let cmd):
    do {
        switch cmd {
        case .help:
            print(usage())
        case .version:
            printVersion()
        case .listDevices:
            try printDevices()
        case .runRoute(let r):
            try runRoute(r)
        }
    } catch {
        FileHandle.standardError.write("error: \(error)\n".data(using: .utf8) ?? Data())
        exit(1)
    }
}
