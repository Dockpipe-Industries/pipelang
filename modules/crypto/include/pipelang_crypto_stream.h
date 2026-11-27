// SPDX-License-Identifier: Apache-2.0
#ifndef PIPELANG_CRYPTO_STREAM_H
#define PIPELANG_CRYPTO_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Additive stream ABI2. The independent ABI1 version/function remain unchanged.
 * This version function returns2, not the ABI1 version function's value1.
 */
uint32_t pipelang_crypto_stream_abi_version(void);

/* Status:0 success;1 invalid arguments;2 update chunk exceeds1MiB;
 * 3 caught panic/unavailable registry;4 owner limit;5 context limit;
 * 6 invalid/closed owner;7 invalid/closed/foreign context;8 sealed/failed context;
 * 9 running message limit;10 identifier generation exhausted.
 *
 * Fixed provider-wide bounds:16 live owners,64 live contexts including sealed
 * contexts. IDs are positive, <=INT64_MAX, never reused within this provider
 * instance. An owner belongs to one future native adapter, which must keep that
 * ID private and validate its source-visible context IDs. IDs are lifecycle
 * capabilities, not an authorization boundary against trusted native callers.
 * No context pointer or Rust allocation crosses this ABI.
 *
 * Calls are serialized internally. Native input/output addresses are trusted:
 * documented extents/alignment must be valid throughout the call. The ABI cannot
 * validate arbitrary pointers. No borrowed buffer is retained. Each output is
 * changed only on success. Output ranges within one call must not overlap.
 * Fatal process faults/allocator aborts are not recoverable status codes.
 */
int32_t pipelang_crypto_stream_v2_owner_create(uint64_t *owner);
int32_t pipelang_crypto_stream_v2_owner_close(uint64_t owner);

/* maximum_bytes may be0 and must be <=UINT64_MAX/8. Update checks its running
 * count before hashing; a running-limit failure poisons that context. Invalid
 * argument/chunk-size refusal does not consume input or mutate a context.
 * The caller selects its own admission limit (for example512MiB for a file).
 */
int32_t pipelang_crypto_stream_v2_begin(uint64_t owner, uint64_t maximum_bytes,
                                      uint64_t *context);
int32_t pipelang_crypto_stream_v2_update(uint64_t owner, uint64_t context,
                                       const unsigned char *input, size_t length);

/* Exactly32 output bytes plus one writable aligned uint64_t byte count.
 * Successful finish seals the context; update/repeated finish then fail.
 * Close is still required to free its bounded slot. A failed finish never
 * publishes a partial digest. Empty-input update may use NULL with length0.
 */
int32_t pipelang_crypto_stream_v2_finish(uint64_t owner, uint64_t context,
                                       unsigned char *output, size_t output_length,
                                       uint64_t *byte_count);

/* Close drops active/sealed/failed state. Immediately repeated close succeeds
 * while its slot's generation has not been reused. Reuse makes the stale ID
 * invalid. Owner close drops all its contexts; context calls then fail with6.
 * Any caught panic invalidates all live owners/contexts; a fresh owner may be
 * created. This deliberately refuses continuation after uncertain hash mutation.
 * ABI calls contain no cancellation scheduler. Future source adapters check
 * cancellation before/after bounded updates and close state before returning a
 * cancelled outcome, so an already-consumed chunk cannot be retried twice.
 */
int32_t pipelang_crypto_stream_v2_close(uint64_t owner, uint64_t context);

#ifdef __cplusplus
}
#endif
#endif
