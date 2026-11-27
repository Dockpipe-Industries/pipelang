// SPDX-License-Identifier: Apache-2.0
//! Additive lifecycle ABI over the existing maintained sha2 dependency.
//! Native owners remain private to their adapters. Only positive generation IDs,
//! never unchecked Rust pointers, may become source-visible handles.

use sha2::{Digest, Sha256};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Mutex, OnceLock};

const MAX_OWNERS: usize = 16;
const MAX_CONTEXTS: usize = 64;
const MAX_CHUNK_BYTES: usize = 1_048_576;
const MAX_MESSAGE_BYTES: u64 = u64::MAX / 8;
const MAX_IDENTIFIER: u64 = i64::MAX as u64;

const SUCCESS: i32 = 0;
const INVALID_ARGUMENT: i32 = 1;
const CHUNK_LIMIT: i32 = 2;
const PANIC: i32 = 3;
const OWNER_LIMIT: i32 = 4;
const CONTEXT_LIMIT: i32 = 5;
const INVALID_OWNER: i32 = 6;
const INVALID_CONTEXT: i32 = 7;
const SEALED_CONTEXT: i32 = 8;
const MESSAGE_LIMIT: i32 = 9;
const GENERATION_LIMIT: i32 = 10;

struct OwnerSlot {
    generation: u64,
    live: bool,
}

struct ActiveDigest {
    hasher: Sha256,
    count: u64,
    maximum: u64,
}

enum StreamState {
    Active(ActiveDigest),
    Finalized,
    Failed,
}

struct ContextSlot {
    generation: u64,
    owner: u64,
    state: Option<StreamState>,
}

struct Registry {
    next_identifier: u64,
    owners: [OwnerSlot; MAX_OWNERS],
    contexts: [ContextSlot; MAX_CONTEXTS],
}

impl Registry {
    fn new() -> Self {
        Self {
            next_identifier: 1,
            owners: std::array::from_fn(|_| OwnerSlot {
                generation: 0,
                live: false,
            }),
            contexts: std::array::from_fn(|_| ContextSlot {
                generation: 0,
                owner: 0,
                state: None,
            }),
        }
    }

    fn identifier(&mut self) -> Result<u64, i32> {
        if self.next_identifier > MAX_IDENTIFIER {
            return Err(GENERATION_LIMIT);
        }

        let identifier = self.next_identifier;
        self.next_identifier += 1;
        Ok(identifier)
    }

    fn owner_index(&self, owner: u64) -> Result<usize, i32> {
        self.owners
            .iter()
            .position(|slot| owner != 0 && slot.generation == owner && slot.live)
            .ok_or(INVALID_OWNER)
    }

    fn context_index(&self, owner: u64, context: u64) -> Result<usize, i32> {
        self.owner_index(owner)?;

        self.contexts
            .iter()
            .position(|slot| context != 0 && slot.generation == context && slot.owner == owner)
            .ok_or(INVALID_CONTEXT)
    }

    fn invalidate_all(&mut self) {
        for context in &mut self.contexts {
            context.state = None;
        }
        for owner in &mut self.owners {
            owner.live = false;
        }
    }
}

static STREAMS: OnceLock<Mutex<Registry>> = OnceLock::new();

fn operation(function: impl FnOnce(&mut Registry) -> i32) -> i32 {
    let registry = STREAMS.get_or_init(|| Mutex::new(Registry::new()));
    let mut registry = match registry.lock() {
        Ok(registry) => registry,
        Err(_) => return PANIC,
    };

    // Catch while the guard remains outside the unwinding closure. A caught
    // panic invalidates retained state without poisoning this mutex, permitting
    // a fresh owner rather than continuing a possibly half-updated digest.
    match catch_unwind(AssertUnwindSafe(|| function(&mut registry))) {
        Ok(status) => status,
        Err(_) => {
            registry.invalidate_all();
            PANIC
        }
    }
}

