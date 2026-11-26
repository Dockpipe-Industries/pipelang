// SPDX-License-Identifier: Apache-2.0
#ifndef PIPELANG_CRYPTO_H
#define PIPELANG_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t pipelang_crypto_abi_version(void);
/* ABI v1: caller-owned readable input and exactly32 writable output bytes.
 * No retention/allocation transfer. Input/output may overlap. Empty input may
 * be NULL. Return0 success;1 invalid arguments;2 input limit;3 caught panic.
 * Maximum input1MiB. Pointer validity remains the trusted native caller contract.
 */
int32_t pipelang_crypto_v1_sha256(const unsigned char *input, size_t length,
                                unsigned char *output, size_t output_length);
#ifdef __cplusplus
}
#endif
#endif
