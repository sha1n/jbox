import Foundation
import JboxEngineSwift

// Phase 1 CLI: prints the engine ABI version and exits.
// Future phases extend this with --list-devices and --route flags.
// See docs/plan.md § Phase 3.

let version = JboxEngine.abiVersion
print("Jbox Engine ABI version: \(version)")
