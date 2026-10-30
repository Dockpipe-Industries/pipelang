// Package state provides a bounded, synchronous PipeView reference state machine.
// Hosts own persistence and deliver its result through Complete.
package state

import (
	"fmt"
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
)

const MaximumStates = 128
const MaximumActions = 128

type Values map[string]pipeview.Value

type Operation string

const (
	Set    Operation = "set"
	Save   Operation = "save"
	Revert Operation = "revert"
)

type Action struct {
	Name      string
	Payload   pipeview.ValueKind
	Operation Operation
	Target    string
}

type Definition struct {
	Initial Values
	Actions []Action
}

type Outcome string

const (
	Success   Outcome = "success"
	Cancelled Outcome = "cancelled"
	Failure   Outcome = "failure"
)

type SaveRequest struct {
	ID       uint64
	Snapshot Values
}

type Snapshot struct {
	Draft   Values
	Saved   Values
	Dirty   bool
	Valid   bool
	Saving  bool
	CanSave bool
	Status  string
}

// Validator receives an owned copy and must be deterministic for given values.
type Validator func(Values) bool

// Model is synchronous. The host must serialize calls to its methods.
type Model struct {
	actions   map[string]Action
	draft     Values
	saved     Values
	validator Validator
	valid     bool
	status    string
	nextID    uint64
	pending   *SaveRequest
}

// New admits scalar state and typed actions before the host can dispatch events.
// A nil saved map uses Initial; an explicitly empty saved map is invalid.
func New(definition Definition, saved Values, validator Validator) (*Model, error) {
	if len(definition.Initial) == 0 || len(definition.Initial) > MaximumStates {
		return nil, fmt.Errorf("state count must be between 1 and %d", MaximumStates)
	}
	initial := clone(definition.Initial)
	for name, value := range initial {
		if !validIdentity(name) || !validScalar(value) {
			return nil, fmt.Errorf("invalid state %q", name)
		}
	}
	if len(definition.Actions) == 0 || len(definition.Actions) > MaximumActions {
		return nil, fmt.Errorf("action count must be between 1 and %d", MaximumActions)
	}
	actions := make(map[string]Action, len(definition.Actions))
	saveCount := 0
	revertCount := 0
	for _, action := range definition.Actions {
		if !validIdentity(action.Name) {
			return nil, fmt.Errorf("invalid action name %q", action.Name)
		}
		if _, exists := actions[action.Name]; exists {
			return nil, fmt.Errorf("duplicate action %q", action.Name)
		}
		switch action.Operation {
		case Set:
			target, exists := initial[action.Target]
			if !exists || action.Payload != target.Kind {
				return nil, fmt.Errorf("action %q requires a matching state target and scalar payload", action.Name)
			}
		case Save, Revert:
			if action.Payload != pipeview.Invocation || action.Target != "" {
				return nil, fmt.Errorf("action %q requires invocation payload and no target", action.Name)
			}
			if action.Operation == Save {
				saveCount++
			} else {
				revertCount++
			}
		default:
			return nil, fmt.Errorf("unsupported operation %q", action.Operation)
		}
		actions[action.Name] = action
	}
	if saveCount > 1 || revertCount > 1 {
		return nil, fmt.Errorf("at most one save action and one revert action are allowed")
	}
	var baseline Values
	if saved == nil {
		baseline = clone(initial)
	} else {
		if len(saved) != len(initial) {
			return nil, fmt.Errorf("saved state must contain exactly the declared states")
		}
		baseline = clone(saved)
	}
	if !sameShape(initial, baseline) {
		return nil, fmt.Errorf("saved state must contain exactly the declared canonical scalar types")
	}
	if validator == nil {
		validator = func(Values) bool { return true }
	}
	if !validator(clone(initial)) {
		return nil, fmt.Errorf("initial state failed validation")
	}
	if !validator(clone(baseline)) {
		return nil, fmt.Errorf("saved state failed validation")
	}
	return &Model{
		actions:   actions,
		draft:     clone(baseline),
		saved:     baseline,
		validator: validator,
		valid:     true,
		status:    "Saved",
	}, nil
}

