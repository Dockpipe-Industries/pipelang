# Debug a native PipeLang executable

[Select.pipe](Select.pipe) is authored entirely in PipeLang. The experimental
object compiler checks it through the ordinary project frontend, lowers the
selected method to a typed scalar IR, and uses LLVM to write an object with
PipeLang line and variable information. The Linux x86-64 proof workflow links that
object with a small Linux entry fixture and checks its result. Application
compilation and linking require LLVM and the native linker.

## Build the proof

From the repository root, use the repository-pinned DockPipe and an existing
SDK CMake build directory. Set these variables to their absolute paths:

```sh
export DOCKPIPE_BIN=/absolute/path/to/dockpipe
export PIPELANG_NATIVE_DEBUG_BUILD=/absolute/path/to/sdk-build
export PIPELANG_NATIVE_DEBUG_RUNNER=/absolute/path/to/qualified/pipelang-contained
export PIPELANG_NATIVE_DEBUG_RUNNER_SHA256="$(sha256sum "$PIPELANG_NATIVE_DEBUG_RUNNER" | cut -d ' ' -f 1)"
export PIPELANG_NATIVE_DEBUG_LLC=/absolute/path/to/llvm/bin/llc
export PIPELANG_NATIVE_DEBUG_GDB=/absolute/path/to/gdb

proof_parent=$(mktemp -d)
export PIPELANG_NATIVE_DEBUG_PROOF="$proof_parent/proof"
"$DOCKPIPE_BIN" --workflow-file workflows/native.debug/config.yml
```

Run the [SDK build workflow](../../workflows/build/config.yml) first if no
build directory or qualified containment runner exists. Building the compiler
itself uses the project's C++ toolchain. The object compilation proof traces
the PipeLang driver and LLVM and verifies that those are the only executables
started for that phase.

The workflow also verifies objects for six targets using the `llvm-readobj` and
`llvm-dwarfdump` tools next to `llc`. The compiler defaults to its host target.
Use `--target` to select another destination explicitly:

```sh
"$PIPELANG_NATIVE_DEBUG_BUILD/sdk-tools/pipelang-objectc" \
  --source examples/native/Select.pipe --entry Select.Selection.Select \
  --llc "$PIPELANG_NATIVE_DEBUG_LLC" --target windows-arm64 --output /tmp/select.obj
```

The target names are `linux-x64`, `linux-arm64`, `windows-x64`, `windows-arm64`,
`macos-x64`, and `macos-arm64`. Windows objects use `.obj`. Linux and macOS
objects use `.o`.

For the additional link and emulation checks, set both
`PIPELANG_NATIVE_DEBUG_LINKER_BIN` to the canonical LLVM linker directory and
`PIPELANG_NATIVE_DEBUG_QEMU_ARM64` to the canonical `qemu-aarch64` executable
before running the workflow. Those checks exercise the Linux ARM64 binary
under QEMU and link both Windows executables with PDBs. The resulting files
are in the proof directory's `targets` folder.

## Try it in VS Code

Open `Standalone.code-workspace` from the proof directory. Set a breakpoint
on line 10 of [Standalone.pipe](Standalone.pipe) and launch **Debug PipeLang
Standalone**. The stack frame is `Standalone.App.Run` and `answer` is `42`.
Continue execution to see the program print `42`.

The launch configurations use the VS Code C++ extension's GDB adapter
for the native executables.

Open `Select.code-workspace` from the proof directory. Open `Select.pipe`,
set a breakpoint on line 5, and launch **Debug PipeLang Select**. Watch
`result` in **Variables** and step over twice. It starts at `7` and reaches
`42`. The stack frame is `Select.Selection.Select`.

For GDB from a terminal, run the same binary from this example's directory:

```sh
cd examples/native
"$PIPELANG_NATIVE_DEBUG_GDB" "$PIPELANG_NATIVE_DEBUG_PROOF/select"
```

Set `break Select.pipe:5`, then use `run`, `print result`, `next` twice, and
`print result` again.

The scalar proof uses a public static pure method with signed integer inputs
and result. It supports integer and Boolean locals,
literals, comparisons, branches and assignments. Unsupported operations fail
without publishing an object. This experimental build does not select the
production default. See the [qualification contract](../../docs/concepts/pipelang-direct-native.md).

