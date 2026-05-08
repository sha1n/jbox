# Makefile — common build / test / release commands for Jbox.
#
# Thin wrapper over the shell scripts under scripts/. Every target
# ultimately invokes tools that write their output to git-ignored
# directories (.build/, build/, test-results/, .swiftpm/). There is
# no in-tree build output.
#
# Run `make` (no args) for a list of targets.

SHELL := /bin/bash

# -----------------------------------------------------------------------------
# Version
# -----------------------------------------------------------------------------

# Default version string: git-describe of the current HEAD. If no tags
# exist yet, falls back to a short commit hash; if not in a git repo at
# all, falls back to a literal placeholder.
#
# Override: `make VERSION=1.2.3 build`
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//' || echo 0.0.0-local)
export JBOX_VERSION := $(VERSION)

# -----------------------------------------------------------------------------
# Phony declarations — every target is a command, not a file.
# -----------------------------------------------------------------------------

.PHONY: help all clean build test \
        app dmg run cli verify \
        swift-test cxx-test cxx-test-tsan rt-scan coverage

# -----------------------------------------------------------------------------
# Default target — help.
# -----------------------------------------------------------------------------

help:
	@echo "Jbox — make targets"
	@echo ""
	@echo "Common:"
	@echo "  make clean          Remove all build artifacts (.build/, build/, test-results/, .swiftpm/)"
	@echo "  make build          Compile and produce the distributable DMG (Jbox-$(VERSION).dmg)"
	@echo "  make test           Run the full test pipeline (RT-scan + build + Swift + C++ + TSan)"
	@echo ""
	@echo "Composed:"
	@echo "  make all            clean + build + test"
	@echo ""
	@echo "Fine-grained:"
	@echo "  make app            Build Jbox.app bundle only (no DMG)"
	@echo "  make dmg            Build the distribution DMG (same as 'build')"
	@echo "  make run            Build, bundle, and launch Jbox.app"
	@echo "  make cli            Build JboxEngineCLI in release mode"
	@echo "  make verify         Run the full verification pipeline (same as 'test')"
	@echo "  make swift-test     Run Swift Testing tests only"
	@echo "  make cxx-test       Run C++ engine tests only (with per-test durations)"
	@echo "  make cxx-test-tsan  Run C++ engine tests under ThreadSanitizer"
	@echo "  make rt-scan        Run the RT-safety static scanner"
	@echo "  make coverage       Generate Swift + C++ lcov reports under test-results/"
	@echo ""
	@echo "Version: JBOX_VERSION=$(VERSION)"
	@echo "   Override with:  make VERSION=X.Y.Z <target>"

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------

clean:
	rm -rf .build .swiftpm build test-results

# -----------------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------------

# `build` is the primary distribution target — produces the DMG that
# Releases CI would also produce for the current HEAD.
build: dmg

app:
	./scripts/build_release.sh

dmg: app
	./scripts/package_unsigned_release.sh

run: app
	open ./build/Jbox.app

cli:
	swift build -c release --product JboxEngineCLI

# -----------------------------------------------------------------------------
# Test
# -----------------------------------------------------------------------------

# `test` is the full pipeline — same thing CI runs on every push.
# For quick iteration, see `swift-test` / `cxx-test` below.
test: verify

verify:
	./scripts/verify.sh

swift-test:
	swift test

cxx-test:
	swift run JboxEngineCxxTests --durations yes

cxx-test-tsan:
	swift run --sanitize=thread JboxEngineCxxTests

rt-scan:
	./scripts/rt_safety_scan.sh

coverage:
	./scripts/coverage.sh

# -----------------------------------------------------------------------------
# All
# -----------------------------------------------------------------------------

all: clean build test
