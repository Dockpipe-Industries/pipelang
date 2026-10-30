package pipeview

import (
	"fmt"
	"unicode/utf8"
)

const Contract = "pipelang.pipeview.declaration.v1"
const PercentContract = "pipelang.pipeview.declaration.v2"
const MaximumNodes = 128
const MaximumDepth = 8
const MaximumTextBytes = 4096

// EventInfo is a typed host wiring requirement. Target is empty for clicks.
type EventInfo struct {
	Payload ValueKind
	Target  string
}

// Element is the read-only declaration/inspection projection. A Snapshot owns
// its collections, so editing it cannot change the compiled Document.
type Element struct {
	ID       string
	Kind     Kind
	Text     string
	Binding  string
	Event    string
	Children []Element
}

type Snapshot struct {
	Contract string
	Root     Element
	State    map[string]Value
	Events   map[string]EventInfo
}

type Document struct {
	root  Node
	valid bool
}

// Compile validates the bounded declaration before any adapter sees it.
// It does not introduce a second executable IR or change Application IR.
func Compile(root Node) (Document, error) {
	if _, err := inspect(root); err != nil {
		return Document{}, err
	}
	return Document{root: root, valid: true}, nil
}

func (document Document) Inspect() (Snapshot, error) {
	if !document.valid {
		return Snapshot{}, fmt.Errorf("PipeView document has not been compiled")
	}
	return inspect(document.root)
}

func validIdentity(identity string) bool {
	if len(identity) == 0 || len(identity) > 128 {
		return false
	}
	for _, character := range identity {
		if character >= 'a' && character <= 'z' || character >= 'A' && character <= 'Z' ||
			character >= '0' && character <= '9' || character == '.' || character == '-' || character == '_' {
			continue
		}
		return false
	}
	return true
}

func inspect(root Node) (Snapshot, error) {
	result := Snapshot{Contract: Contract, State: make(map[string]Value), Events: make(map[string]EventInfo)}
	identities := make(map[string]bool)
	count := 0
	var visit func(Node, int) (Element, error)
	visit = func(node Node, depth int) (Element, error) {
		count++
		if depth > MaximumDepth || count > MaximumNodes {
			return Element{}, fmt.Errorf("PipeView declaration exceeds depth or node capacity")
		}
		if !validIdentity(node.identity) || identities[node.identity] {
			return Element{}, fmt.Errorf("invalid or duplicate control identity %q", node.identity)
		}
		identities[node.identity] = true
		if len(node.text) > MaximumTextBytes || !utf8.ValidString(node.text) {
			return Element{}, fmt.Errorf("control %s has invalid or excessive UTF-8 text", node.identity)
		}
		container := node.kind == ColumnKind || node.kind == RowKind
		switch node.kind {
		case ColumnKind, RowKind, LabelKind, ButtonKind, ToggleKind, TextFieldKind, PercentStepperKind:
		default:
			return Element{}, fmt.Errorf("unsupported component %q", node.kind)
		}
		if !container && len(node.children) != 0 {
			return Element{}, fmt.Errorf("leaf control %s contains children", node.identity)
		}
		if node.kind == PercentStepperKind {
			result.Contract = PercentContract
		}
		needsBinding := node.bound
		if needsBinding {
			if !validIdentity(node.binding) {
				return Element{}, fmt.Errorf("control %s requires a valid state identity", node.identity)
			}
			switch node.initial.Kind {
			case Boolean, Integer:
			case Text:
				if len(node.initial.Text) > MaximumTextBytes || !utf8.ValidString(node.initial.Text) {
					return Element{}, fmt.Errorf("state %s has invalid or excessive UTF-8 text", node.binding)
				}
			default:
				return Element{}, fmt.Errorf("state %s has unsupported value type", node.binding)
			}
			if existing, found := result.State[node.binding]; found && existing != node.initial {
				return Element{}, fmt.Errorf("conflicting declarations for state %s", node.binding)
			}
			result.State[node.binding] = node.initial
		}
		needsEvent := node.kind == ButtonKind || node.kind == ToggleKind || node.kind == TextFieldKind || node.kind == PercentStepperKind
		if needsEvent {
			expected := Invocation
			if node.kind == ToggleKind {
				expected = Boolean
			} else if node.kind == TextFieldKind {
				expected = Text
			} else if node.kind == PercentStepperKind {
				expected = Integer
			}
			if !validIdentity(node.event) || node.payload != expected {
				return Element{}, fmt.Errorf("control %s requires a matching typed event", node.identity)
			}
			if expected != Invocation && node.initial.Kind != expected {
				return Element{}, fmt.Errorf("control %s has incompatible state", node.identity)
			}
			event := EventInfo{Payload: expected, Target: node.binding}
			if existing, found := result.Events[node.event]; found && existing != event {
				return Element{}, fmt.Errorf("conflicting declarations for event %s", node.event)
			}
			result.Events[node.event] = event
		}
		element := Element{ID: node.identity, Kind: node.kind, Text: node.text,
			Binding: node.binding, Event: node.event}
		for _, child := range node.children {
			projected, err := visit(child, depth+1)
			if err != nil {
				return Element{}, err
			}
			element.Children = append(element.Children, projected)
		}
		return element, nil
	}
	projected, err := visit(root, 1)
	if err != nil {
		return Snapshot{}, err
	}
	result.Root = projected
	return result, nil
}
