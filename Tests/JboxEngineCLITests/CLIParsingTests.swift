import Testing
@testable import JboxEngineCLICore
import JboxEngineC
import JboxEngineSwift

// -----------------------------------------------------------------------------
// parseChannelList
// -----------------------------------------------------------------------------

@Suite("parseChannelList")
struct ParseChannelListTests {
    @Test("single channel converts 1-indexed → 0-indexed")
    func single() throws {
        #expect(try parseChannelList("1") == [0])
        #expect(try parseChannelList("7") == [6])
    }

    @Test("multi-channel list preserves order")
    func multi() throws {
        #expect(try parseChannelList("1,2,3") == [0, 1, 2])
        #expect(try parseChannelList("3,1,2") == [2, 0, 1])
    }

    @Test("whitespace around items is tolerated")
    func whitespace() throws {
        #expect(try parseChannelList(" 1 , 2 , 3 ") == [0, 1, 2])
    }

    @Test("empty string parses to empty array")
    func empty() throws {
        #expect(try parseChannelList("") == [])
    }

    @Test("empty subsequences are omitted (trailing/double commas)")
    func emptySubsequences() throws {
        #expect(try parseChannelList(",1,,2,") == [0, 1])
    }

    @Test("zero is rejected (channels are 1-indexed)")
    func zeroRejected() {
        #expect(throws: JboxError.self) { _ = try parseChannelList("0") }
        #expect(throws: JboxError.self) { _ = try parseChannelList("1,0,2") }
    }

    @Test("non-numeric input is rejected")
    func nonNumeric() {
        #expect(throws: JboxError.self) { _ = try parseChannelList("a") }
        #expect(throws: JboxError.self) { _ = try parseChannelList("1,b,3") }
    }

    @Test("negative values are rejected (UInt32 parse failure surfaces as INVALID_ARGUMENT)")
    func negativeRejected() {
        // `-1` doesn't parse as UInt32, so we get the "invalid channel" branch.
        #expect(throws: JboxError.self) { _ = try parseChannelList("-1") }
    }
}

// -----------------------------------------------------------------------------
// parseRouteSpec
// -----------------------------------------------------------------------------

@Suite("parseRouteSpec")
struct ParseRouteSpecTests {
    @Test("simple 2-channel spec round-trips into 0-indexed mapping")
    func simple() throws {
        let r = try parseRouteSpec("src@1,2->dst@3,4")
        #expect(r.sourceUID == "src")
        #expect(r.destUID == "dst")
        #expect(r.mapping == [ChannelEdge(src: 0, dst: 2),
                              ChannelEdge(src: 1, dst: 3)])
    }

    @Test("missing -> is rejected")
    func missingArrow() {
        #expect(throws: JboxError.self) { _ = try parseRouteSpec("src@1dst@1") }
    }

    @Test("missing @ on source side is rejected")
    func missingAtSrc() {
        #expect(throws: JboxError.self) { _ = try parseRouteSpec("src1->dst@1") }
    }

    @Test("missing @ on destination side is rejected")
    func missingAtDst() {
        #expect(throws: JboxError.self) { _ = try parseRouteSpec("src@1->dst1") }
    }

    @Test("UID containing @ is parsed using the last @ as the separator")
    func uidContainsAt() throws {
        // CoreAudio UIDs occasionally embed `@` (e.g., aggregate sub-device tags).
        let r = try parseRouteSpec("com.apple@vendor@1->dst@2")
        #expect(r.sourceUID == "com.apple@vendor")
        #expect(r.mapping == [ChannelEdge(src: 0, dst: 1)])
    }

    @Test("fan-out: same source channel mapped to multiple destinations")
    func fanOut() throws {
        let r = try parseRouteSpec("src@1,1->dst@3,4")
        #expect(r.mapping == [ChannelEdge(src: 0, dst: 2),
                              ChannelEdge(src: 0, dst: 3)])
    }

    @Test("mismatched source/destination channel counts are rejected")
    func mismatchedCounts() {
        let err = #expect(throws: JboxError.self) {
            _ = try parseRouteSpec("src@1,2->dst@1")
        }
        #expect(err?.code == JBOX_ERR_MAPPING_INVALID)
    }

    @Test("empty channel lists on both sides parse to an empty mapping")
    func emptyMapping() throws {
        // The route layer rejects empty mappings; parseRouteSpec itself
        // does not — it only checks count parity.
        let r = try parseRouteSpec("src@->dst@")
        #expect(r.mapping.isEmpty)
    }
}

// -----------------------------------------------------------------------------
// parseCLI
// -----------------------------------------------------------------------------

@Suite("parseCLI")
struct ParseCLITests {
    // The first arg is argv[0] (the program name) and is dropped by parseCLI.
    private let argv0 = "JboxEngineCLI"

    @Test("no args returns help")
    func empty() {
        switch parseCLI([argv0]) {
        case .success(.help): break
        default: Issue.record("expected .help")
        }
    }

    @Test("empty input array (not even argv0) returns help")
    func trulyEmpty() {
        // Defensive: dropFirst on an empty array is safe.
        switch parseCLI([]) {
        case .success(.help): break
        default: Issue.record("expected .help on empty argv")
        }
    }

    @Test("--help and -h both return help")
    func help() {
        for flag in ["--help", "-h"] {
            switch parseCLI([argv0, flag]) {
            case .success(.help): break
            default: Issue.record("expected .help for \(flag)")
            }
        }
    }

    @Test("--version and -V both return version")
    func version() {
        for flag in ["--version", "-V"] {
            switch parseCLI([argv0, flag]) {
            case .success(.version): break
            default: Issue.record("expected .version for \(flag)")
            }
        }
    }

    @Test("--list-devices and -l both return listDevices")
    func listDevices() {
        for flag in ["--list-devices", "-l"] {
            switch parseCLI([argv0, flag]) {
            case .success(.listDevices): break
            default: Issue.record("expected .listDevices for \(flag)")
            }
        }
    }

    @Test("--route with a valid spec returns runRoute")
    func routeValid() {
        switch parseCLI([argv0, "--route", "src@1,2->dst@3,4"]) {
        case .success(.runRoute(let r)):
            #expect(r.sourceUID == "src")
            #expect(r.destUID == "dst")
            #expect(r.mapping.count == 2)
        default:
            Issue.record("expected .runRoute")
        }
    }

    @Test("--route without a spec is rejected with INVALID_ARGUMENT")
    func routeMissingSpec() {
        switch parseCLI([argv0, "--route"]) {
        case .failure(let err):
            #expect(err.code == JBOX_ERR_INVALID_ARGUMENT)
        default:
            Issue.record("expected failure")
        }
    }

    @Test("--route with a malformed spec surfaces the parser's error")
    func routeMalformed() {
        switch parseCLI([argv0, "--route", "no-arrow-here"]) {
        case .failure(let err):
            #expect(err.code == JBOX_ERR_INVALID_ARGUMENT)
        default:
            Issue.record("expected failure")
        }
    }

    @Test("unknown flag is rejected with INVALID_ARGUMENT")
    func unknownFlag() {
        switch parseCLI([argv0, "--nope"]) {
        case .failure(let err):
            #expect(err.code == JBOX_ERR_INVALID_ARGUMENT)
        default:
            Issue.record("expected failure")
        }
    }
}