## Compile a project with source libraries

The workflow also builds a project whose call chain spans three source files,
including a checksum-pinned SDK import. To compile a project explicitly, use
the SDK's source-library references file and select the authored method:

```sh
"$PIPELANG_NATIVE_DEBUG_BUILD/sdk-tools/pipelang-objectc" \
  --project-root /absolute/path/to/project --project-entry Entry.pipe \
  --source-libraries /absolute/path/to/references.json \
  --entry Module.Class.Method --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output /tmp/project.o
```

`--project-entry` is relative to the source root, which defaults to `src`.
Use `--source-root` for a different project-relative directory. Omit
`--source-libraries` when every import is supplied by the project itself.

## Run the bundled collections and async app

[Standalone.pipe](Standalone.pipe) is a synchronous program. The workflow
builds it with the bundled SDK. Run `standalone/synchronous-debug` without
arguments. It prints `42`. In GDB, break at `Standalone.pipe:10` and run
`print answer` to inspect the same value before the return. The
`standalone/synchronous-debug-checked` executable takes an integer, prints its
successor, and exits with code 1 for a negative input.

[CollectionsAsync.pipe](CollectionsAsync.pipe) takes an integer index, awaits a
local async method, and reads from an owned `List<int>`. The same workflow builds
it through the experimental Linux x86-64 runtime path. Run the resulting
`standalone/collections-async` executable with an index:

```sh
"$PIPELANG_NATIVE_DEBUG_PROOF/standalone/collections-async" 1
```

This prints `42`. Index `2` returns the list count. Index `3` produces a bounds
failure. Supplying `--cancel` after the index cancels the task before its body runs.

The workflow stages the native object compiler, a prebuilt runtime archive and
the static link inputs in `runtime-sdk`. On Linux x86-64 you can compile and
link another copy with a single command:

```sh
sdk="$PIPELANG_NATIVE_DEBUG_PROOF/runtime-sdk"
"$sdk/pipelang-objectc" --executable --target linux-x64 \
  --source examples/native/CollectionsAsync.pipe --entry CollectionsAsync.App.Run \
  --llc "$PIPELANG_NATIVE_DEBUG_LLC" --linker /usr/bin/ld \
  --link-sdk "$sdk" --link-sdk-sha256 "$(cat "$sdk/link-sdk.sha256")" \
  --output /tmp/collections-async
/tmp/collections-async 1
```

If you have an installed PipeLang SDK built with native executable support,
use `pipelang build --backend native` in place of
`"$sdk/pipelang-objectc" --executable` with the same options.

The compiler validates the pinned link SDK and publishes the executable
after the object and link steps succeed. Use `--profile release` for optimized
code with source line tables. The resulting Linux executable includes the
runtime and runs with an empty environment.

For Linux ARM64, use `--target linux-arm64`, an ARM64 link SDK and an
absolute path to LLVM's `ld.lld`. Build the SDK once with the ARM64 runtime
archive and the matching compiler's static link inputs using
[`stage-runtime-sdk.sh`](../../workflows/native.debug/assets/scripts/stage-runtime-sdk.sh).
Point `PIPELANG_NATIVE_DEBUG_LINUX_ARM64_SDK` at the staged directory to
exercise debug and release under QEMU in the workflow. Set
`PIPELANG_NATIVE_DEBUG_LINUX_ARM64_LLDB` to an ARM64-capable LLDB to verify
the live `.pipe` breakpoint in a private network namespace.

This runtime path admits `List<int>` creation, append, indexing and count, and
one directly forwarded local `Task<Result<int,string>>` await. The
[suspending example](SuspendingCollections.pipe) adds a timed semaphore
await and uses `List<bool>` and `Array<int>`. The proof verifies a pending
task before the launcher polls it again:

```sh
"$PIPELANG_NATIVE_DEBUG_PROOF/suspending/collections-async" 1 --require-suspension
```

That prints `42` after resumption. `--cancel-after-suspension` verifies that an
already pending task is cancelled. The native qualification fixtures also
exercise collection copies, structured host calls, and cancellation of an
awaited host operation.

