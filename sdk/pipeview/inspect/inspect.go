// Package inspect projects admitted PipeView declarations and authoritative
// state into an owned, value-redacted inspection frame. Static labels, control
// identities and model status remain visible; this is not secret-text detection.
package inspect

import (
	"fmt"
	"strconv"
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
)

const Contract = "pipelang.pipeview.inspect.v1"
const PercentContract = "pipelang.pipeview.inspect.v2"

type Policy struct {
	ExposeBindings map[string]bool
}

type Frame struct {
	Schema   string             `json:"schema"`
	Root     Node               `json:"root"`
	Bindings map[string]Binding `json:"bindings"`
	Actions  map[string]Action  `json:"actions"`
	Model    Model              `json:"model"`
}

type Node struct {
	ID              string        `json:"id"`
	Kind            pipeview.Kind `json:"kind"`
	Text            string        `json:"text"`
	Binding         string        `json:"binding,omitempty"`
	Action          string        `json:"action,omitempty"`
	Enabled         bool          `json:"enabled"`
	ActionAvailable *bool         `json:"action_available,omitempty"`
	Children        []Node        `json:"children,omitempty"`
}

type Binding struct {
	Type     pipeview.ValueKind `json:"type"`
	ReadOnly bool               `json:"readonly"`
	Redacted bool               `json:"redacted"`
	// Value is nil when redacted. Exposed integers use decimal strings so a
	// JSON consumer does not lose int64 precision through a JavaScript number.
	Value any `json:"value,omitempty"`
}

type Action struct {
	Operation state.Operation    `json:"operation"`
	Payload   pipeview.ValueKind `json:"payload"`
	Target    string             `json:"target,omitempty"`
}

type Model struct {
	Dirty   bool   `json:"dirty"`
	Valid   bool   `json:"valid"`
	Saving  bool   `json:"saving"`
	CanSave bool   `json:"can_save"`
	Status  string `json:"status"`
}

// Capture reads a declaration and state snapshot without invoking any action,
// application validator, renderer or host effect. The default policy exposes no values.
// statusBindings maps each derived binding identity to its owning save action.
func Capture(document pipeview.Document, definition state.Definition, snapshot state.Snapshot,
	statusBindings map[string]string, policy Policy) (Frame, error) {
	declaration, err := document.Inspect()
	if err != nil {
		return Frame{}, err
	}
	if err := validateSnapshot(definition, snapshot); err != nil {
		return Frame{}, err
	}
	actions := make(map[string]Action, len(definition.Actions))
	for _, action := range definition.Actions {
		actions[action.Name] = Action{Operation: action.Operation, Payload: action.Payload, Target: action.Target}
	}
	if len(declaration.Events) != len(actions) {
		return Frame{}, fmt.Errorf("document actions must match the state definition")
	}
	for name, event := range declaration.Events {
		action, exists := actions[name]
		if !exists || action.Payload != event.Payload || action.Target != event.Target {
			return Frame{}, fmt.Errorf("document action %q differs from its typed definition", name)
		}
	}
	if len(declaration.State) != len(definition.Initial)+len(statusBindings) {
		return Frame{}, fmt.Errorf("document bindings must match state and derived status declarations")
	}
	for name, initial := range definition.Initial {
		if declared, exists := declaration.State[name]; !exists || declared != initial {
			return Frame{}, fmt.Errorf("document binding %q differs from its state declaration", name)
		}
	}
	for binding, owner := range statusBindings {
		action, exists := actions[owner]
		if !exists || action.Operation != state.Save || binding != owner+".status" {
			return Frame{}, fmt.Errorf("derived status %q requires its declared save owner", binding)
		}
		if _, collision := definition.Initial[binding]; collision {
			return Frame{}, fmt.Errorf("derived status %q collides with user state", binding)
		}
		expected := pipeview.Value{Kind: pipeview.Text, Text: "Saved"}
		if declared, exists := declaration.State[binding]; !exists || declared != expected {
			return Frame{}, fmt.Errorf("derived status %q must declare the initial Saved text", binding)
		}
	}
	if len(policy.ExposeBindings) > len(declaration.State) {
		return Frame{}, fmt.Errorf("exposure policy contains unknown binding identities")
	}
	for binding := range policy.ExposeBindings {
		if _, exists := declaration.State[binding]; !exists {
			return Frame{}, fmt.Errorf("exposure policy contains unknown binding %q", binding)
		}
	}
	bindings := make(map[string]Binding, len(declaration.State))
	for name, value := range snapshot.Draft {
		bindings[name] = projectBinding(value, false, policy.ExposeBindings[name])
	}
	for binding := range statusBindings {
		value := pipeview.Value{Kind: pipeview.Text, Text: snapshot.Status}
		bindings[binding] = projectBinding(value, true, policy.ExposeBindings[binding])
	}
	contract := Contract
	if declaration.Contract == pipeview.PercentContract {
		contract = PercentContract
	}
	return Frame{
		Schema:   contract,
		Root:     projectNode(declaration.Root, actions, snapshot),
		Bindings: bindings,
		Actions:  actions,
		Model: Model{
			Dirty:   snapshot.Dirty,
			Valid:   snapshot.Valid,
			Saving:  snapshot.Saving,
			CanSave: snapshot.CanSave,
			Status:  snapshot.Status,
		},
	}, nil
}

