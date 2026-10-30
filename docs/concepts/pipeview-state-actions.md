# Experimental PipeView typed state and actions

Status: locally implemented settings slice, 2026-10-28. Source profile v2 adds
closed typed action handlers to the explicit PipeLang view entrypoint. It does
not implement arbitrary action bodies, ordinary language mutation/effects/tasks,
or the complete settings-screen roadmap. Go is the reference implementation.

## Source

```pipe
view SettingsView {
    state playerName: string = "Rider";
    state vsync: bool = true;

    action nameEdited(string) => set playerName;
    action vsyncEdited(bool) => set vsync;
    action save() => save;
    action revert() => revert;

    Column(id: "root") {
        TextField(id: "name-field", value: bind playerName, changed: nameEdited);
        Toggle(id: "vsync-toggle", label: "VSync", value: bind vsync, changed: vsyncEdited);
        Label(id: "save-status", value: bind save.status);
        Row(id: "actions") {
            Button(id: "save-button", label: "Save", clicked: save);
            Button(id: "revert-button", label: "Revert", clicked: revert);
        }
    }
}
```

A `set` handler consumes one scalar payload and updates its declared target.
Payload, target and control binding types must agree; an action cannot silently
edit another same-typed binding. Save and Revert have no payload or target. There
can be at most one Save and one Revert action, with arbitrary declared names.
`save.status` is a read-only string projection of that declared Save action;
it is not persisted and cannot be edited. Each action must be used by the view.
Legacy event declarations cannot be mixed with these managed actions.

The source profile retains the existing scalar/bounds rules. Each profile has a
versioned contract: event-only v1 is unchanged; typed actions require
`pipelang.pipeview.source.v2`. Existing `source.Compile` refuses action views
rather than dropping their runtime plan. `source.CompileProgram` returns the
neutral document, checked state definition and derived status bindings.

## State and effect ownership

`sdk/pipeview/state` is a renderer/compiler/I/O-independent library. Its Go model
is the reference; the bounded C++ companion lives in `state/native/runtime.hpp`
and is embedded into explicitly selected generated action-view artifacts. The
exporter records `pipelang.pipeview.state.v1`, the exact runtime SHA-256 and the
complete generated-header SHA-256. No sibling runtime discovery or second state
implementation is copied into the showcase.

The model owns two typed snapshots: Draft and Saved. Only admitted `set` actions
change Draft. Dirty compares Draft with Saved; Valid comes from a deterministic
host-supplied validator; CanSave requires dirty, valid and not pending. The
showcase validator is a compiled PipeLang function with an admitted string-to-bool
signature. It currently requires a nonempty player name.

Save revalidates Draft at the effect boundary, captures an owned snapshot and
returns a request with a nonzero monotonically increasing ID. It performs no I/O.
While pending, edits, another Save and Revert refuse. The host performs the effect
and completes the matching request with a typed terminal outcome:

- Success advances Saved to the captured snapshot and clears dirty state.
- Failure preserves Draft and Saved for retry and displays `Save failed`.
- Cancelled preserves both snapshots and displays `Save cancelled`.

Unknown, stale, duplicate and mismatched completions refuse without mutation.
IDs are scoped to a live model instance; hosts must bind callbacks to that owner,
not route completions using a number alone. No detached work, scheduler or worker
thread is introduced by the library. The host must reconcile actual effect
outcomes; cancellation is not permission to erase an already committed effect.

Revert restores the saved baseline, including settings loaded from storage; it
is not factory reset. Snapshots, request data and validation inputs are owned
copies. The model validates types, canonical scalar fields, UTF-8, identity and
capacity bounds. Status strings are presentation only: transitions use typed
operations/outcomes and flags, never parsed display text.

## The settings showcase

The canonical showcase is now [examples/settings](../../examples/settings/README.md)
in PipeLang. ForgePipe supplies only its installed rendering/input SDK. Earlier
ForgePipe settings/counter examples remain bounded integration fixtures; new
application behavior is not being developed there.

The showcase requires an explicit absolute `--settings-file` path. Its small
versioned file codec preserves the typed player-name/VSync/volume snapshot, bounds input,
refuses malformed/nonregular/symlink sources, and checks the descriptor actually
opened. It writes a same-directory private temporary file, flushes it and atomically
replaces the destination. Parent-directory crash durability and multiprocess merge
or locking are not qualified. Corrupt data fails startup instead of silently
becoming defaults.

The installed ForgePipe Session stages text/toggle/percent edits in presentation state.
The showcase dispatches typed actions and then restores/projects the authoritative
model before drawing. Saving never reads the presentation map as application
state. Rejected or busy edits therefore cannot alter the model or stored snapshot.
The Session implementation itself is unchanged.

The showcase now runs Save persistence on one owned background worker. The
worker captures only an owned typed snapshot and persistence callable; it never
captures Application, Model or Session. The creator thread alone dispatches
model actions and polls/applies matching completions. Foreign-thread action and
completion calls refuse. Startup loading remains synchronous, and closing joins
started persistence before model/view teardown rather than cancelling a write.
This host integration does not implement general language async/task semantics.
See the [owned-worker proof](../research/pipeview-owned-save-worker.md). Model CanSave is enforced, but the installed Session has no dynamic
control-enabled API: the buttons remain visually enabled and policy refusals do
not write files. VSync remains a persisted setting value, not a renderer-mode
change. The model supports cancellation outcomes; the screen has no Cancel button
in this slice. Live AI inspection, styles, XML, hot reload, richer controls and
Qt/QML proof remain separate work.

See [verification](../research/pipeview-settings-save.md) and the
[roadmap](../agents/backlog.md#pipeview-modular-typed-ui-roadmap).

## Checked volume extension

The canonical settings screen adds int64 volume, initial 100, through a fixed
PercentStepper and a generated existing-pilot `IsValidVolume` predicate. The
reference and native models are unchanged. A canonical invalid integer remains
an invalid visible draft; Save refuses and Revert restores the saved volume.
Volume is not yet applied to an audio engine. Source v1/v2 behavior is unchanged
for earlier controls; the new component requires native transport v2.

Storage v2 writes the complete snapshot. Valid v1 files load volume 100 in
memory without any write; only explicit successful Save writes v2. Invalid v2
shape, noncanonical decimal spelling or volume outside 0..100 refuses. The same
owned worker captures volume with the other values. See the
[contract and qualification](../research/pipeview-percent-stepper.md).
