# Validation

Run the default workflows through the exact released DockPipe admitted by
`dockpipe.config.json` and `dockpipe.lock.json`, with `DOCKPIPE_BIN` absolute:

```sh
"$DOCKPIPE_BIN" --workflow-file workflows/build/config.yml
"$DOCKPIPE_BIN" --workflow-file workflows/check/config.yml
"$DOCKPIPE_BIN" --workflow-file workflows/test/config.yml
"$DOCKPIPE_BIN" --workflow-file workflows/release/config.yml
```

There is no Go/Python fallback or opt-in native lane. Build compiles the C++
SDK, command line tools and C++ assertions. Check rejects Go/Python sources,
bytecode and manifests, interpreter/toolchain commands in workflow/CMake scripts,
unpinned CI actions and toolchain-lock drift. All remaining declared workflows
are validated. Tests run compiler oracles, generated runtime fixtures and
ASan/UBSan counterparts. Packaging verifies the installed manifest, writes a
bounded deterministic archive, extracts by exact checksum, and qualifies relocated
tools and an installed CMake consumer. Publication is separate.

On Linux, the first build bootstraps only the C++ containment runner/probe
inside one verified systemd unit. It checks memory.max=1GiB, memory.high=700MiB, no swap and
128 tasks, with an 800MiB proactive stop. The probe then exercises nine failure
contracts before the runner controls full builds. A qualified absolute runner
and its SHA256, or a checksum-pinned native SDK seed archive, may be supplied
instead. These are optional inputs, not dependencies on a historical SDK.

Every Linux full job retains the existing aggregate cap of 2GiB, memory.high=1536MiB,
zero swap and 384 tasks; workload units retain the above 1GiB/800MiB limits.
The proof estate cap remains 8GiB. Accounting does not follow symbolic/reparse
entries. Windows counts their own allocation and refuses redirected estate roots;
SDK/package/source admission separately rejects reparse inputs. The cap covers the
selected proof directory, not every filesystem write by a workload. Windows
receipts explicitly report that filesystem write isolation is not provided.
Heavy compilation is sequential. Failed and
partial proof stays available. Do not restart the old large research campaign.

Use fresh absolute paths outside the checkout for
`PIPELANG_NATIVE_TOOLCHAIN_PROOF`, `PIPELANG_NATIVE_COMPAT_PROOF`,
`PIPELANG_NATIVE_CHECK_PROOF` and `PIPELANG_NATIVE_PACKAGE_PROOF` when explicit
proof ownership is needed. Otherwise the wrappers allocate temporary roots.
`PIPELANG_NATIVE_BUILD_DIRECTORY` selects an explicit build cache. Default
Unix build success updates ignored `bin/native-current`; packaging updates
`bin/native-package`. Windows build success records its SDK/proof paths in ignored
`bin/native-current.json`. Set `PIPELANG_NATIVE_TOOL_PATH` to a native-only tool path
for a restricted-environment proof on Unix. Linux execution traces are audited
in C++. Its sanitizer runtime units are untraced because LeakSanitizer refuses
ptrace; their normal counterparts and compilation are traced. Native macOS and
Windows use explicit compiler/tool paths and native-only producer PATHs; this
does not claim Linux syscall tracing on those platforms.

Focused suite/runtime selectors provide subset evidence only. Default tests
include all declared modern suites, C++ source/runtime fixtures, inheritance and
view/style oracles. An exact prior containment receipt may be reused via
`PIPELANG_NATIVE_CONTAINMENT_ADMISSION` and its SHA256; otherwise the probe reruns.
Frozen v0.0.0.1 inputs/digests remain unchanged. Retired historical profiles/APIs
are not support promises and do not need executable replicas.

Local Linux/headless/installed-SDK proof does not establish Windows/macOS,
native interactive UI, hosted CI, signed release or publication acceptance.
The [migration task](../tasks/native-toolchain-20261030.md) owns current evidence.

Windows/macOS qualification is in progress in the
[cross-platform task](../tasks/native-cross-platform-20261031.md). The owner
explicitly authorized distinct native platform policies while retaining Linux's
limits. macOS sampled receipts identify their lack of kernel hard memory/thread,
zero-swap and escape-prevention guarantees; they never inherit Linux acceptance.
The physical M6 is native macOS evidence. Windows uses the existing guest VM.

The root workflow commands dispatch through CMake to the existing Unix scripts
or workflow-owned Windows PowerShell scripts. Windows selects the pinned native
LLVM-MinGW x64 UCRT toolchain, CMake and Ninja archives in the build workflow's
windows-toolchain.json; it does not require a system compiler installation.
Optional debugger/Python content in the upstream tool archive is not on the
producer PATH or redistributed in the installed SDK. Static runtime notices
and their upstream provenance are included in Windows SDK manifests/packages.

Windows source bootstrap admits suspended processes to a Job Object before
resuming them. Workload commit caps are 1 GiB with an 800 MiB proactive stop;
aggregate jobs use 2 GiB with an 1800 MiB stop. Active-process caps are 128/384,
with separately sampled thread stops at the same counts. Commit limits do not
prevent swap. Independent native probes must pass before these source changes
establish Windows acceptance. Windows ASan/UBSan and macOS sanitizer runs disable
LeakSanitizer; they do not claim Linux leak-detection coverage.

Source-only qualification transfers verify every file's bytes before normalizing
the temporary checkout's modification times to receiver UTC. ZIP DOS timestamps
can otherwise put CMake inputs in the future across time zones and keep Ninja's
build manifest dirty. This changes task-owned checkout metadata only. An optional
PIPELANG_NATIVE_STOPWATCH_DEADLINE bounds Windows bootstrap/job/unit execution
using an inherited monotonic deadline; the controller owns its derivation from
the user's requested stop time.