func validateSnapshot(definition state.Definition, snapshot state.Snapshot) error {
	// New has no validator here: it admits bounded canonical types without
	// claiming to recompute the application's domain validity decision.
	if len(snapshot.Draft) != len(definition.Initial) || len(snapshot.Saved) != len(definition.Initial) {
		return fmt.Errorf("snapshot must contain exactly the declared draft and saved state")
	}
	if _, err := state.New(definition, snapshot.Draft, nil); err != nil {
		return fmt.Errorf("invalid draft snapshot: %w", err)
	}
	if _, err := state.New(definition, snapshot.Saved, nil); err != nil {
		return fmt.Errorf("invalid saved snapshot: %w", err)
	}
	dirty := false
	for name, value := range snapshot.Draft {
		if snapshot.Saved[name] != value {
			dirty = true
			break
		}
	}
	if snapshot.Dirty != dirty || snapshot.CanSave != (dirty && snapshot.Valid && !snapshot.Saving) {
		return fmt.Errorf("snapshot dirty and save availability flags are inconsistent")
	}
	if snapshot.Saving && (!snapshot.Dirty || !snapshot.Valid) {
		return fmt.Errorf("saving requires a valid dirty snapshot")
	}
	if len(snapshot.Status) > pipeview.MaximumTextBytes || !utf8.ValidString(snapshot.Status) {
		return fmt.Errorf("snapshot status exceeds the UTF-8 text profile")
	}
	return nil
}

func projectBinding(value pipeview.Value, readOnly, expose bool) Binding {
	binding := Binding{Type: value.Kind, ReadOnly: readOnly, Redacted: !expose}
	if !expose {
		return binding
	}
	switch value.Kind {
	case pipeview.Boolean:
		binding.Value = value.Boolean
	case pipeview.Integer:
		binding.Value = strconv.FormatInt(value.Integer, 10)
	case pipeview.Text:
		binding.Value = value.Text
	}
	return binding
}

func projectNode(element pipeview.Element, actions map[string]Action, snapshot state.Snapshot) Node {
	node := Node{
		ID:      element.ID,
		Kind:    element.Kind,
		Text:    element.Text,
		Binding: element.Binding,
		Action:  element.Event,
		Enabled: true,
	}
	if element.Event != "" {
		available := !snapshot.Saving
		if actions[element.Event].Operation == state.Save {
			available = snapshot.CanSave
		}
		node.ActionAvailable = &available
	}
	for _, child := range element.Children {
		node.Children = append(node.Children, projectNode(child, actions, snapshot))
	}
	return node
}
