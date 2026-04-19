/*
 * jbox_engine.h — public C API for the Jbox audio engine.
 *
 * This header is the stable public contract between the engine and any
 * client (Swift UI layer, CLI, future alternative UIs). Changes here are
 * semver-governed:
 *   - Adding functions, enum values, or struct fields (appended) is MINOR.
 *   - Removing or renaming symbols, reordering fields, changing behavior
 *     is MAJOR.
 *
 * See docs/spec.md § 1.6 and § 2.11.
 */

#ifndef JBOX_ENGINE_H
#define JBOX_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time ABI version. Bump on breaking changes. */
#define JBOX_ENGINE_ABI_VERSION 1u

/* Runtime ABI version. */
uint32_t jbox_engine_abi_version(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* JBOX_ENGINE_H */
