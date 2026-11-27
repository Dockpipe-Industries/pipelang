# Optional PipeLang.Crypto stream package 0.1.0

This independent Linux x86_64 package supplies the `PipeLang.Crypto` source
library and `PipeLang::CryptoStream` native adapter target. It consumes an
explicitly provisioned maintained `sha2=0.10.9` stream ABI2 artifact from the
existing crypto owner. It does not build or acquire that dependency. The original
crypto ABI1, consumers, pins and core SDK installation remain separate.

The source interface is `Sha256Streams`. `BeginSha256(maximumBytes)` returns an
opaque positive `int` owned by that provider instance. Maximum bytes may be zero
and must be at most 2,305,843,009,213,693,951 (the SHA256 bit-count ceiling).
`UpdateSha256(hash, bytes, token)` consumes a bounded ByteBuffer without retaining
it. Chunks remain at most 1 MiB. `FinishSha256(hash, token)` produces exact signed
`byteCount` and 64 lowercase hex characters, then seals the context. Close remains
required. Negative limits and handles are checked before unsigned conversion.

The header-only `StreamAdapter<Contract,CryptoFailure,Sha256Digest>` implements
actual generated host interfaces, with typed Success/Failure/Cancelled results.
The adapter holds one private maintained-provider owner, a mutex and 64 fixed
local slots. Live/sealed slots remain bounded by the provider-wide 64-context and
16-owner caps. Copy/move are forbidden. Destruction closes the provider owner,
including every active/sealed/failed context. Native callers must retain the
adapter until outstanding calls complete; destruction concurrent with use is not
an admitted lifecycle. Tokens are method arguments and are never stored.

Source handles are globally unique provider generations, validated against the
adapter's own ledger before FFI. Foreign, negative and stale identities cannot
access or release another adapter's state. Closed local tombstones make repeated
Close successful even after another adapter reuses the provider's tombstone.
This guarantee lasts until that local slot is replaced by a later successful
Begin. Then the old identity is stale and Close returns Failure. There is no
unbounded closed-handle history.

An admitted active update/finish failure or cancellation closes the context and
retains only its local closed tombstone. Cancellation is checked before and after
bounded provider operations, and before returning the encoded digest. A cancelled
update may already have consumed bytes; its handle cannot be retried or finalized.
Begin a fresh stream if the application chooses to retry the whole input. Invalid
or sealed handle calls are refused before provider work. A caught provider panic
or invalid owner disables the adapter rather than silently replacing its owner.

`CryptoFailure.code` maps expected boundary failures to `crypto.invalid_limit`,
`crypto.invalid_context`, `crypto.message_limit`, `crypto.chunk_limit`,
`crypto.context_limit`, `crypto.owner_limit`, `crypto.invalid_argument`,
`crypto.invalid_state`, `crypto.invalid_owner`, `crypto.identifier_limit`,
`crypto.provider_unavailable`, `crypto.abi_version`, `crypto.provider_failure`,
`crypto.invalid_provider_output`, `crypto.memory_limit`, or `crypto.cancelled`.
Unexpected native exceptions retain the ordinary generated host boundary.

Configure this module with an exact installed `PipeLangModel` SDK and these
explicit inputs: `PIPELANG_CORE_MANIFEST_SHA256`,
`PIPELANG_CRYPTO_PROVIDER_PREFIX`, `PIPELANG_CRYPTO_PROVIDER_LIBRARY_SHA256`,
`PIPELANG_CRYPTO_PROVIDER_HEADER_SHA256`, `PIPELANG_CRYPTO_PROVIDER_LOCK_SHA256`,
`PIPELANG_CRYPTO_PROVIDER_RECEIPT`, and `PIPELANG_CRYPTO_PROVIDER_RECEIPT_SHA256`.
The receipt's artifact/lock/target must match the selected inputs. CMake performs
no downloads or automatic installation. Choose an independent install prefix
outside the core SDK. The optional package owns a separate SDK-tool manifest over
its include, lib and package-share roots; the core SDK's 16 roots are unchanged.

A relocated consumer supplies that manifest's exact
`PIPELANG_CRYPTO_STREAM_MANIFEST_SHA256` plus its selected core SDK manifest pin.
Package loading verifies both inventories before source generation. The exported
`PipeLangCryptoStream_SOURCE_LIBRARY_DIRECTORY` and
`PipeLangCryptoStream_SOURCE_LIBRARY_SHA256` identify the installed descriptor.
The descriptor pins every source file. This package does not add a default core
source library or change an existing application consumer.

`tests/crypto-stream` is the dedicated installed consumer. Source owns assertions
for known answers, partitions, binary bytes, exact/over limits, lifecycle/custody,
64-context admission, destructor reclaim, cancellation and cleanup. Native code
supplies bounded bytes and actual providers/tokens. Linux linker wrappers always
delegate real ABI2 Update/Finish once and can cancel the actual independent probe
token immediately after the selected provider returns. Source selects that effect
and checks its typed result, invocation counts and consumed-byte retry refusal.
No test hook or fake provider enters the production adapter.

The consumer explicitly selects the existing finite 33,554,432 evaluation-step
profile for the one complete 16-case invocation. Initialization and cleanup keep
their 65,536 defaults. This accommodates every split of the short known vectors,
two partitions of 512 MiB, and the fixed lifetime/cancellation campaign in one
Source invocation. It adds no Source pumping, hidden batches, global limits or
workload-containment changes. This fixture profile is not acceptance for arbitrary
input sizes or schedules.

The 512 MiB ASCII-a fixture retains bounded chunks and verifies exact counts and
both independently partitioned streams against a hardcoded Source expected hash.
That answer was independently obtained using maintained OpenSSL SHA256 over exactly
536,870,912 streamed ASCII-a bytes under the existing containment limits, with
explicit OpenSSL/libcrypto identities. The native fixture never supplies expected
hashes. This is a separate local cryptographic oracle, not standards certification.
Published empty/abc/56-byte/112-byte/million-a vectors supply additional known
answers; the explicit binary vector is bytes 0 through 255 with its source-pinned
expected hex. Cancellation cases also refill all 64 slots after each cancelled
stream to verify actual provider reclaim beyond local stale-handle refusal.

Status: locally qualified on Linux x86_64. All sixteen Source-owned cases pass
normally, with generated code/adapter/native fixture under C++ ASan/UBSan/leak
checks, and after unchanged optional-package relocation with installed-only
headers, source libraries and provider archive. Twelve explicit identity and
install-alias refusals, repeated package loading and unchanged core SDK checks
also pass. The package remains uncommitted and unpublished; no active consumer
pin or core SDK default changed. These results cover this finite fixture profile,
not arbitrary inputs or hostile install-directory races.
C++ sanitizers do not instrument maintained Rust internals. Windows CNG, native
Windows/macOS, I/O, output, serialization and application policy remain outside
this Linux-only package. The selected existing native artifact lacks its complete
upstream license/notice bundle; retain its exact receipt and Cargo.lock for local
qualification, and complete redistributable upstream notices before publication.
Artifact hash pins establish selected-byte integrity, not publisher authenticity.
