import JboxEngineC

/// Swift-facing entry point for the Jbox engine.
///
/// This phase provides only ABI-version access; real engine lifecycle
/// and route management are introduced in Phase 2 and Phase 3
/// (see `docs/plan.md`).
public enum JboxEngine {
    /// Runtime ABI version reported by the engine.
    public static var abiVersion: UInt32 {
        jbox_engine_abi_version()
    }
}
