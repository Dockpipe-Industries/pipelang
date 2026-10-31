// Package pipeview defines the experimental backend-neutral PipeView declaration
// contract. It owns no renderer, compiler, mutable state runtime or host process.
package pipeview

// Scalar is the bounded value profile shared by the initial adapters.
// Named domain types require a future explicit conversion contract.
type Scalar interface {
	bool | int64 | string
}

type ValueKind string

const (
	Boolean    ValueKind = "bool"
	Integer    ValueKind = "int64"
	Text       ValueKind = "string"
	Invocation ValueKind = "invocation"
)

// Value is a snapshot, not mutable application state. Exactly the field selected
// by Kind is meaningful; declarations only construct canonical values.
type Value struct {
	Kind    ValueKind
	Boolean bool
	Integer int64
	Text    string
}

type Binding[T Scalar] struct {
	identity string
	initial  Value
}

func State[T Scalar](identity string, initial T) Binding[T] {
	return Binding[T]{identity: identity, initial: scalarValue(initial)}
}

func scalarValue[T Scalar](value T) Value {
	switch typed := any(value).(type) {
	case bool:
		return Value{Kind: Boolean, Boolean: typed}
	case int64:
		return Value{Kind: Integer, Integer: typed}
	case string:
		return Value{Kind: Text, Text: typed}
	default:
		panic("unreachable PipeView scalar type")
	}
}

// Unit identifies a payload-free UI event.
type Unit struct{}

// Event describes a UI event's payload. It is not a language action or an effect
// capability; the host must connect it to admitted application behavior.
type Event[T interface{ bool | int64 | string | Unit }] struct {
	identity string
	kind     ValueKind
}

func Change[T Scalar](identity string) Event[T] {
	var zero T
	return Event[T]{identity: identity, kind: scalarValue(zero).Kind}
}

func Click(identity string) Event[Unit] {
	return Event[Unit]{identity: identity, kind: Invocation}
}
