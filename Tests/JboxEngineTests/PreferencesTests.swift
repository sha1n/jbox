import Testing
@testable import JboxEngineSwift

// Unit tests for the Preferences value types. No engine / UI — these
// exercise the raw-value round-tripping and defaults the SwiftUI
// `@AppStorage` layer depends on. Living under JboxEngineSwift
// (where the types do) keeps them compilable without dragging
// AppKit or SwiftUI into the test target.

@Suite("AppearanceMode")
struct AppearanceModeTests {
    @Test("default is .system (follow OS)")
    func defaultIsSystem() {
        #expect(AppearanceMode.default == .system)
    }

    @Test("raw-value round-trip works for every case")
    func rawValueRoundTrip() {
        for mode in AppearanceMode.allCases {
            let raw = mode.rawValue
            #expect(AppearanceMode(rawValue: raw) == mode)
        }
    }

    @Test("rawValueOrDefault recovers known strings")
    func rawValueOrDefaultKnown() {
        #expect(AppearanceMode(rawValueOrDefault: "system") == .system)
        #expect(AppearanceMode(rawValueOrDefault: "light")  == .light)
        #expect(AppearanceMode(rawValueOrDefault: "dark")   == .dark)
    }

    @Test("rawValueOrDefault falls back to default on unknown input")
    func rawValueOrDefaultUnknown() {
        // Forward-migrated preferences / hand-edited values should
        // never crash the app.
        #expect(AppearanceMode(rawValueOrDefault: "") == .system)
        #expect(AppearanceMode(rawValueOrDefault: "solarized-dark") == .system)
    }

    @Test("allCases covers the three user-visible options")
    func allCasesCovers() {
        #expect(AppearanceMode.allCases.count == 3)
        #expect(AppearanceMode.allCases.contains(.system))
        #expect(AppearanceMode.allCases.contains(.light))
        #expect(AppearanceMode.allCases.contains(.dark))
    }
}

@Suite("BufferSizePolicy")
struct BufferSizePolicyTests {
    @Test("default is .useDeviceSetting")
    func defaultIsUseDevice() {
        #expect(BufferSizePolicy.default == .useDeviceSetting)
    }

    @Test("storedRaw == 0 means useDeviceSetting round-trips back")
    func zeroMeansUseDevice() {
        let policy = BufferSizePolicy(storedRaw: 0)
        #expect(policy == .useDeviceSetting)
        #expect(policy.storedRaw == 0)
        #expect(policy.frames == nil)
    }

    @Test("non-zero storedRaw round-trips as explicitOverride")
    func nonzeroRoundTripsAsOverride() {
        for frames in BufferSizePolicy.frameChoices {
            let policy = BufferSizePolicy(storedRaw: frames)
            #expect(policy == .explicitOverride(frames: frames))
            #expect(policy.storedRaw == frames)
            #expect(policy.frames == frames)
        }
    }

    @Test("frameChoices contains the standard powers of two")
    func frameChoicesAreStandard() {
        // Matches the AddRouteSheet picker options so the global
        // policy never surfaces a frame size the per-route picker
        // can't agree on.
        #expect(BufferSizePolicy.frameChoices == [32, 64, 128, 256, 512, 1024])
    }

    @Test("non-standard storedRaw stays as-is (caller-trusted)")
    func nonStandardRawSurvives() {
        // The HAL is the real gate on validity; the Swift type
        // should not pre-filter. Guarantees that a migrated value
        // from a future options set round-trips to the engine.
        let policy = BufferSizePolicy(storedRaw: 17)
        #expect(policy == .explicitOverride(frames: 17))
        #expect(policy.storedRaw == 17)
        #expect(policy.frames == 17)
    }

    @Test("explicitOverride(0) survives as its own case (lossy round-trip)")
    func explicitOverrideZeroIsLossy() {
        // Syntactically reachable but not produced by the normal
        // `@AppStorage` → `init(storedRaw:)` path. Documented here
        // so a future refactor does not silently start collapsing
        // .explicitOverride(0) into .useDeviceSetting at construction
        // time (which would break API symmetry).
        let direct = BufferSizePolicy.explicitOverride(frames: 0)
        #expect(direct != .useDeviceSetting)
        #expect(direct.frames == 0)
        #expect(direct.storedRaw == 0)
        // Round-tripping through storedRaw is intentionally lossy: 0
        // means "use device setting" on disk, not "explicit override
        // of zero frames" (which is nonsensical audio-wise anyway).
        #expect(BufferSizePolicy(storedRaw: direct.storedRaw) == .useDeviceSetting)
    }

    @Test("different override frame counts are not equal")
    func overridesWithDifferentFramesAreDistinct() {
        #expect(BufferSizePolicy.explicitOverride(frames: 64)
                != BufferSizePolicy.explicitOverride(frames: 128))
    }
}

@Suite("Engine.ResamplerQuality")
struct ResamplerQualityTests {
    @Test("raw values match ABI-stable constants")
    func rawValuesMatchAbi() {
        // The raw values are ABI-stable (jbox_resampler_quality_t).
        // If someone bumps them accidentally every stored
        // preference on-disk breaks, so pin them explicitly.
        #expect(Engine.ResamplerQuality.mastering.rawValue   == 0)
        #expect(Engine.ResamplerQuality.highQuality.rawValue == 1)
    }

    @Test("raw-value init accepts both known values")
    func rawValueInit() {
        #expect(Engine.ResamplerQuality(rawValue: 0) == .mastering)
        #expect(Engine.ResamplerQuality(rawValue: 1) == .highQuality)
        #expect(Engine.ResamplerQuality(rawValue: 2) == nil)
    }

    @Test("allCases covers both presets")
    func allCasesCovers() {
        #expect(Engine.ResamplerQuality.allCases.count == 2)
        #expect(Engine.ResamplerQuality.allCases.contains(.mastering))
        #expect(Engine.ResamplerQuality.allCases.contains(.highQuality))
    }
}

@Suite("Engine wrapper: resampler quality (live engine)")
struct EngineResamplerQualityTests {
    @Test("default is .mastering on a fresh engine")
    func defaultIsMastering() throws {
        let engine = try Engine()
        #expect(engine.resamplerQuality == .mastering)
    }

    @Test("setResamplerQuality round-trips through the bridge")
    func setResamplerQualityRoundTrips() throws {
        let engine = try Engine()
        try engine.setResamplerQuality(.highQuality)
        #expect(engine.resamplerQuality == .highQuality)
        try engine.setResamplerQuality(.mastering)
        #expect(engine.resamplerQuality == .mastering)
    }

    @Test("EngineStore mirrors Engine.resamplerQuality")
    @MainActor
    func storeMirrorsEngine() async throws {
        let store = try EngineStore()
        #expect(store.resamplerQuality == .mastering)
        store.setResamplerQuality(.highQuality)
        #expect(store.resamplerQuality == .highQuality)
    }
}