fn checked_count(count: u64, length: u64, maximum: u64) -> Result<u64, i32> {
    let next = count.checked_add(length).ok_or(MESSAGE_LIMIT)?;
    if next > maximum || next > MAX_MESSAGE_BYTES {
        return Err(MESSAGE_LIMIT);
    }

    Ok(next)
}

#[no_mangle]
pub extern "C" fn pipelang_crypto_stream_abi_version() -> u32 {
    2
}

/// # Safety
/// `owner` must reference one aligned writable u64 throughout this call.
#[no_mangle]
pub unsafe extern "C" fn pipelang_crypto_stream_v2_owner_create(owner: *mut u64) -> i32 {
    if owner.is_null() {
        return INVALID_ARGUMENT;
    }

    operation(|registry| {
        let slot = match registry.owners.iter().position(|slot| !slot.live) {
            Some(slot) => slot,
            None => return OWNER_LIMIT,
        };
        let generation = match registry.identifier() {
            Ok(generation) => generation,
            Err(status) => return status,
        };

        registry.owners[slot] = OwnerSlot {
            generation,
            live: true,
        };
        unsafe { std::ptr::write(owner, generation) };
        SUCCESS
    })
}

#[no_mangle]
pub extern "C" fn pipelang_crypto_stream_v2_owner_close(owner: u64) -> i32 {
    operation(|registry| {
        let slot = match registry
            .owners
            .iter()
            .position(|slot| owner != 0 && slot.generation == owner)
        {
            Some(slot) => slot,
            None => return INVALID_OWNER,
        };

        for context in &mut registry.contexts {
            if context.owner == owner {
                context.state = None;
            }
        }
        registry.owners[slot].live = false;
        SUCCESS
    })
}

/// # Safety
/// `context` must reference one aligned writable u64 throughout this call.
#[no_mangle]
pub unsafe extern "C" fn pipelang_crypto_stream_v2_begin(
    owner: u64,
    maximum_bytes: u64,
    context: *mut u64,
) -> i32 {
    if context.is_null() || maximum_bytes > MAX_MESSAGE_BYTES {
        return INVALID_ARGUMENT;
    }

    operation(|registry| {
        if let Err(status) = registry.owner_index(owner) {
            return status;
        }
        let slot = match registry.contexts.iter().position(|slot| slot.state.is_none()) {
            Some(slot) => slot,
            None => return CONTEXT_LIMIT,
        };
        let generation = match registry.identifier() {
            Ok(generation) => generation,
            Err(status) => return status,
        };

        registry.contexts[slot] = ContextSlot {
            generation,
            owner,
            state: Some(StreamState::Active(ActiveDigest {
                hasher: Sha256::new(),
                count: 0,
                maximum: maximum_bytes,
            })),
        };
        unsafe { std::ptr::write(context, generation) };
        SUCCESS
    })
}

/// # Safety
/// Nonempty input must reference `length` readable bytes. Empty input may be
/// null. The buffer remains caller-owned and is not retained after this call.
#[no_mangle]
pub unsafe extern "C" fn pipelang_crypto_stream_v2_update(
    owner: u64,
    context: u64,
    input: *const u8,
    length: usize,
) -> i32 {
    if input.is_null() && length != 0 {
        return INVALID_ARGUMENT;
    }
    if length > MAX_CHUNK_BYTES {
        return CHUNK_LIMIT;
    }

    operation(|registry| {
        let slot = match registry.context_index(owner, context) {
            Ok(slot) => &mut registry.contexts[slot],
            Err(status) => return status,
        };
        let state = match slot.state.as_mut() {
            Some(StreamState::Active(state)) => state,
            Some(_) => return SEALED_CONTEXT,
            None => return INVALID_CONTEXT,
        };
        let next = match checked_count(state.count, length as u64, state.maximum) {
            Ok(next) => next,
            Err(status) => {
                slot.state = Some(StreamState::Failed);
                return status;
            }
        };
        let bytes = if length == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(input, length) }
        };

        state.hasher.update(bytes);
        state.count = next;
        SUCCESS
    })
}

