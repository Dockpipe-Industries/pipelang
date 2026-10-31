package pipeview

// Kind is a portable component capability, not a native widget class.
type Kind string

const (
	ColumnKind         Kind = "column"
	RowKind            Kind = "row"
	LabelKind          Kind = "label"
	ButtonKind         Kind = "button"
	ToggleKind         Kind = "toggle"
	TextFieldKind      Kind = "text-field"
	PercentStepperKind Kind = "percent-stepper"
)

// Node can only be populated through typed constructors. A zero Node is invalid.
type Node struct {
	bound    bool
	identity string
	kind     Kind
	text     string
	binding  string
	initial  Value
	event    string
	payload  ValueKind
	children []Node
}

func Column(identity string, children ...Node) Node {
	return container(identity, ColumnKind, children)
}

func Row(identity string, children ...Node) Node {
	return container(identity, RowKind, children)
}

func container(identity string, kind Kind, children []Node) Node {
	return Node{identity: identity, kind: kind, children: append([]Node(nil), children...)}
}

func StaticText(identity, text string) Node {
	return Node{identity: identity, kind: LabelKind, text: text}
}

func Label[T Scalar](identity, prefix string, binding Binding[T]) Node {
	return Node{identity: identity, kind: LabelKind, text: prefix,
		bound: true, binding: binding.identity, initial: binding.initial}
}

func Button(identity, label string, clicked Event[Unit]) Node {
	return Node{identity: identity, kind: ButtonKind, text: label,
		event: clicked.identity, payload: clicked.kind}
}

func Toggle(identity, label string, binding Binding[bool], changed Event[bool]) Node {
	return Node{identity: identity, kind: ToggleKind, text: label,
		bound: true, binding: binding.identity, initial: binding.initial,
		event: changed.identity, payload: changed.kind}
}

func TextField(identity string, binding Binding[string], changed Event[string]) Node {
	return Node{identity: identity, kind: TextFieldKind,
		bound: true, binding: binding.identity, initial: binding.initial,
		event: changed.identity, payload: changed.kind}
}

// PercentStepper edits an integer percentage through a typed change event.
// Bounds apply to gestures, not the raw authoritative draft value.
func PercentStepper(identity, label string, binding Binding[int64], changed Event[int64]) Node {
	return Node{identity: identity, kind: PercentStepperKind, text: label,
		bound: true, binding: binding.identity, initial: binding.initial,
		event: changed.identity, payload: changed.kind}
}
