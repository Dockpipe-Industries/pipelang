# PipeLang native dependency resolver — candidate 0.1.0

Optional installed CMake integration for maintained native dependencies. It builds
libraries with C ABIs for generated PipeLang C++; it does not generate Rust, Go or
C# from `.pipe`, translate managed object models, or add runtime tool dependencies
to the core compiler. Application/domain logic remains PipeLang.

Use `find_package(PipeLangNativeDependencies 0.1.0 EXACT CONFIG REQUIRED)` then
`pipelang_add_native_dependency(name LANGUAGE Rust|Go|CSharp|CMake ...)`.
The named imported target carries the library artifact and declared system links;
consumers link it using normal `target_link_libraries`. A build target always
rechecks inputs, including when the language's incremental compiler reuses work.

Common required arguments: absolute SOURCE_DIRECTORY, MANIFEST/MANIFEST_SHA256,
LOCK_FILE/LOCK_SHA256, TOOLCHAIN_FILE/TOOLCHAIN_SHA256, and LIBRARY_NAME.
Rust also requires PACKAGE and a separately prepared CACHE_DIRECTORY; DEFAULT_FEATURES
is opt-in, FEATURES and RUSTFLAGS are explicit. TESTS adds a Rust library-unit-test target.
Go uses the actual go.sum, an explicit cache and C compiler, cgo C exports and
c-archive output. CSharp uses the actual packages.lock.json, pinned RESTORE_ASSETS
and RESTORE_ASSETS_SHA256 from a separately completed locked restore, an explicit
NuGet cache, UnmanagedCallersOnly C exports, and NativeAOT shared output. CMake
supports C/C++ libraries through an explicit BUILD_TARGET, relative ARTIFACT and
pinned CMAKE_TOOLCHAIN/CMAKE_TOOLCHAIN_SHA256. All native APIs retain their own
ownership, ABI/versioning, errors and cancellation obligations.

Toolchain JSON schema 1 declares language, target `x86_64-unknown-linux-gnu`, a
trusted build PATH, and a tools object. Each named tool declares absolute path,
sha256, exact `version` output and one `version_argument`. Rust requires cargo,
rustc; Go requires go and cc; CSharp requires dotnet and ilc; CMake requires
cmake, cc, cxx and make. Extra declared tools are also verified. Tool executable,
manifest and lock digests are checked before and after each build; expected version
outputs must match. Build receipts identify the inputs and output digest.

Rust uses frozen/offline Cargo with one worker and explicit compiler paths. Go
uses readonly modules, no proxy/sum service/toolchain download, local Go, explicit
cgo compiler and one worker. C# selects an explicit toolchain runtime_identifier (linux-x64 or ubuntu.22.04-x64)
and publishes without restore; missing prepared assets
or NativeAOT packs fail. CMake disables FetchContent downloads; the selected project
is trusted native code. No resolver installs tools, upgrades locks, downloads
missing dependencies, selects latest or falls back to another profile.

This is initially Linux x86-64 only. Resolver/tool/lock checks do not sandbox native
build scripts or hash an entire compiler installation, sysroot, PATH or dependency
cache. Sources, caches, CMake toolchains and transitive platform tools must be
admitted by the caller; execute through existing workflow containment. A hash pin
is an integrity check, not publisher authentication. Dependency source archives
and their manifests/licenses remain the owning package's responsibility.

Java/JVM, JavaScript/TypeScript and Python require runtime/process interoperability;
they do not directly supply a native C ABI through these profiles. Unknown language
selections fail explicitly. Cross-platform builds and NativeAOT packs not present
locally remain unqualified; see the consumer's exact evidence.


The owning optional workflows are `crypto.build`, `tls.build` and
`native-dependencies.test`. The latter executes durable generated native caller
fixtures and checks lock drift after configuration, bad tool pins and unsupported
language refusal. A missing NativeAOT pack is a prerequisite failure, not a reason
to fetch implicitly or claim a successful C# library build.

C# explicitly uses the pinned `ilc` executable’s tools directory and single-threaded
NativeAOT compilation. Preparing a pack alone does not select it: SDK-inferred
compiler locations are not used by this profile.
