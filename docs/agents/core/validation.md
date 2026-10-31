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

The first build bootstraps only the C++ containment runner/probe inside one
verified systemd unit. It checks memory.max=1GiB, memory.high=700MiB, no swap and
128 tasks, with an 800MiB proactive stop. The probe then exercises nine failure
contracts before the runner controls full builds. A qualified absolute runner
and its SHA256, or a checksum-pinned native SDK seed archive, may be supplied
instead. These are optional inputs, not dependencies on a historical SDK.

Every full job retains the existing aggregate cap of 2GiB, memory.high=1536MiB,
zero swap and 384 tasks; workload units retain the above 1GiB/800MiB limits.
The proof estate cap remains 8GiB. Heavy compilation is sequential. Failed and
partial proof stays available. Do not restart the old large research campaign.

Use fresh absolute paths outside the checkout for
`PIPELANG_NATIVE_TOOLCHAIN_PROOF`, `PIPELANG_NATIVE_COMPAT_PROOF`,
`PIPELANG_NATIVE_CHECK_PROOF` and `PIPELANG_NATIVE_PACKAGE_PROOF` when explicit
proof ownership is needed. Otherwise the wrappers allocate temporary roots.
`PIPELANG_NATIVE_BUILD_DIRECTORY` selects an explicit build cache. Default
build success updates ignored `bin/native-current`; packaging updates
`bin/native-package`. Set `PIPELANG_NATIVE_TOOL_PATH` to a native-only tool path
for a restricted-environment proof. Execution traces are audited in C++.
Sanitizer runtime units are untraced because LeakSanitizer refuses ptrace;
their normal counterparts and compilation are traced.

Focused suite/runtime selectors provide subset evidence only. Default tests
include all declared modern suites, C++ source/runtime fixtures, inheritance and
view/style oracles. An exact prior containment receipt may be reused via
`PIPELANG_NATIVE_CONTAINMENT_ADMISSION` and its SHA256; otherwise the probe reruns.
Frozen v0.0.0.1 inputs/digests remain unchanged. Retired historical profiles/APIs
are not support promises and do not need executable replicas.

Local Linux/headless/installed-SDK proof does not establish Windows/macOS,
native interactive UI, hosted CI, signed release or publication acceptance.
The [migration task](../tasks/native-toolchain-20261030.md) owns current evidence.