To build the same example for Windows x64, set
`PIPELANG_NATIVE_DEBUG_WINDOWS_TOOLCHAIN` to the canonical root of the pinned
LLVM-MinGW UCRT toolchain for Linux hosts and run the proof workflow to stage
the Windows SDK. Then compile and link the application with:

```sh
sdk="$PIPELANG_NATIVE_DEBUG_PROOF/windows-runtime/sdk"
"$sdk/pipelang-objectc" --executable --target windows-x64 \
  --source examples/native/SuspendingCollections.pipe --entry SuspendingCollections.App.Run \
  --llc "$PIPELANG_NATIVE_DEBUG_LLC" --linker "$PIPELANG_NATIVE_DEBUG_WINDOWS_TOOLCHAIN/bin/ld.lld" \
  --link-sdk "$sdk" --link-sdk-sha256 "$(cat "$sdk/link-sdk.sha256")" \
  --output /tmp/collections-async.exe
```

The executable uses Windows system DLLs and includes the PipeLang runtime.
The linker writes `collections-async.pdb` beside the executable by default.
Use `--pdb` to select a different PDB path.

For Windows ARM64, use `--target windows-arm64` and select
`$PIPELANG_NATIVE_DEBUG_PROOF/windows-arm64-runtime/sdk` as the link SDK.
The workflow builds debug and release ARM64 executables and PDBs and checks
the target architecture and source records. ARM64 Windows execution requires
a separate qualification run.

For macOS, run the declared PipeLang build workflow for the intended
architecture and select Apple's system SDK locally.
Prepare the pinned link SDK once, then build the example with the same
executable driver:

```sh
installed=/absolute/path/to/installed/pipelang-sdk
native_app="$installed/share/pipelang-model/native-application"
sdk=/tmp/pipelang-macos-link-sdk
bash workflows/native.debug/assets/scripts/stage-macos-link-sdk.sh \
  "$native_app/libpipelang_native_app_runtime.a" "$installed/bin/pipelang-sdk-tool" "$sdk"
"$installed/bin/pipelang" build --target macos-arm64 \
  --source examples/native/SuspendingCollections.pipe --entry SuspendingCollections.App.Run \
  --llc /absolute/path/to/llvm/bin/llc --linker "$(xcrun --sdk macosx --find ld)" \
  --macos-sysroot "$(xcrun --sdk macosx --show-sdk-path)" \
  --macos-sdk-version "$(xcrun --sdk macosx --show-sdk-version)" \
  --dsymutil "$(xcrun --find dsymutil)" \
  --link-sdk "$sdk" --link-sdk-sha256 "$(cat "$sdk/link-sdk.sha256")" \
  --output /tmp/collections-async
```

The app links against macOS system libraries and has a matching
`collections-async.dSYM` for source debugging. Use `--target macos-x64`
with the x64 runtime archive for Intel systems. The earlier macOS link proof
ran suspension, resumption, and cancellation on the physical M6. The
one-command link has a separate cross-link and dSYM proof for both architectures.

## Debug the resumed task on Windows and macOS

On Windows x64, place the executable, its generated `collections-async.pdb`,
and `SuspendingCollections.pipe` in the same proof directory. Run the
[Windows source-debug check](../../workflows/native.debug/assets/scripts/verify-windows-source-debug.ps1)
with `-Debugger` pointing to LLVM-MinGW's LLDB 22 and `-ProofDirectory`
pointing to that directory. The check explicitly loads the PDB and maps the
recorded source directory to the copy of the example. It verifies the
breakpoint at line 12, `index = 1`, `acquired = false`, the step to line 13,
and a successful exit.

On macOS ARM64, run the
[macOS source-debug check](../../workflows/native.debug/assets/scripts/verify-macos-source-debug.sh)
with Bash from Terminal. Pass the absolute executable path, the absolute
directory containing `SuspendingCollections.pipe`, and a path for the LLDB log.
Keep the compiled object available where it was when the Mach-O was linked.
Approve macOS's debugger authorization if prompted. The breakpoint resolved
on the M6 and a live debugger run still needs that authorization.
