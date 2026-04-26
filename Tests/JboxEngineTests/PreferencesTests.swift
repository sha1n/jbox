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
