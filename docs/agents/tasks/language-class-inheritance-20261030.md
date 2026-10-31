# Native class inheritance

Objective: `pipelang-class-inheritance-20261030`.

The C++ v3 object compiler now implements ordinary single-class inheritance,
base constructor chaining, inherited state and interface contracts, protected
members and explicit virtual/override dispatch. Exported roots retain their
separate role. The canonical semantics are in
[the object contract](../../concepts/pipelang-objects.md#class-inheritance).

New compiler acceptance/refusal and generated runtime assertions are C++.
`workflows/test/config.yml` owns the optional native proof step. Select it with
`PIPELANG_TEST_CLASS_INHERITANCE=1` and an absolute fresh
`PIPELANG_CLASS_INHERITANCE_PROOF` outside the checkout. The workflow uses the
existing capped execution launchers, a fresh installed SDK, an independent C++
API oracle, exact installed compiler response comparisons and generated C++
runtime/sanitizer fixtures. No new Python test driver or Go feature implementation
is introduced. Existing compatibility tests retain their current owners.

State: completed locally. DockPipe root check/build/test passed, with 107 existing
native units and 12 new C++ inheritance units. The C++ compiler oracle verified
45 semantic refusals, six legacy-profile refusals, exact installed CLI/API
agreement, source/module behavior and hierarchy/member bounds. Runtime checks
cover base-view identity and physical state, constructor order and 4,200 failed
constructor savepoints, required inherited references, effective dispatch,
rollback, graph capacity/cycle reclamation, async cancellation and owner teardown.
All 21 runtime sanitizer fixture pairs passed ASAN/UBSAN. The obsolete previous
class-inheritance refusal was removed from the existing compatibility inventory;
new behavior assertions belong to the C++ oracle.

Evidence, retained partial attempts and protected baseline inventories live under
`/tmp/pipelang-class-inheritance-20261030/`; `acceptance.json` records terminal
receipts and exact ownership. Prior object/interface acceptance is admitted.
MesaMoto/ForgePipe bytes and all Git anchors are preserved. No commit, push,
publication, consumer port, system installation or live UI startup occurred.
ThreadSanitizer was not repeated after the preceding unsupported-host probe;
full compiler conformance, native UI, platform and release qualification remain
outside this local headless proof.
