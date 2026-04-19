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
        .library(name: "JboxEngineSwift", targets: ["JboxEngineSwift"]),
    ],
    targets: [
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

        // Tests.
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
