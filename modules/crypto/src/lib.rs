// SPDX-License-Identifier: Apache-2.0
//! Small versioned C ABI over maintained SHA-256, not a cryptographic implementation.
//! Native callers own all buffers for the duration of the call. Ordinary PipeLang
//! values never carry these pointers. Invalid addresses remain a trusted caller
//! contract; pointer/length checks cannot prove arbitrary foreign memory is valid.

mod stream;

use sha2::{Digest, Sha256};
use std::panic::{catch_unwind, AssertUnwindSafe};

const MAX_INPUT_BYTES: usize = 1_048_576;

#[no_mangle]
pub extern "C" fn pipelang_crypto_abi_version() -> u32 {
    1
}

/// Hashes at most 1 MiB into an exactly 32-byte caller-owned output.
/// Returns 0=success, 1=invalid pointer/length, 2=input limit, 3=caught panic.
///
/// # Safety
/// Nonempty input must reference `length` readable bytes. Output must reference
/// 32 writable bytes. The ranges may overlap: the digest is fully owned before
/// the output is written. Neither buffer is retained or freed by this library.
#[no_mangle]
pub unsafe extern "C" fn pipelang_crypto_v1_sha256(
    input: *const u8,
    length: usize,
    output: *mut u8,
    output_length: usize,
) -> i32 {
    if output.is_null() || output_length != 32 || (input.is_null() && length != 0) {
        return 1;
    }
    if length > MAX_INPUT_BYTES {
        return 2;
    }
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        let bytes = if length == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(input, length)
        };
        let digest = Sha256::digest(bytes);
        std::ptr::copy_nonoverlapping(digest.as_ptr(), output, digest.len());
    }));
    if outcome.is_ok() {
        0
    } else {
        3
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn published_sha256_vectors_and_ownership() {
        let mut output = [0u8; 32];
        assert_eq!(unsafe { pipelang_crypto_v1_sha256(std::ptr::null(), 0, output.as_mut_ptr(), 32) }, 0);
        assert_eq!(output, [0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
                            0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55]);
        let mut overlapping = [0u8; 32];
        overlapping[..3].copy_from_slice(b"abc");
        assert_eq!(unsafe { pipelang_crypto_v1_sha256(overlapping.as_ptr(), 3, overlapping.as_mut_ptr(), 32) }, 0);
        assert_eq!(overlapping, [0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                                 0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad]);
        assert_eq!(pipelang_crypto_abi_version(), 1);
    }

    #[test]
    fn invalid_arguments_do_not_change_output() {
        let mut output = [17u8; 32];
        assert_eq!(unsafe { pipelang_crypto_v1_sha256(std::ptr::null(), 1, output.as_mut_ptr(), 32) }, 1);
        assert_eq!(unsafe { pipelang_crypto_v1_sha256(b"x".as_ptr(), 1, std::ptr::null_mut(), 32) }, 1);
        assert_eq!(unsafe { pipelang_crypto_v1_sha256(b"x".as_ptr(), 1, output.as_mut_ptr(), 31) }, 1);
        assert_eq!(unsafe { pipelang_crypto_v1_sha256(b"x".as_ptr(), MAX_INPUT_BYTES + 1, output.as_mut_ptr(), 32) }, 2);
        assert_eq!(output, [17u8; 32]);
    }
}
