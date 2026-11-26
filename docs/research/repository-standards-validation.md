# Repository standards validation, 2026-10-28

The repository now has a root `check` workflow for authored Go formatting,
DockPipe workflow declarations, shell/Python syntax and controlled toolchain/
delivery contracts. Linux CI runs it before build/test. The existing baseline
test workflow and optional native component build choices remain separate.

The exact DockPipe config/lock pin is now `0.6.33`, matching the installed runner.
All five official published staging-release archives were downloaded and checked
against release metadata, including regular executable member sizes. The Linux
archive executable matches `/usr/bin/dockpipe` byte-for-byte. Root validation no
longer removes a version gate in a temporary copy. This qualifies the Linux
runner; archive integrity checks are not native execution proof for macOS,
Windows or Linux ARM64.

GitHub Action references use verified full commit SHAs. Annotated tags were
dereferenced, including CodeQL and build provenance attestation. The existing
Go 1.26.10 CI selection and publication behavior were preserved. Hosted workflows
were not dispatched. Local checks used installed Go 1.25.0, satisfying the
module's declared minimum.

Thirty-eight authored Go files needed formatting normalization. Changes in those
files are `gofmt` output only; frozen compatibility inputs, licenses, source
ownership and public language/ABI behavior were preserved. The baseline suite
compiled the test sources and passed its selected cases. This is not a claim of
complete compiler conformance or a new performance result.

Evidence under `/tmp/repo-standards-pipelang` retains guarded source inventories,
verified release/action metadata, bounded job/worker receipts, check/build/test
logs and the final acceptance receipt. Validation includes 32 toolchain/delivery
tests, 25 native protocol tests, Go/native lexer comparisons, the eight-program
frozen pilot pipeline, and both authoring formats generating the exact previously
accepted ForgePipe settings headers. The first native-pilot preparation correctly
refused an older corpus containing extra receipt files; a fresh exact fixture
preserved that evidence and passed strict admission.

The actual checkout's root check/build/test workflows were run with installed
DockPipe and verified containment. Native runtime/ForgePipe UI evidence from the
accepted settings checkpoint is reused because the generated header bytes are
unchanged. No game/UI startup, full resource campaign, hosted release, commit or
push is included. Existing staged, unstaged and untracked work remains present;
generated tools and workflow state remain under ignored `bin/` paths.
