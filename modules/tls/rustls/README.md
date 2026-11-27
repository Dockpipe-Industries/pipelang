# Maintained Rust TLS dependency — source candidate

This module builds upstream rustls-ffi 0.15.7 with AWS-LC through PipeLang's
installed native-dependency resolver. It contains no TLS protocol implementation
or replacement Rust binding. PipeServe owns socket/TLS connection interoperability
and HTTP hosting; ordinary application logic remains `.pipe`.

Run the declared `workflows/tls.build/config.yml` with an admitted DockPipe CLI,
PIPELANG_TLS_SDK_PREFIX, PIPELANG_NATIVE_RESOLVER_PREFIX, an explicit
PIPELANG_NATIVE_TOOLCHAIN and PIPELANG_NATIVE_TOOLCHAIN_SHA256, prepared offline
PIPELANG_NATIVE_CACHE, pinned PIPELANG_RUSTLS_ARCHIVE and a fresh absolute
PIPELANG_TLS_PROOF. The workflow verifies the upstream archive, extracts to the
owned proof directory, configures the resolver consumer, builds one worker under
existing containment, and runs an ABI/version/AWS-LC/randomness smoke fixture.
It installs the library, C header, Cargo.lock and root notices. Distribution also
requires all linked dependency notices; callers must preserve their verified
license inventory. Native artifact receipts identify toolchain, manifests, lock
and library digests. No dependency fetching, fallback provider or system installs.

This optional module does not make Rust a prerequisite of the PipeLang compiler.
Linux x86-64 is locally qualified; ARM/NAS, Windows/macOS, FIPS, installed services
and Cloudflare deployment are not established by this native dependency build.