// Dispatch edits a draft or returns a host-owned save request. It performs no I/O.
// Save and revert actions require a nil payload; set actions require one scalar.
func (model *Model) Dispatch(name string, payload *pipeview.Value) (*SaveRequest, error) {
	if model.pending != nil {
		return nil, fmt.Errorf("a save is already pending")
	}
	action, exists := model.actions[name]
	if !exists {
		return nil, fmt.Errorf("unknown action %q", name)
	}
	if action.Operation == Set {
		if payload == nil || payload.Kind != action.Payload || !validScalar(*payload) {
			return nil, fmt.Errorf("action %q requires a canonical %s payload", name, action.Payload)
		}
		draft := clone(model.draft)
		draft[action.Target] = *payload
		valid := model.validator(clone(draft))
		model.draft = draft
		model.valid = valid
		model.refreshStatus()
		return nil, nil
	}
	if payload != nil {
		return nil, fmt.Errorf("action %q does not accept a payload", name)
	}
	if action.Operation == Revert {
		model.draft = clone(model.saved)
		model.valid = true
		model.status = "Saved"
		return nil, nil
	}
	if !model.valid || equal(model.draft, model.saved) {
		return nil, fmt.Errorf("save requires valid unsaved changes")
	}
	// Validate the captured candidate again at the effect boundary.
	if !model.validator(clone(model.draft)) {
		return nil, fmt.Errorf("save candidate failed validation")
	}
	if model.nextID == ^uint64(0) {
		return nil, fmt.Errorf("save request identity capacity exhausted")
	}
	model.nextID++
	model.pending = &SaveRequest{ID: model.nextID, Snapshot: clone(model.draft)}
	model.status = "Saving"
	return &SaveRequest{ID: model.pending.ID, Snapshot: clone(model.pending.Snapshot)}, nil
}

// Complete accepts exactly the outstanding request ID and one supported outcome.
// A successful result advances the baseline to the captured save request.
func (model *Model) Complete(id uint64, outcome Outcome) error {
	if model.pending == nil || model.pending.ID != id {
		return fmt.Errorf("save request %d is not pending", id)
	}
	switch outcome {
	case Success:
		model.saved = clone(model.pending.Snapshot)
		model.status = "Saved"
	case Failure:
		model.status = "Save failed"
	case Cancelled:
		model.status = "Save cancelled"
	default:
		return fmt.Errorf("unsupported save outcome %q", outcome)
	}
	model.pending = nil
	return nil
}

// Snapshot returns owned maps; callers cannot mutate model state through them.
func (model *Model) Snapshot() Snapshot {
	dirty := !equal(model.draft, model.saved)
	saving := model.pending != nil
	return Snapshot{
		Draft:   clone(model.draft),
		Saved:   clone(model.saved),
		Dirty:   dirty,
		Valid:   model.valid,
		Saving:  saving,
		CanSave: dirty && model.valid && !saving,
		Status:  model.status,
	}
}

func (model *Model) refreshStatus() {
	if !model.valid {
		model.status = "Invalid settings"
	} else if !equal(model.draft, model.saved) {
		model.status = "Unsaved changes"
	} else {
		model.status = "Saved"
	}
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

func validScalar(value pipeview.Value) bool {
	switch value.Kind {
	case pipeview.Boolean:
		return value.Integer == 0 && value.Text == ""
	case pipeview.Integer:
		return !value.Boolean && value.Text == ""
	case pipeview.Text:
		return !value.Boolean && value.Integer == 0 &&
			len(value.Text) <= pipeview.MaximumTextBytes && utf8.ValidString(value.Text)
	default:
		return false
	}
}

func sameShape(declared, values Values) bool {
	if len(declared) != len(values) {
		return false
	}
	for name, expected := range declared {
		value, exists := values[name]
		if !exists || value.Kind != expected.Kind || !validScalar(value) {
			return false
		}
	}
	return true
}

func clone(values Values) Values {
	result := make(Values, len(values))
	for name, value := range values {
		result[name] = value
	}
	return result
}

func equal(first, second Values) bool {
	if len(first) != len(second) {
		return false
	}
	for name, value := range first {
		if other, exists := second[name]; !exists || other != value {
			return false
		}
	}
	return true
}
