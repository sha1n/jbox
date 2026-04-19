#!/usr/bin/env bash
#
# rt_safety_scan.sh — static scanner for real-time safety violations
# in the Jbox audio engine's real-time code path.
#
# Scans Sources/JboxEngineC/rt/ for banned symbols that would break
# the no-allocate / no-lock / no-syscall discipline required on the
# audio thread. Exits non-zero on any match.
#
# See docs/spec.md § 2.10 for the full set of rules. This scanner
# enforces the subset that can be checked syntactically; the rest
# (e.g., bounded execution time, absence of exceptions) is enforced
# through compile flags and code review.
#
# Limitations:
#   - This is a pattern-based scanner, not a real C++ parser. Comments
#     and strings can produce false positives. If a banned symbol
#     appears legitimately in a comment, rephrase the comment or move
#     the explanation out of rt/.
#   - Not all RT violations are detectable statically (e.g., hidden
#     allocations inside third-party APIs). Code review remains
#     required for any change to rt/.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RT_DIR="${ROOT_DIR}/Sources/JboxEngineC/rt"

if [[ ! -d "${RT_DIR}" ]]; then
  echo "rt_safety_scan: ${RT_DIR} not found — nothing to scan."
  exit 0
fi

# Banned patterns. ERE syntax (grep -E).
BANNED_PATTERNS=(
  # Heap allocation
  '\bnew\b'
  '\bmalloc\b'
  '\bcalloc\b'
  '\brealloc\b'
  '\bfree\b'

  # Locks
  '\bstd::mutex\b'
  '\bstd::lock_guard\b'
  '\bstd::unique_lock\b'
  '\bstd::shared_mutex\b'
  '\bstd::scoped_lock\b'
  '\bpthread_mutex_lock\b'
  '\bpthread_mutex_unlock\b'
  '\bpthread_rwlock_'

  # GCD / dispatch — not RT-safe
  '\bdispatch_async\b'
  '\bdispatch_sync\b'
  '\bdispatch_after\b'
  '\bdispatch_once\b'

  # Logging that may allocate or syscall
  '\bos_log_info\b'
  '\bos_log_debug\b'
  '\bos_log_error\b'
  '\bos_log_fault\b'
  '\bprintf\b'
  '\bfprintf\b'
  '\bsprintf\b'
  '\bsnprintf\b'
  '\bputs\b'

  # Smart pointers (construction may allocate)
  '\bstd::shared_ptr\b'
  '\bstd::make_shared\b'
  '\bstd::make_unique\b'
)

FOUND_VIOLATIONS=0
FILES_SCANNED=0

# Iterate source files using process substitution so that counters set
# inside the loop persist in the parent shell. `mapfile` is bash 4+ only;
# macOS ships bash 3.2 as /bin/bash, so we avoid it.
while IFS= read -r -d '' file; do
  FILES_SCANNED=$((FILES_SCANNED + 1))
  for pattern in "${BANNED_PATTERNS[@]}"; do
    if matches=$(grep -nE "${pattern}" "${file}" 2>/dev/null); then
      echo "RT-safety violation in ${file#"${ROOT_DIR}"/}:"
      printf '%s\n' "${matches}" | sed 's/^/  /'
      FOUND_VIOLATIONS=1
    fi
  done
done < <(find "${RT_DIR}" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' -o -name '*.c' \) -print0)

if [[ "${FILES_SCANNED}" -eq 0 ]]; then
  echo "rt_safety_scan: no source files in ${RT_DIR#"${ROOT_DIR}"/} yet."
  exit 0
fi

if [[ "${FOUND_VIOLATIONS}" -eq 0 ]]; then
  echo "rt_safety_scan: clean (${FILES_SCANNED} file(s) scanned)"
  exit 0
fi

echo ""
echo "rt_safety_scan: violations found. See docs/spec.md § 2.10 for rules."
exit 1
