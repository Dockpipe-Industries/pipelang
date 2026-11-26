# Ordinary-model nullable values

The ordinary-model source form is `T?`. `Optional<T>` and source `Some`/`None`
constructor calls are rejected; they are not accepted compatibility spellings.
The local Linux compiler, runtime and relocated SDK have passed qualification;
Windows, macOS and release qualification remain separate.

```pipelang
public Interface HeightSampler {
    Result<float32?,string> Sample();
}

public Class Heights {
    public pure Result<float32?,string> Read(bool present) {
        if (present)
            return Result<float32?,string>.Success(12f);
        return Result<float32?,string>.Success(null);
    }
}
```

A known nullable return, field, local, assignment or argument admits an owned
value as present and `null` as absent. This includes a Result's success payload,
collection elements and closed generic parameters. `null` never means zero,
default payload, Failure or Cancelled. Nonnullable values/references remain
nonnullable; there is no implicit unwrapping, raw pointer or unchecked access.

`HasValue` observes presence. `Value` retains the existing checked runtime
access: absence raises the same ExecutionFailure at the owning result boundary.
This is not a flow-sensitive compiler proof that a preceding HasValue dominates
the read. Nullable arithmetic requires explicit checked payload access.

The four Result states remain distinct: Success(value), Success(null),
Failure(exact error) and Cancelled(exact error). Native callbacks and propagation
must preserve those distinctions. A cancellation is never fabricated from a
message or converted into successful absence.

`T??` retains two layers; it is not flattened. `null` assigned to `int??` is an
absent outer layer. To represent a present outer layer with an absent inner
layer, give the inner expression its type first:

```pipelang
public Class Layers {
    public pure int?? PresentAbsent() {
        int? inner = null;
        return inner;
    }
}
```

Suffix `?` is parsed only in type positions; expression `condition ? a : b`
remains conditional evaluation with the same lazy arm ordering. Equality with
null is permitted when the other side supplies a nullable type. Null-to-nonnullable
assignment, implicit unwrapping, ambiguous overloads, untyped null equality,
malformed suffixes and excessive nesting are rejected diagnostically.

The parser lowers `T?` to the existing internal optional type. Its identity text,
closed generic/native symbol mapping, runtime storage, object-domain codecs,
owned-value validation and C++ layout remain unchanged. User diagnostics format
nullable types as `T?`; identity and display are deliberately separate. The C++
runtime still names its internal carrier Optional and retains Some/None; that is
not a second accepted PipeLang source syntax. Historical frozen profile inputs
remain historical evidence, not compiler compatibility promises.

Tests migrate active ordinary-model fixtures, preserving existing assertions.
Concrete private test functions returning T? retain the former temporary-Value
lifetime/refusal cases; no extra constructor syntax or application helper policy
is introduced. The qualified local Linux SDK manifest SHA256 is
`54465c14d9f65c9f1c00f426f2ebcfc1e9a3ae42a262b9afc6225b06722abaf0`.
Its default campaign passed 303 contained units and audits, including runtime,
sanitizer and resource checks; repository check, historical response comparisons,
package and relocated installed consumers also passed. These local results do not
qualify Windows, macOS or a release.
