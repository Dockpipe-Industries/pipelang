# Local type inference

The ordinary native-model compiler supports `var` local declarations
in its v1, v2, v3 and project profiles. The checked type is the initializer's
exact static type; it remains fixed for every later assignment. This includes
primitives, owned records, closed generic values, nullable values, permitted
source-object references, shared handles and awaited results.

```pipelang
var result = Assert.Equal<int>(42, actual, "answer");
var values = List<int>.Empty();
var optional = ReadOptional();
var response = await service.Read(token);

for (var index = 0; index < values.Count; index = index + 1) {
    var value = values.At(index);
}

foreach (var item in values) {
    var copy = item;
}

parallel foreach (var item in values) {
    var previous = total.Add(item);
}
```

The examples assume the ordinary enclosing pure/effect/async contracts and
declared values. `var` does not infer generic method arguments, alter overload
selection, relax effects, admit unawaited tasks, or change object/worker ownership.
Parallel loops retain the same explicit shared-handle capture requirements.

A local must have an initializer. Bare `null` and void expressions have no usable
inferred local type and are refused; use an explicit nullable type for bare null.
Reading a local within its own initializer is refused. All ordinary naming,
scope, assignment, resource and cancellation rules still apply. Loop initializer
locals end with the loop; foreach locals use the exact sequence item type.
ByteBuffer items remain `uint32` and dictionary items remain `KeyValuePair<K,V>`.

`var` is contextual. Existing identifiers named `var` remain valid. If an authored
nominal type named `var` is visible, the normal type resolver selects that explicit
type; otherwise `var` requests local inference and is not an API type. Fields,
records, parameters, return types, generic type
arguments and catch bindings still require explicit types. This keeps public
contracts stable and preserves existing native interfaces.

The compiler resolves every inferred local into the ordinary checked type before
generation. Header and compiled output use the same typed lowering. The focused
`VarInferenceTests.cpp` oracle checks explicit/inferred emitted equivalence and
concrete AST types; its runtime fixtures cover objects, project headers and compiled
project units through the existing normal/sanitizer native workflow lane.

The compiler's semantic analysis protocol returns exact inferred types and UTF-8
byte ranges for local declarations/references and `var` tokens, including lexical
scope and original module/file identity. Editor clients consume these checked
symbols for type information and navigation; analysis does not generate a native
backend artifact. Platform qualification remains limited to the recorded runs.