/// # Safety
/// `output` must reference32 writable bytes and `byte_count` one aligned writable
/// u64. These ranges must not overlap. Neither range is retained after the call.
#[no_mangle]
pub unsafe extern "C" fn pipelang_crypto_stream_v2_finish(
    owner: u64,
    context: u64,
    output: *mut u8,
    output_length: usize,
    byte_count: *mut u64,
) -> i32 {
    if output.is_null() || output_length != 32 || byte_count.is_null() {
        return INVALID_ARGUMENT;
    }

    operation(|registry| {
        let slot = match registry.context_index(owner, context) {
            Ok(slot) => &mut registry.contexts[slot],
            Err(status) => return status,
        };
        let state = match slot.state.take() {
            Some(StreamState::Active(state)) => state,
            Some(state) => {
                slot.state = Some(state);
                return SEALED_CONTEXT;
            }
            None => return INVALID_CONTEXT,
        };

        let digest = state.hasher.finalize();
        slot.state = Some(StreamState::Finalized);
        unsafe {
            std::ptr::copy_nonoverlapping(digest.as_ptr(), output, digest.len());
            std::ptr::write(byte_count, state.count);
        }
        SUCCESS
    })
}

#[no_mangle]
pub extern "C" fn pipelang_crypto_stream_v2_close(owner: u64, context: u64) -> i32 {
    operation(|registry| {
        let slot = match registry.context_index(owner, context) {
            Ok(slot) => &mut registry.contexts[slot],
            Err(status) => return status,
        };

        slot.state = None;
        SUCCESS
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn owner() -> u64 {
        let mut owner = 0;
        assert_eq!(unsafe { pipelang_crypto_stream_v2_owner_create(&mut owner) }, SUCCESS);
        owner
    }

    fn begin(owner: u64, maximum: u64) -> u64 {
        let mut context = 0;
        assert_eq!(unsafe { pipelang_crypto_stream_v2_begin(owner, maximum, &mut context) }, SUCCESS);
        context
    }

    fn update(owner: u64, context: u64, bytes: &[u8]) -> i32 {
        unsafe { pipelang_crypto_stream_v2_update(owner, context, bytes.as_ptr(), bytes.len()) }
    }

    fn finish(owner: u64, context: u64) -> ([u8; 32], u64) {
        let mut digest = [0; 32];
        let mut count = 0;
        assert_eq!(unsafe {
            pipelang_crypto_stream_v2_finish(owner, context, digest.as_mut_ptr(), digest.len(), &mut count)
        }, SUCCESS);
        (digest, count)
    }

    fn hex(bytes: &[u8]) -> String {
        const DIGITS: &[u8; 16] = b"0123456789abcdef";
        let mut result = String::with_capacity(bytes.len() * 2);
        for byte in bytes {
            result.push(DIGITS[(byte >> 4) as usize] as char);
            result.push(DIGITS[(byte & 15) as usize] as char);
        }
        result
    }

    fn published_vectors_and_partitions() {
        let owner = owner();
        let vectors: &[(&[u8], &str)] = &[
            (b"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
            (b"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
            (b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        ];
        for (bytes, expected) in vectors {
            for split in 0..=bytes.len() {
                let context = begin(owner, bytes.len() as u64);
                assert_eq!(update(owner, context, &bytes[..split]), SUCCESS);
                assert_eq!(update(owner, context, &bytes[split..]), SUCCESS);
                let (digest, count) = finish(owner, context);
                assert_eq!(hex(&digest), *expected);
                assert_eq!(count, bytes.len() as u64);
                assert_eq!(pipelang_crypto_stream_v2_close(owner, context), SUCCESS);
            }
        }

        let context = begin(owner, 1_000_000);
        let block = [b'a'; 1000];
        for _ in 0..1000 {
            assert_eq!(update(owner, context, &block), SUCCESS);
        }
        let (digest, count) = finish(owner, context);
        assert_eq!(count, 1_000_000);
        assert_eq!(hex(&digest), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        assert_eq!(pipelang_crypto_stream_v2_owner_close(owner), SUCCESS);
    }

    fn bounds_and_unchanged_outputs() {
        let owner = owner();
        let context = begin(owner, 3);
        assert_eq!(update(owner, context, b"abc"), SUCCESS);
        assert_eq!(update(owner, context, b"x"), MESSAGE_LIMIT);
        assert_eq!(update(owner, context, b""), SEALED_CONTEXT);
        let mut output = [17; 32];
        let mut count = 91;
        assert_eq!(unsafe {
            pipelang_crypto_stream_v2_finish(owner, context, output.as_mut_ptr(), 32, &mut count)
        }, SEALED_CONTEXT);
        assert_eq!(output, [17; 32]);
        assert_eq!(count, 91);
        assert_eq!(pipelang_crypto_stream_v2_close(owner, context), SUCCESS);

        let context = begin(owner, MAX_CHUNK_BYTES as u64);
        let block = vec![0xa5; MAX_CHUNK_BYTES + 1];
        assert_eq!(unsafe {
            pipelang_crypto_stream_v2_update(owner, context, block.as_ptr(), MAX_CHUNK_BYTES + 1)
        }, CHUNK_LIMIT);
        assert_eq!(unsafe {
            pipelang_crypto_stream_v2_update(owner, context, std::ptr::null(), 1)
        }, INVALID_ARGUMENT);
        assert_eq!(update(owner, context, &block[..MAX_CHUNK_BYTES]), SUCCESS);
        assert_eq!(unsafe {
            pipelang_crypto_stream_v2_finish(owner, context, output.as_mut_ptr(), 31, &mut count)
        }, INVALID_ARGUMENT);
        assert_eq!(output, [17; 32]);
        assert_eq!(count, 91);
        assert_eq!(unsafe {
            pipelang_crypto_stream_v2_finish(owner, context, std::ptr::null_mut(), 32, &mut count)
        }, INVALID_ARGUMENT);
        assert_eq!(unsafe {
            pipelang_crypto_stream_v2_finish(owner, context, output.as_mut_ptr(), 32, std::ptr::null_mut())
        }, INVALID_ARGUMENT);
        assert_eq!(output, [17; 32]);
        assert_eq!(count, 91);
        let (digest, bytes) = finish(owner, context);
        let mut original = [0; 32];
        assert_eq!(unsafe {
            crate::pipelang_crypto_v1_sha256(block.as_ptr(), MAX_CHUNK_BYTES, original.as_mut_ptr(), 32)
        }, SUCCESS);
        assert_eq!(digest, original);
        assert_eq!(bytes, MAX_CHUNK_BYTES as u64);
        assert_eq!(update(owner, context, b""), SEALED_CONTEXT);

        let mut sentinel = 73;
        assert_eq!(unsafe {
            pipelang_crypto_stream_v2_begin(owner, MAX_MESSAGE_BYTES + 1, &mut sentinel)
        }, INVALID_ARGUMENT);
        assert_eq!(sentinel, 73);
        assert_eq!(unsafe { pipelang_crypto_stream_v2_owner_create(std::ptr::null_mut()) }, INVALID_ARGUMENT);
        assert_eq!(unsafe { pipelang_crypto_stream_v2_begin(owner, 0, std::ptr::null_mut()) }, INVALID_ARGUMENT);
        assert_eq!(checked_count(u64::MAX, 1, u64::MAX), Err(MESSAGE_LIMIT));
        assert_eq!(checked_count(MAX_MESSAGE_BYTES, 1, u64::MAX), Err(MESSAGE_LIMIT));
        assert_eq!(checked_count(MAX_MESSAGE_BYTES - 1, 1, MAX_MESSAGE_BYTES), Ok(MAX_MESSAGE_BYTES));
        assert_eq!(pipelang_crypto_stream_v2_owner_close(owner), SUCCESS);
    }

    fn ownership_generations_and_capacity() {
        assert_eq!(pipelang_crypto_stream_v2_owner_close(0), INVALID_OWNER);
        let first = owner();
        let second = owner();
        let context = begin(first, 3);
        assert_eq!(update(second, context, b"x"), INVALID_CONTEXT);
        assert_eq!(pipelang_crypto_stream_v2_close(second, context), INVALID_CONTEXT);
        assert_eq!(pipelang_crypto_stream_v2_close(first, context), SUCCESS);
        assert_eq!(pipelang_crypto_stream_v2_close(first, context), SUCCESS);
        assert_eq!(update(first, context, b"x"), INVALID_CONTEXT);
        let replacement = begin(first, 3);
        assert_ne!(replacement, context);
        assert_eq!(pipelang_crypto_stream_v2_close(first, context), INVALID_CONTEXT);
        assert_eq!(pipelang_crypto_stream_v2_owner_close(first), SUCCESS);
        assert_eq!(pipelang_crypto_stream_v2_owner_close(first), SUCCESS);
        assert_eq!(update(first, replacement, b"x"), INVALID_OWNER);
        assert_eq!(pipelang_crypto_stream_v2_owner_close(second), SUCCESS);

        let mut owners = Vec::new();
        for _ in 0..MAX_OWNERS {
            owners.push(owner());
        }
        let mut sentinel = 97;
        assert_eq!(unsafe { pipelang_crypto_stream_v2_owner_create(&mut sentinel) }, OWNER_LIMIT);
        assert_eq!(sentinel, 97);
        let owner = owners[0];
        let mut contexts = Vec::new();
        for _ in 0..MAX_CONTEXTS {
            contexts.push(begin(owner, 0));
        }
        assert_eq!(unsafe { pipelang_crypto_stream_v2_begin(owner, 0, &mut sentinel) }, CONTEXT_LIMIT);
        assert_eq!(sentinel, 97);
        finish(owner, contexts[0]);
        assert_eq!(unsafe { pipelang_crypto_stream_v2_begin(owner, 0, &mut sentinel) }, CONTEXT_LIMIT);
        assert_eq!(pipelang_crypto_stream_v2_close(owner, contexts[0]), SUCCESS);
        let replacement = begin(owner, 0);
        assert_ne!(replacement, contexts[0]);
        for owner in owners {
            assert_eq!(pipelang_crypto_stream_v2_owner_close(owner), SUCCESS);
        }

        let mut registry = Registry::new();
        registry.next_identifier = MAX_IDENTIFIER;
        assert_eq!(registry.identifier(), Ok(MAX_IDENTIFIER));
        assert_eq!(registry.identifier(), Err(GENERATION_LIMIT));
    }

    fn evidence_sized_running_count() {
        let owner = owner();
        let maximum = 512 * 1024 * 1024;
        let context = begin(owner, maximum);
        let block = vec![0x5a; MAX_CHUNK_BYTES];
        let mut reference = Sha256::new();
        for _ in 0..512 {
            assert_eq!(update(owner, context, &block), SUCCESS);
            reference.update(&block);
        }
        let (digest, count) = finish(owner, context);
        assert_eq!(count, maximum);
        let expected = reference.finalize();
        assert_eq!(&digest[..], &expected[..]);
        assert_eq!(pipelang_crypto_stream_v2_owner_close(owner), SUCCESS);
    }

    fn panic_cleanup_and_recovery() {
        let original_owner = owner();
        let context = begin(original_owner, 1);
        assert_eq!(operation(|_| panic!("injected stream failure")), PANIC);
        assert_eq!(update(original_owner, context, b"x"), INVALID_OWNER);
        let fresh = owner();
        let context = begin(fresh, 0);
        finish(fresh, context);
        assert_eq!(pipelang_crypto_stream_v2_owner_close(fresh), SUCCESS);
    }

    // Keep registry-mutating ABI witnesses in one test so Rust's parallel test
    // runner cannot invalidate another witness's owners during fault injection.
    #[test]
    fn additive_stream_abi_contracts() {
        assert_eq!(crate::pipelang_crypto_abi_version(), 1);
        assert_eq!(pipelang_crypto_stream_abi_version(), 2);
        published_vectors_and_partitions();
        bounds_and_unchanged_outputs();
        ownership_generations_and_capacity();
        evidence_sized_running_count();
        panic_cleanup_and_recovery();
    }
}
