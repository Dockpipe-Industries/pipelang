# PipeLang Workspace I/O 0.1.0 candidate

Optional Linux native capability for admitted local files, paged directory access,
read-only Git signals and bounded inotify changes. Public C++20 umbrella:
`<pipelang/workspace/WorkspaceIo.hpp>`; namespace `pipelang::workspace`.
Installed CMake contract: `find_package(PipeLangWorkspaceIo 0.1.0 EXACT CONFIG REQUIRED)`
and target `PipeLang::WorkspaceIo`.

This module implements OS/API interoperability. It contains no project, tenant,
Markdown, indexing, relationship, synchronization or cloud policy. PipeLang
application interfaces consume these capabilities through their generated native
port adapters. Tenant/home registration and marker validation belong to the
consumer composition boundary.

## Admission, ownership and failures

`FileIo(homeAbsolute, cancellation={})` requires an existing euid-owned private
root (no group/other permission bits). Every absolute root component is opened
without following symlinks. Relative paths reject absolute names, NUL, invalid
UTF-8, backslashes, empty/dot/dot-dot components and case variants of `.git`.
Directory traversal pins descriptors, rejects symlinks and device crossings.
Root replacement or widened root permissions are rejected. Files must be regular,
euid-owned and single-link, at most1MiB; bytes are not decoded as text.

`read`, `writeNew`, `replace`, `makeDirectory`, `resolve`, `DirectoryPager::list`,
`RepositoryInspector::inspect`, `ChangeFeed::watch/changes` throw `WorkspaceError`
with stable nonsecret `workspace.*` codes. Expected failures are translated to
Success/Cancelled/Failure at the generated port adapter. No library/OS diagnostics,
paths, file content or remote URLs are logged. Cancellation is checked between
bounded operations. A stalled local kernel filesystem syscall is not preempted;
this module does not qualify network/FUSE filesystems or cross-platform behavior.

New files are0600 and new directories0700. Publication creates an exclusive random
temporary file, writes and fsyncs it, rechecks its identity, then links without
replacement for create or renames for replace. It flushes the parent directory and
reads the result back. Replace verifies the expected SHA256 and uses a per-instance
mutex plus a root-directory flock shared by cooperating processes. This is not an
atomic CAS against arbitrary editors or a multi-file transaction. Any uncertain
post-publication result or failed owned-temp cleanup reports `workspace.unknown_effect`;
inspect/reconcile before retrying. MakeDirectory creates missing parents and is
idempotent for admitted directories; errors after creating a prefix also report
unknown effect. Resolved paths are verified at the call, not a retained pathname
lease against later external changes.

SHA256 uses the separately pinned maintained PipeLang Crypto ABI1, not a custom
implementation. UUIDv4 entropy uses Linux getrandom with a nonblocking failure.

## Bounds

- Relative/root paths:4096bytes; component255bytes; file1MiB.
- Cooperative lock admission:1second, with cancellation checks.
- Directory page:1..150 entries; at most10,000 scanned names and250ms of userspace
  work;64KiB materialized entry metadata. Cursor is opaque stamped state containing
  directory identity/mtime/ctime and the last name. A changed directory rejects the
  cursor. Forbidden, symlink and non-regular/non-directory entries are omitted.
- Git:at most64 operator-selected aliases; exact checkout root required; fixed
  readonly rev-parse/config argv; no shell, hooks, network, checkout, clone, commit
  or publish. Child environment excludes inherited GIT_* overrides, while HOME
  retains normal Git safety/config behavior. Output max16KiB, individual signals
  max4096bytes, total inspection deadline1.5seconds(default; admitted1..2000ms).
  Child cancellation/timeout kills its process group and reaps the direct child.
  Checkout/common-dir identities are local dev/inode signals, not canonical remote
  repository identity. The raw origin can contain credentials; the PipeLang caller
  must normalize/redact it before persistence, display or logging.
- inotify:64 explicitly watched directories, no implicit recursive watcher;
 512pending events,256returned events,64KiB reply,256KiB drained native bytes per
  call,wait0..1000ms. Kernel overflow, bounded queue loss or invalidated/moved-out
  watches set `overflow=true`, requiring an authoritative rescan. Directory changes
  require the caller to register newly admitted subdirectories. The stream is not
  a durable journal. Destruction closes the inotify descriptor and releases watches.

## Build and qualification

No downloads or sibling source dependencies. CMake requires an installed exact
PipeLangModel0.1.0 package and an installed Crypto header/archive selected by
explicit prefix and exact SHA256 values. It preserves the Crypto header, static
archive, receipt, lockfile and license notices in the installed package.

Run the declared module workflow with the exact SDK and Crypto prefixes and an
absolute artifact directory:

```sh
PIPELANG_SDK_PREFIX=/absolute/qualified/sdk \
PIPELANG_WORKSPACE_CRYPTO_PREFIX=/absolute/qualified/crypto \
PIPELANG_WORKSPACE_IO_PROOF=/tmp/workspace-io-proof \
dockpipe --workflow-file workflows/test/config.yml --workdir /absolute/modules/workspace-io
```

The candidate workflow verifies the pinned SDK manifest/runner and Crypto digests,
then runs single-worker configure/build/test/install under unchanged job/unit caps.
Set `PIPELANG_WORKSPACE_IO_SANITIZE=ON` for C++ ASan/UBSan; this does not instrument
or qualify Rust dependencies. Native fixtures cover hostile paths/links/FIFO,
byte limits, stale revisions, cooperating writers, cursor invalidation, root
replacement, direct-editor atomic rename, bounded queue overflow, moved watches,
Git raw-signal handling without credential output, and subprocess timeout/cancel
cleanup. They do not prove crash/power-loss durability, kernel queue exhaustion,
arbitrary external-writer CAS or hostile filesystem syscall preemption.
