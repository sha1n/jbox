// swift-tools-version: 6.0
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "Jbox",
    platforms: [
        .macOS(.v15),
    ],
    products: [
        .executable(name: "JboxApp", targets: ["JboxApp"]),
        .executable(name: "JboxEngineCLI", targets: ["JboxEngineCLI"]),
        .executable(name: "JboxEngineCxxTests", targets: ["JboxEngineCxxTests"]),
        .library(name: "JboxEngineSwift", targets: ["JboxEngineSwift"]),
    ],
    targets: [
        // Vendored Catch2 v3 amalgamated — provides the C++ test runner
        // (including main()) and assertion macros. See ThirdParty/Catch2/README.md.
        .target(
            name: "Catch2",
            path: "ThirdParty/Catch2",
            exclude: ["README.md", "LICENSE.txt"],
            sources: ["catch_amalgamated.cpp"],
            publicHeadersPath: "include"
        ),

        // C++ real-time audio engine.
        // Public C API in Sources/JboxEngineC/include/jbox_engine.h.
        // RT-safe code lives in rt/ (statically scanned).
        // Non-RT engine code lives in control/.
        .target(
            name: "JboxEngineC",
            path: "Sources/JboxEngineC",
            publicHeadersPath: "include"
        ),

        // Thin Swift wrapper over the C bridge.
        .target(
            name: "JboxEngineSwift",
            dependencies: ["JboxEngineC"],
            path: "Sources/JboxEngineSwift"
        ),

        // Standalone headless CLI — exercises the engine without the GUI.
        .executableTarget(
            name: "JboxEngineCLI",
            dependencies: ["JboxEngineSwift"],
            path: "Sources/JboxEngineCLI"
        ),

        // The macOS GUI application.
        .executableTarget(
            name: "JboxApp",
            dependencies: ["JboxEngineSwift"],
            path: "Sources/JboxApp"
        ),

        // C++ test executable — Catch2-based unit and stress tests for
        // the real-time engine internals (C++ classes not reachable
        // through the C bridge).
        //
        // Run with:   swift run JboxEngineCxxTests
        // Run with TSan for concurrent tests:
        //             swift run --sanitize=thread JboxEngineCxxTests
        //
        // Sanitizer flags are applied via the SPM CLI (`--sanitize=...`)
        // rather than baked into the target: the clang-driver flag
        // `-fsanitize=thread` isn't a valid linker argument on its own,
        // and SPM's `--sanitize=thread` wires in the runtime library
        // correctly for us.
        //
        // The header search paths reach into the engine's internal
        // subdirectories so tests can instantiate C++ classes that are
        // not exposed through the public C bridge. This is a deliberate
        // test-only break of encapsulation; production code goes through
        // the C API.
        .executableTarget(
            name: "JboxEngineCxxTests",
            dependencies: ["JboxEngineC", "Catch2"],
            path: "Tests/JboxEngineCxxTests",
            cxxSettings: [
                .headerSearchPath("../../Sources/JboxEngineC/rt"),
                .headerSearchPath("../../Sources/JboxEngineC/control"),
            ]
        ),

        // Swift-side tests (Swift Testing). Verify the C bridge and
        // Swift wrapper behavior.
        .testTarget(
            name: "JboxEngineTests",
            dependencies: ["JboxEngineC", "JboxEngineSwift"],
            path: "Tests/JboxEngineTests"
        ),
        .testTarget(
            name: "JboxEngineIntegrationTests",
            dependencies: ["JboxEngineC", "JboxEngineSwift"],
            path: "Tests/JboxEngineIntegrationTests"
        ),
        .testTarget(
            name: "JboxAppTests",
            dependencies: ["JboxEngineSwift"],
            path: "Tests/JboxAppTests"
        ),
    ],
    cxxLanguageStandard: .cxx20
)
