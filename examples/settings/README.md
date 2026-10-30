# Typed native settings application

The ordinary PipeLang SettingsModel owns draft/saved values, validation, edit and
Revert actions, pure availability/status properties and one async Save method.
Save captures an owned typed value, awaits the injected storage service, and
updates its own baseline/status/error. There is no Set/Save/Revert runtime opcode
list or handwritten native state-transition controller in this application.

The exact installed experimental PipeLangModel 0.1.0 SDK compiles Settings.pipe
and SettingsView.pipe. Generated model/view headers provide typed snapshots,
actions, field/getter bindings and inspection metadata. The C++ shell supplies
SettingsStore I/O and pumps owner-thread completions through the generated bridge.
SettingsValue is an alias of the generated source record; file-format migration
remains in SettingsStore. An atomic file replacement that already completed is
reported as Success even when cancellation arrived during the commit.

Configure with exact installed ForgePipe 0.9.0 and PipeLangModel SDK prefixes:

```sh
cmake -S examples/settings -B /absolute/build/settings \
  -DCMAKE_PREFIX_PATH='/absolute/PipeLangModel;/absolute/ForgePipe' \
  -DPIPEVIEW_SETTINGS_BUILD_NATIVE=OFF
cmake --build /absolute/build/settings --parallel 1
ctest --test-dir /absolute/build/settings --output-on-failure
```

The repository root workflows remain the development runner. Compiler/generated
execution and these headless fixtures must run within the existing containedexec
job.py/run.py limits when used for repository qualification. Standalone consumers
need installed SDKs and CMake/C++20, without DockPipe or Go at application runtime.

The selected SDK verifies source/tool/runtime manifests before and after native
compilation. Refused generation preserves previous complete outputs. Views bind
ordinary members and pure properties; generated dispatch checks event provenance,
payload types and enabled state through ForgePipe's reusable adapter. Pure enabled
properties drive rendering, dispatch and inspection from one committed projection.
Protected fields are redacted in inspection unless explicitly exposed by binding
identity. Expected failures/cancellation remain visible in the model's status and
error bindings and in generated typed action/completion reports.

The old generate_bindings.py, test_bindings.py, SettingsView.xml and v2 inspection
oracle remain legacy scalar/source-v2 compatibility fixtures; they are not the
current application's compiler or controller. General local async composition,
Optional, loops, arbitrary generics and XML authoring for the new model profile
remain separate language work. This example does not claim those features, GPU
qualification, native input qualification, performance or release readiness.
