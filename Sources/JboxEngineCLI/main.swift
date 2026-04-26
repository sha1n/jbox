import Foundation
import JboxEngineCLICore
import JboxEngineSwift
import Darwin

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
