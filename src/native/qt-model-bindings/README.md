# Optional Qt model binding compiler

The [current checked Qt binding contract](../../../docs/concepts/pipelang-qt-model-bindings.md)
is canonical for supported model types, typed outcomes, thread ownership and the
single-thread WASM exception strategy. This directory contains the isolated native
host metadata admission/emitter. It does not replace the ForgePipe view compiler
or implement application policy.

`pipelang-qt-bindc --manifest model.json --model DockPipe.Cloud.Presentation.AccessStatus
--namespace cloud_presentation --adapter AccessStatusQt` emits a header to stdout.
The canonical source identity is selected explicitly from the checked v1/v2 model
manifest. The optional SDK helper `pipelang_add_qt_bindings` records exact host-tool,
manifest and output hashes and verifies the installed SDK on every build.

The standalone CMake target requires an explicit SHA256-pinned nlohmann header.
Build/test through the owning declared contained workflow; no dependency download
or sibling-source fallback is part of this tool. `pipelang-qt-binding-tests` owns
positive/negative profile and emitted-contract checks. Actual Qt moc consumers and
Qt/WASM application/browser acceptance remain separate tests.
