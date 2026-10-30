# PipeView owned background Save host

Status: locally implemented and verified, 2026-10-28. Recovery inspection found
the prior typed state/action and Save slice complete, with no active duplicate
work. Under the user's continuation instruction, this bounded follow-up stays
within the existing Save path. Existing compiler/model proofs and checkpoints were preserved.

## Changes

Only the PipeLang-owned settings host and its tests change. Save now starts one
explicit `std::launch::async` operation capturing an owned SettingsValue and
persistence callable. The default callable owns its SettingsStore/path; neither
Application, Model nor Session is captured by the worker. Model single-flight
policy prevents another persistence operation while the request is pending.

The application records its creator thread. Dispatch and completion entrypoints
refuse foreign callers before touching the model, future or Session. `pumpSave`
checks readiness without waiting; only the owner consumes the future and applies
the matching typed completion. Storage errors, launch/future exceptions and
invalid completion values become matching-ID Failure outcomes, preserving the
draft for retry. Cancellation results retain their existing model semantics.

The future is the last member, and the destructor explicitly waits before
model/view teardown. Closing finishes an already-started write; it does not
promise cancellation, rollback or a bounded filesystem shutdown time. Startup
loading is still synchronous. Snapshot/session access remains owner-thread-only.
The injected persistence seam is host testing infrastructure and must not retain
UI/model objects. No scheduler, detached thread, language task semantics, public
SDK ABI or compiler contract is introduced.

The native proof captures the actual Saving frame and waits for model completion
before final capture, with an independent deadline. Test-only worker fixtures are
built only when BUILD_TESTING is enabled. Thread linkage uses CMake Threads.

## Verification

Evidence root: `/tmp/pipeview-save-async`. `source-receipt.json` links the previous
accepted checkpoint and captures original showcase sources. Temporary proof
workflow assets live with that snapshot; no owning workflow, pin or config was
changed. Installed `/usr/bin/dockpipe` validated and ran this host proof inside
the existing bounded execution harness.

- `host-proof.json` and `ui-result.json`: both default-Go and selected-native
  compiler-component builds pass the existing file lifecycle/admission checks and
  the new worker fixture against the qualified installed ForgePipe SDK0.9.0.
- The gated worker test checks off-owner execution, captured values, nonblocking
  pumping while storage is held, busy action refusal and single flight. A worker
  completing its callable does not update the model until the owner pumps it.
- Worker failures, standard/nonstandard exceptions, invalid outcome values and
  cancellation are followed by successful retry. Foreign dispatch/pump calls
  refuse. A controlled close joins held persistence before teardown completes.
- Review strengthened the teardown fixture to hold its gate across a bounded
  future wait and check worker completion after destruction. Only that changed
  fixture was rebuilt/retested in `worker-final.json`, for both component builds;
  completed compiler/model campaigns were not repeated.
- Three visible native processes per build cover Save, fresh-process reopen and
  destination failure. All six render at least eight frames. New pending captures
  show Saving before owner completion; final captures show Saved or Save failed.
  Saved file bytes match the previous accepted settings checkpoint.
- Generated validator, bridge and view/model headers match the prior checkpoint
  byte-for-byte. Compiler tools, Go reference state model, native SDK companion,
  source profiles and frozen fixtures are unchanged. Existing Go/native-model
  acceptance is reused rather than claimed as newly executed work.

The installed SDK archive SHA-256 remains
`76b28415007dae63dd68f755825155451dda84b24ebd32be009e9004ff859f08`; its expanded
regular files were rechecked. No sibling engine implementation is compiled.
Accepted containment receipts require no swap/OOM and process-tree cleanup.
Pending and final native captures are under `settings-{go,cpp}/native-*`.

This is host-owned background persistence, not general PipeLang async support.
UI cancellation controls, dynamic visual disabling, richer action bodies,
styles/XML/hot reload and additional backend qualification remain pending.
No new physical-input, Qt, platform, performance or crash-durability claims are
made. No ForgePipe/DockPipe source, compiler binaries, Git lifecycle, publication
or system installation is changed by this increment.
