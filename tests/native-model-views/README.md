# Native model-bound view tests

`test_protocol.py --compiler /absolute/pipelang-model-viewc --output /tmp/view-generated`
checks structured refusals and writes an independent C++ model oracle, the generated
adapter and a CMake project. Run the script and generated build/execution through
`tests/containedexec/run.py` with explicit limits. Configure the generated project
with the exact installed ForgePipe 0.9.0 UI SDK and
`PIPELANG_MODEL_RUNTIME_INCLUDE` pointing at the installed model runtime include.
No compiler, engine source or native window startup is needed at test runtime.

The oracle checks typed field/getter projection, sensitive inspection, authoritative
enabled state, malformed event refusal, sync failure/cancellation, owned async
cancellation and polling, raw integer preservation and atomic projection refusal.
It intentionally does not use the model compiler, so independent adapter defects
cannot be hidden by equivalent defects in the model compiler. Parent consumer
qualification separately joins both compilers against installed SDKs.

`test_cross_tool.py --model-compiler /absolute/pipelang-modelc --view-compiler
/absolute/pipelang-model-viewc` compiles the authored `FixtureModel.pipe` and
passes its actual emitted manifest to the view compiler. It checks both an
explicit constructor and the model compiler's synthesized default constructor,
plus malformed constructor refusal. This is the cross-tool schema oracle;
`test_protocol.py` retains its independent manifest for focused hostile inputs.
