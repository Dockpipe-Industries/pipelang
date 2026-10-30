// Package testing provides deterministic typed widget handles for the PipeView
// reference state model. Hosts explicitly complete saves; mounting and events
// perform no rendering, persistence, process execution or clock operations.
package testing

import (
	"fmt"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/inspect"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
)

// Options supplies a previously saved baseline and optional domain validation.
// A nil Saved map selects the definition's initial values.
type Options struct {
	Saved    state.Values
	Validate state.Validator
}

// Host connects admitted declaration events to the real reference state model.
// Calls are synchronous and must be serialized by the caller.
type Host struct {
	document       pipeview.Document
	definition     state.Definition
	statusBindings map[string]string
	model          *state.Model
	controls       map[string]pipeview.Element
}

// Mount admits the complete declaration, state model and status wiring before
// exposing any widget handle. Definition and status maps are copied on admission.
func Mount(document pipeview.Document, definition state.Definition, statusBindings map[string]string, options Options) (*Host, error) {
	if len(statusBindings) > pipeview.MaximumNodes {
		return nil, fmt.Errorf("status binding capacity exceeded")
	}
	model, err := state.New(definition, options.Saved, options.Validate)
	if err != nil {
		return nil, fmt.Errorf("mount state model: %w", err)
	}
	// Model admission bounds all caller collections before owned copies.
	ownedDefinition := state.Definition{
		Initial: make(state.Values, len(definition.Initial)),
		Actions: append([]state.Action(nil), definition.Actions...),
	}
	for identity, value := range definition.Initial {
		ownedDefinition.Initial[identity] = value
	}
	ownedBindings := make(map[string]string, len(statusBindings))
	for identity, action := range statusBindings {
		ownedBindings[identity] = action
	}
	if _, err := inspect.Capture(document, ownedDefinition, model.Snapshot(), ownedBindings, inspect.Policy{}); err != nil {
		return nil, fmt.Errorf("mount declaration: %w", err)
	}
	declaration, err := document.Inspect()
	if err != nil {
		return nil, fmt.Errorf("mount declaration: %w", err)
	}
	host := &Host{
		document:       document,
		definition:     ownedDefinition,
		statusBindings: ownedBindings,
		model:          model,
		controls:       make(map[string]pipeview.Element),
	}
	host.collectControls(declaration.Root)
	return host, nil
}

func (host *Host) collectControls(element pipeview.Element) {
	host.controls[element.ID] = element
	for _, child := range element.Children {
		host.collectControls(child)
	}
}

func (host *Host) control(identity string, kind pipeview.Kind) (pipeview.Element, error) {
	if host == nil || host.model == nil {
		return pipeview.Element{}, fmt.Errorf("widget requires a mounted host")
	}
	control, found := host.controls[identity]
	if !found {
		return pipeview.Element{}, fmt.Errorf("unknown control %q", identity)
	}
	if control.Kind != kind {
		return pipeview.Element{}, fmt.Errorf("control %q has kind %s, expected %s", identity, control.Kind, kind)
	}
	return control, nil
}

// TextField returns a typed handle for one admitted text-field identity.
func (host *Host) TextField(identity string) (TextField, error) {
	control, err := host.control(identity, pipeview.TextFieldKind)
	if err != nil {
		return TextField{}, err
	}
	return TextField{host: host, event: control.Event}, nil
}

// Toggle returns a typed handle for one admitted toggle identity.
func (host *Host) Toggle(identity string) (Toggle, error) {
	control, err := host.control(identity, pipeview.ToggleKind)
	if err != nil {
		return Toggle{}, err
	}
	return Toggle{host: host, event: control.Event}, nil
}

// Button returns a typed handle for one admitted button identity.
func (host *Host) Button(identity string) (Button, error) {
	control, err := host.control(identity, pipeview.ButtonKind)
	if err != nil {
		return Button{}, err
	}
	return Button{host: host, event: control.Event}, nil
}

// TextField can submit only canonical string payloads through its declared event.
type TextField struct {
	host  *Host
	event string
}

func (field TextField) Change(value string) error {
	if field.host == nil || field.host.model == nil {
		return fmt.Errorf("text field requires a mounted host")
	}
	payload := pipeview.Value{Kind: pipeview.Text, Text: value}
	_, err := field.host.model.Dispatch(field.event, &payload)
	return err
}

// Toggle can submit only canonical Boolean payloads through its declared event.
type Toggle struct {
	host  *Host
	event string
}

func (toggle Toggle) Change(value bool) error {
	if toggle.host == nil || toggle.host.model == nil {
		return fmt.Errorf("toggle requires a mounted host")
	}
	payload := pipeview.Value{Kind: pipeview.Boolean, Boolean: value}
	_, err := toggle.host.model.Dispatch(toggle.event, &payload)
	return err
}

// Button invokes its declared payload-free save or revert action.
type Button struct {
	host  *Host
	event string
}

func (button Button) Invoke() (*state.SaveRequest, error) {
	if button.host == nil || button.host.model == nil {
		return nil, fmt.Errorf("button requires a mounted host")
	}
	return button.host.model.Dispatch(button.event, nil)
}

// Complete supplies an explicit host-controlled result for the pending save.
func (host *Host) Complete(identity uint64, outcome state.Outcome) error {
	if host == nil || host.model == nil {
		return fmt.Errorf("completion requires a mounted host")
	}
	return host.model.Complete(identity, outcome)
}

// Snapshot returns an owned copy of the model's current draft and saved values.
func (host *Host) Snapshot() state.Snapshot {
	return host.model.Snapshot()
}

// Inspect captures the current semantic state with values redacted by default.
func (host *Host) Inspect(policy inspect.Policy) (inspect.Frame, error) {
	if host == nil || host.model == nil {
		return inspect.Frame{}, fmt.Errorf("inspection requires a mounted host")
	}
	return inspect.Capture(host.document, host.definition, host.model.Snapshot(), host.statusBindings, policy)
}

// PercentStepper returns a typed handle for an integer percent editor.
func (host *Host) PercentStepper(identity string) (PercentStepper, error) {
	control, err := host.control(identity, pipeview.PercentStepperKind)
	if err != nil {
		return PercentStepper{}, err
	}
	return PercentStepper{host: host, event: control.Event}, nil
}

// PercentStepper submits canonical int64 drafts; model validation owns bounds.
type PercentStepper struct {
	host  *Host
	event string
}

func (stepper PercentStepper) Change(value int64) error {
	if stepper.host == nil || stepper.host.model == nil {
		return fmt.Errorf("percent stepper requires a mounted host")
	}
	payload := pipeview.Value{Kind: pipeview.Integer, Integer: value}
	_, err := stepper.host.model.Dispatch(stepper.event, &payload)
	return err
}
