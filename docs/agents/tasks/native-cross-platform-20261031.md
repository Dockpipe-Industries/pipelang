# Native Windows/macOS compiler, SDK and workflows

Objective `pipelang-native-cross-platform-20261031` has completed native Windows,
Linux and macOS qualification. Authorized dev integration is in progress. Acceptance
requires actual native build/test/package execution and relocated installed-SDK
consumption on both operating systems, through DockPipe, with no Go/Python
repository or toolchain dependency. Linux acceptance in
[native-toolchain-20261030.md](native-toolchain-20261030.md) remains the baseline.
Cross-compilation, declarations and Wine do not establish native acceptance.

The owner authorized existing physical M6 execution through DockPipe, the
existing Windows VM and Linux VM/container execution. The M6 is a physical native
macOS ARM64 host. Once required qualification passes, the owner authorized
integration into `dev` through DockPipe's Git runtime. PR #8 targets `dev`; integration is pending its final merge.
No consumer edits, new paid infrastructure, credential changes, publication or
hosted dispatch are part of this objective. The owner subsequently authorized
visible startup of the existing Windows VM to log in and install its attached
Guest Tools. Preserve that desktop and installation state.

On 2026-10-31 the owner explicitly authorized distinct native platform policies:
Windows Job Object hard commit caps, and macOS process-tree monitoring with an
800 MiB stop. Linux limits remain unchanged. Native receipts must identify actual
guarantees; sampled accounting is not a kernel memory/thread limit, zero swap
policy or escape-prevention mechanism. macOS descendants observed before
reparenting can be cleaned up by verified process identity; descendants that
appear and reparent between samples can escape observation. Current zero swap
allocation does not prove swap prevention.

The macOS backend belongs to `src/native/containedexec/containment_macos.cpp`;
its independent probes belong to `probe_macos.cpp`. Bootstrap's shell monitor
signals only its verified private process group, never PIDs from global process
inventory. The initial bootstrap sampler incorrectly included unrelated parent
PIDs and killed the build; the corrected sampler and process-group signaling
replace that implementation. Earlier failed proof is retained.

## Current evidence

Proof root: /tmp/pipelang-native-cross-platform-20261031. All failed attempts are
retained. The qualified implementation was committed as `7520a0e` and pushed
through DockPipe Git runtime to PR #8. The current `dev` commit `01a7e60` was
merged into the source branch through the same runtime. Conflicts retained the
qualified native implementations and retired Go/Python files. The resulting
source tree exactly matches `7520a0e` (tree `b4cee57b6074cfbc5859753b01f179a3a6cd6238`);
only this evidence record changes afterward. All newer non-Go/Python `dev` files
are already present in that qualified tree. No SDK release was published.

- The receiver was clean at f10fcda7cdc0fb01a1707905fa24ed1ebc0f6268 on
  codex/pipelang-editor-staging. The authored implementation remains in that checkout; no
  worktree or consumer edit was made.
- Linux default build 10 and repository check 2 passed under the unchanged
  cgroup caps and native-only PATH. Full default test 2 passed 111 base cases,
  all 21 normal/ASan/UBSan runtime pairs and 175 contained units, including 77
  execution audits. Packaging 2 passed archive construction, independent reading,
  extraction and relocated SDK/CMake/CLI/runtime consumption in 26 units. All
  execution audits passed without Go/Python attempts. Current SDK is
  linux-build-10/sdk; the qualified archive is under linux-package-2/package.
- Native Windows x64 default build 13 passed compilation, installation and all
  16 build assertion units. Both bootstrap and rebuilt supervisors passed all
  fourteen policy probes, including actual oversized-commit refusal, cleanup,
  sustained junction accounting, non-following traversal, redirected-root
  refusal and the real-byte cap. The selected compiler is LLVM-MinGW Clang
  22.1.8 from its checksum-pinned archive; MSVC qualification is not claimed.
- Windows check 14 passed four workflow declarations, six PowerShell syntax
  checks and repository/toolchain hygiene. Full default test 14 passed all 111
  base cases, 21 normal/ASan/UBSan runtime pairs and 98 contained units. Leak
  sanitizer is explicitly disabled on Windows. SDK admission uses exact manifest
  hashes and native reparse attributes; link-refusal fixtures use owned,
  unprivileged junctions with scoped cleanup. Strict warnings remain enabled.
- Windows packaging 14 passed all 13 archive, independent-reader, extraction and
  relocated installed-SDK/CMake/legacy/runtime units. Test/package 14 reuse the
  already-qualified build-13 SDK through the declared existing-build input.
  Source comparison proved every producer input byte-identical: only this task
  record and an intentionally-unused fixture metadata annotation changed. The
  SDK manifest is pinned identically before/after testing and after relocation.
- The Windows archive and metadata were collected to the host, with exact size
  and SHA256 checked against the package manifest. Artifacts are
  windows-bridge-14/results/windows-native-sdk.tar and windows-sdk-package.json.
  Compact receipt audits are windows-qualified-summary.json and
  linux-qualified-summary.json. The completed loopback transfer listener was
  stopped; the owner's Windows desktop, VM overlay and Guest Tools state remain
  preserved. No system service, credentials, privileges or VM resources changed.
- The physical M6 is native macOS ARM64 (27.0.1, Apple Clang 21.0.0,
  CMake 4.4.4). A fresh current-source build passed all 16 build assertion units,
  source bootstrap and rebuilt sampled-policy admission. Full default tests passed
  111 base cases, all 21 normal/ASan/UBSan runtime pairs and 98 contained units.
  Native packaging passed all 13 archive, independent-reader, extraction and
  relocated installed-SDK/CMake/CLI/runtime units. All sampled cleanup receipts
  passed. The earlier companion/refusal and relocation failures did not recur in
  this fresh, source-verified build.
- Mac delivery excludes hidden hosted-CI metadata under DockPipe's source-delivery
  policy. Native source bytes were verified against an exact manifest, Unix shell
  syntax was checked on the M6, and the default build/test/release workflows ran
  there with a native-only PATH. Full repository/hosted-metadata checks remain
  covered by Linux and Windows; no Mac hosted-metadata check is claimed.
- Mac results were recovered with a separate bounded artifact-only delivery after
  the initial result selector named a directory rather than regular files. Native
  build/test/package statuses and every unit passed; the transport selector error
  is preserved. Proof and the checksummed Mac archive are under
  m6-final-evidence-1; macos-qualified-summary.json audits the receipts.
- The owner subsequently authorized a 12-hour remote authentication timeout.
  DockPipe's idle and absolute policy limits were implemented and tested in its
  owning repository, then applied through the supported policy API to the existing
  broker. Read-back reports 43200 seconds for each. This does not change PipeLang's
  pinned 0.6.33 producer or native resource policies.

The owner authorized autonomous continuation until 10:00 AM America/Toronto on
2026-10-31 (14:00 UTC). At that deadline, checkpoint safely and preserve evidence
and the existing VM. Incomplete qualification must remain incomplete; dev
integration is conditional on required acceptance.

Remote delivery owns a generated minimal configuration and refuses sender
project config files. Use an explicit task-owned source boundary and a clean
nested DockPipe environment. Source-only CLI bootstrap can accept an explicit
producer version when project config is absent; it still requires the committed
lock's exact version and archive hash/size. Never rename private files to evade
delivery checks. Keep remote transport client identity separate from build identity.

The owning DockPipe documentation currently describes `dockpipe.lock.json` as a
producer CLI checksum pin, not a package dependency selection lock. The queued
package-store alignment must verify the supported owning contract; do not present
this repository's metadata as an implemented package selection schema.
