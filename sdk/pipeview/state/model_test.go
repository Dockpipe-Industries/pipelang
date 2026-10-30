package state

import (
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
)

func textValue(text string) pipeview.Value {
	return pipeview.Value{Kind: pipeview.Text, Text: text}
}

func integerValue(integer int64) pipeview.Value {
	return pipeview.Value{Kind: pipeview.Integer, Integer: integer}
}

func booleanValue(boolean bool) pipeview.Value {
	return pipeview.Value{Kind: pipeview.Boolean, Boolean: boolean}
}

func settingsDefinition() Definition {
	return Definition{
		Initial: Values{
			"rider": textValue("Rider"), "volume": integerValue(20), "assists": booleanValue(true),
		},
		Actions: []Action{
			{Name: "rename", Payload: pipeview.Text, Operation: Set, Target: "rider"},
			{Name: "volume", Payload: pipeview.Integer, Operation: Set, Target: "volume"},
			{Name: "assists", Payload: pipeview.Boolean, Operation: Set, Target: "assists"},
			{Name: "save", Payload: pipeview.Invocation, Operation: Save},
			{Name: "revert", Payload: pipeview.Invocation, Operation: Revert},
		},
	}
}

func validSettings(values Values) bool {
	return values["rider"].Text != "" && values["volume"].Integer >= 0 && values["volume"].Integer <= 100
}

func newSettings(t *testing.T) *Model {
	t.Helper()
	model, err := New(settingsDefinition(), nil, validSettings)
	if err != nil {
		t.Fatal(err)
	}
	return model
}

func dispatch(t *testing.T, model *Model, action string, payload *pipeview.Value) *SaveRequest {
	t.Helper()
	request, err := model.Dispatch(action, payload)
	if err != nil {
		t.Fatal(err)
	}
	return request
}

func assertUnchanged(t *testing.T, model *Model, operation func() error) {
	t.Helper()
	before := model.Snapshot()
	if err := operation(); err == nil {
		t.Fatal("expected refusal")
	}
	if after := model.Snapshot(); !reflect.DeepEqual(before, after) {
		t.Fatalf("refusal mutated state: before=%+v after=%+v", before, after)
	}
}

func TestStateMachineTrace(t *testing.T) {
	model := newSettings(t)
	var pendingID uint64
	steps := []struct {
		name    string
		action  string
		payload *pipeview.Value
		outcome Outcome
		draft   string
		saved   string
		dirty   bool
		valid   bool
		saving  bool
		canSave bool
		status  string
	}{
		{
			name:   "initial",
			draft:  "Rider",
			saved:  "Rider",
			valid:  true,
			status: "Saved",
		},
		{
			name:    "edit",
			action:  "rename",
			payload: valuePointer(textValue("Jamie")),
			draft:   "Jamie",
			saved:   "Rider",
			dirty:   true,
			valid:   true,
			canSave: true,
			status:  "Unsaved changes",
		},
		{
			name:   "begin",
			action: "save",
			draft:  "Jamie",
			saved:  "Rider",
			dirty:  true,
			valid:  true,
			saving: true,
			status: "Saving",
		},
		{
			name:    "fail",
			outcome: Failure,
			draft:   "Jamie",
			saved:   "Rider",
			dirty:   true,
			valid:   true,
			canSave: true,
			status:  "Save failed",
		},
		{
			name:   "retry",
			action: "save",
			draft:  "Jamie",
			saved:  "Rider",
			dirty:  true,
			valid:  true,
			saving: true,
			status: "Saving",
		},
		{
			name:    "cancel",
			outcome: Cancelled,
			draft:   "Jamie",
			saved:   "Rider",
			dirty:   true,
			valid:   true,
			canSave: true,
			status:  "Save cancelled",
		},
		{
			name:   "retry again",
			action: "save",
			draft:  "Jamie",
			saved:  "Rider",
			dirty:  true,
			valid:  true,
			saving: true,
			status: "Saving",
		},
		{
			name:    "complete",
			outcome: Success,
			draft:   "Jamie",
			saved:   "Jamie",
			valid:   true,
			status:  "Saved",
		},
		{
			name:    "invalid edit",
			action:  "rename",
			payload: valuePointer(textValue("")),
			draft:   "",
			saved:   "Jamie",
			dirty:   true,
			status:  "Invalid settings",
		},
		{
			name:   "revert",
			action: "revert",
			draft:  "Jamie",
			saved:  "Jamie",
			valid:  true,
			status: "Saved",
		},
	}
	for _, step := range steps {
		t.Run(step.name, func(t *testing.T) {
			if step.action != "" {
				request := dispatch(t, model, step.action, step.payload)
				if step.action == "save" {
					if request == nil || request.ID <= pendingID {
						t.Fatalf("save request did not advance: %+v", request)
					}
					pendingID = request.ID
				} else if request != nil {
					t.Fatal("non-save action returned a save request")
				}
			}
			if step.outcome != "" {
				if err := model.Complete(pendingID, step.outcome); err != nil {
					t.Fatal(err)
				}
			}
			expectedDraft := settingsDefinition().Initial
			expectedDraft["rider"] = textValue(step.draft)
			expectedSaved := settingsDefinition().Initial
			expectedSaved["rider"] = textValue(step.saved)
			expected := Snapshot{
				Draft: expectedDraft, Saved: expectedSaved, Dirty: step.dirty, Valid: step.valid,
				Saving: step.saving, CanSave: step.canSave, Status: step.status,
			}
			if actual := model.Snapshot(); !reflect.DeepEqual(actual, expected) {
				t.Fatalf("snapshot=%+v want=%+v", actual, expected)
			}
		})
	}
}

func valuePointer(value pipeview.Value) *pipeview.Value {
	return &value
}

func TestDispatchRefusalsAreAtomic(t *testing.T) {
	model := newSettings(t)
	cases := []struct {
		action  string
		payload *pipeview.Value
	}{
		{action: "unknown"},
		{action: "rename"},
		{action: "rename", payload: valuePointer(booleanValue(true))},
		{action: "rename", payload: valuePointer(pipeview.Value{Kind: pipeview.Text, Text: "Rider", Boolean: true})},
		{action: "rename", payload: valuePointer(textValue(string([]byte{0xff})))},
		{action: "rename", payload: valuePointer(textValue(strings.Repeat("x", 4097)))},
		{action: "volume", payload: valuePointer(pipeview.Value{Kind: pipeview.Integer, Integer: 5, Text: "hidden"})},
		{action: "assists", payload: valuePointer(pipeview.Value{Kind: pipeview.Boolean, Integer: 1})},
		{action: "save", payload: valuePointer(pipeview.Value{Kind: pipeview.Invocation})},
		{action: "revert", payload: valuePointer(textValue(""))},
		{action: "save"},
	}
	for index, test := range cases {
		t.Run(fmt.Sprint(index), func(t *testing.T) {
			assertUnchanged(t, model, func() error {
				_, err := model.Dispatch(test.action, test.payload)
				return err
			})
		})
	}
	dispatch(t, model, "rename", valuePointer(textValue("")))
	assertUnchanged(t, model, func() error {
		_, err := model.Dispatch("save", nil)
		return err
	})
	dispatch(t, model, "rename", valuePointer(textValue("Rider")))
	if snapshot := model.Snapshot(); snapshot.Dirty || !snapshot.Valid || snapshot.Status != "Saved" {
		t.Fatalf("restoring the original value must become clean: %+v", snapshot)
	}
}

func TestPendingSaveAndCompletionAdmission(t *testing.T) {
	model := newSettings(t)
	dispatch(t, model, "rename", valuePointer(textValue("Jamie")))
	request := dispatch(t, model, "save", nil)
	for _, action := range []string{"rename", "save", "revert", "unknown"} {
		assertUnchanged(t, model, func() error {
			_, err := model.Dispatch(action, valuePointer(textValue("Other")))
			return err
		})
	}
	for _, id := range []uint64{0, request.ID + 1} {
		assertUnchanged(t, model, func() error { return model.Complete(id, Success) })
	}
	assertUnchanged(t, model, func() error { return model.Complete(request.ID, Outcome("unknown")) })
	if err := model.Complete(request.ID, Failure); err != nil {
		t.Fatal(err)
	}
	assertUnchanged(t, model, func() error { return model.Complete(request.ID, Success) })
	second := dispatch(t, model, "save", nil)
	assertUnchanged(t, model, func() error { return model.Complete(request.ID, Success) })
	if err := model.Complete(second.ID, Success); err != nil {
		t.Fatal(err)
	}
	assertUnchanged(t, model, func() error { return model.Complete(second.ID, Success) })
}

func TestTypedEditsAndLoadedBaseline(t *testing.T) {
	saved := settingsDefinition().Initial
	saved["rider"] = textValue("Loaded rider")
	model, err := New(settingsDefinition(), saved, validSettings)
	if err != nil {
		t.Fatal(err)
	}
	if snapshot := model.Snapshot(); snapshot.Draft["rider"].Text != "Loaded rider" || snapshot.Dirty {
		t.Fatalf("loaded baseline not reflected in clean draft: %+v", snapshot)
	}
	dispatch(t, model, "volume", valuePointer(integerValue(100)))
	dispatch(t, model, "assists", valuePointer(booleanValue(false)))
	snapshot := model.Snapshot()
	if snapshot.Draft["volume"] != integerValue(100) || snapshot.Draft["assists"] != booleanValue(false) || !snapshot.CanSave {
		t.Fatalf("typed edits lost: %+v", snapshot)
	}
	dispatch(t, model, "volume", valuePointer(integerValue(-1)))
	if snapshot := model.Snapshot(); snapshot.Valid || snapshot.CanSave || snapshot.Status != "Invalid settings" {
		t.Fatalf("invalid draft was not retained with save blocked: %+v", snapshot)
	}
	dispatch(t, model, "revert", nil)
	if snapshot := model.Snapshot(); !reflect.DeepEqual(snapshot.Draft, saved) || !reflect.DeepEqual(snapshot.Saved, saved) || snapshot.Dirty {
		t.Fatalf("revert did not restore loaded baseline: %+v", snapshot)
	}
}

func TestAllExternalCollectionsAreOwned(t *testing.T) {
	definition := settingsDefinition()
	saved := clone(definition.Initial)
	var retainedValidatorInput Values
	validator := func(values Values) bool {
		retainedValidatorInput = values
		values["rider"] = textValue("Validator mutation")
		delete(values, "volume")
		return true
	}
	model, err := New(definition, saved, validator)
	if err != nil {
		t.Fatal(err)
	}
	definition.Initial["rider"] = textValue("Definition mutation")
	definition.Actions[0].Target = "volume"
	saved["rider"] = textValue("Saved input mutation")
	retainedValidatorInput["assists"] = booleanValue(false)
	if snapshot := model.Snapshot(); !reflect.DeepEqual(snapshot.Draft, settingsDefinition().Initial) {
		t.Fatalf("constructor retained caller or validator storage: %+v", snapshot)
	}
	payload := textValue("Jamie")
	dispatch(t, model, "rename", &payload)
	payload.Text = "Payload mutation"
	retainedValidatorInput["rider"] = textValue("Retained validator mutation")
	snapshot := model.Snapshot()
	snapshot.Draft["rider"] = textValue("Snapshot mutation")
	snapshot.Saved["rider"] = textValue("Saved snapshot mutation")
	delete(snapshot.Draft, "volume")
	request := dispatch(t, model, "save", nil)
	request.Snapshot["rider"] = textValue("Save request mutation")
	delete(request.Snapshot, "volume")
	if err := model.Complete(request.ID, Success); err != nil {
		t.Fatal(err)
	}
	actual := model.Snapshot()
	expected := settingsDefinition().Initial
	expected["rider"] = textValue("Jamie")
	if !reflect.DeepEqual(actual.Draft, expected) || !reflect.DeepEqual(actual.Saved, expected) || actual.Dirty {
		t.Fatalf("external mutation reached model or pending request: %+v", actual)
	}
}

func TestDefinitionAdmission(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*Definition)
	}{
		{
			name: "empty states",
			mutate: func(definition *Definition) {
				definition.Initial = nil
			},
		},
		{
			name: "empty actions",
			mutate: func(definition *Definition) {
				definition.Actions = nil
			},
		},
		{
			name: "empty state identity",
			mutate: func(definition *Definition) {
				definition.Initial[""] = textValue("x")
			},
		},
		{
			name: "unicode state identity",
			mutate: func(definition *Definition) {
				definition.Initial["café"] = textValue("x")
			},
		},
		{
			name: "long state identity",
			mutate: func(definition *Definition) {
				definition.Initial[strings.Repeat("x", 129)] = textValue("x")
			},
		},
		{
			name: "noncanonical boolean",
			mutate: func(definition *Definition) {
				definition.Initial["assists"] = pipeview.Value{Kind: pipeview.Boolean, Integer: 1}
			},
		},
		{
			name: "noncanonical integer",
			mutate: func(definition *Definition) {
				definition.Initial["volume"] = pipeview.Value{Kind: pipeview.Integer, Boolean: true}
			},
		},
		{
			name: "noncanonical text",
			mutate: func(definition *Definition) {
				definition.Initial["rider"] = pipeview.Value{Kind: pipeview.Text, Integer: 1}
			},
		},
		{
			name: "unknown scalar",
			mutate: func(definition *Definition) {
				definition.Initial["rider"] = pipeview.Value{Kind: "future"}
			},
		},
		{
			name: "invocation state",
			mutate: func(definition *Definition) {
				definition.Initial["rider"] = pipeview.Value{Kind: pipeview.Invocation}
			},
		},
		{
			name: "invalid UTF8",
			mutate: func(definition *Definition) {
				definition.Initial["rider"] = textValue(string([]byte{0xff}))
			},
		},
		{
			name: "long text",
			mutate: func(definition *Definition) {
				definition.Initial["rider"] = textValue(strings.Repeat("x", 4097))
			},
		},
		{
			name: "invalid action identity",
			mutate: func(definition *Definition) {
				definition.Actions[0].Name = "has spaces"
			},
		},
		{
			name: "duplicate action",
			mutate: func(definition *Definition) {
				definition.Actions[1].Name = "rename"
			},
		},
		{
			name: "unknown operation",
			mutate: func(definition *Definition) {
				definition.Actions[0].Operation = "future"
			},
		},
		{
			name: "wrong payload",
			mutate: func(definition *Definition) {
				definition.Actions[0].Payload = pipeview.Boolean
			},
		},
		{
			name: "missing target",
			mutate: func(definition *Definition) {
				definition.Actions[0].Target = "missing"
			},
		},
		{
			name: "invocation set",
			mutate: func(definition *Definition) {
				definition.Actions[0].Payload = pipeview.Invocation
			},
		},
		{
			name: "save target",
			mutate: func(definition *Definition) {
				definition.Actions[3].Target = "rider"
			},
		},
		{
			name: "save scalar payload",
			mutate: func(definition *Definition) {
				definition.Actions[3].Payload = pipeview.Text
			},
		},
		{
			name: "revert target",
			mutate: func(definition *Definition) {
				definition.Actions[4].Target = "rider"
			},
		},
		{
			name: "duplicate save",
			mutate: func(definition *Definition) {
				definition.Actions = append(definition.Actions, Action{Name: "save2", Payload: pipeview.Invocation, Operation: Save})
			},
		},
		{
			name: "duplicate revert",
			mutate: func(definition *Definition) {
				definition.Actions = append(definition.Actions, Action{Name: "revert2", Payload: pipeview.Invocation, Operation: Revert})
			},
		},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			definition := settingsDefinition()
			test.mutate(&definition)
			if model, err := New(definition, nil, nil); err == nil || model != nil {
				t.Fatalf("invalid definition admitted: model=%+v error=%v", model, err)
			}
		})
	}
}

func TestSavedStateAdmission(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(Values)
	}{
		{
			name: "missing key",
			mutate: func(values Values) {
				delete(values, "rider")
			},
		},
		{
			name: "extra key",
			mutate: func(values Values) {
				values["extra"] = textValue("x")
			},
		},
		{
			name: "same count foreign key",
			mutate: func(values Values) {
				delete(values, "rider")
				values["foreign"] = textValue("x")
			},
		},
		{
			name: "wrong type",
			mutate: func(values Values) {
				values["rider"] = booleanValue(true)
			},
		},
		{
			name: "noncanonical value",
			mutate: func(values Values) {
				values["rider"] = pipeview.Value{Kind: pipeview.Text, Text: "x", Integer: 1}
			},
		},
		{
			name: "invalid saved value",
			mutate: func(values Values) {
				values["rider"] = textValue("")
			},
		},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			saved := settingsDefinition().Initial
			test.mutate(saved)
			if _, err := New(settingsDefinition(), saved, validSettings); err == nil {
				t.Fatal("invalid saved state admitted")
			}
		})
	}
	if _, err := New(settingsDefinition(), Values{}, nil); err == nil {
		t.Fatal("explicit empty saved map must not select defaults")
	}
	definition := settingsDefinition()
	definition.Initial["rider"] = textValue("")
	if _, err := New(definition, settingsDefinition().Initial, validSettings); err == nil {
		t.Fatal("valid saved state hid invalid defaults")
	}
}

func TestBoundedCapacities(t *testing.T) {
	definition := Definition{Initial: make(Values)}
	for index := 0; index < 128; index++ {
		name := fmt.Sprintf("state.%d", index)
		definition.Initial[name] = integerValue(int64(index))
		definition.Actions = append(definition.Actions, Action{Name: fmt.Sprintf("set.%d", index), Payload: pipeview.Integer, Operation: Set, Target: name})
	}
	if _, err := New(definition, nil, nil); err != nil {
		t.Fatalf("maximum bounded definition refused: %v", err)
	}
	definition.Initial["excess"] = integerValue(0)
	if _, err := New(definition, nil, nil); err == nil {
		t.Fatal("excess states admitted")
	}
	delete(definition.Initial, "excess")
	definition.Actions = append(definition.Actions, Action{Name: "excess", Payload: pipeview.Integer, Operation: Set, Target: "state.0"})
	if _, err := New(definition, nil, nil); err == nil {
		t.Fatal("excess actions admitted")
	}
	boundary := strings.Repeat("x", 128)
	definition = Definition{
		Initial: Values{boundary: textValue(strings.Repeat("é", 2048))},
		Actions: []Action{{Name: boundary, Payload: pipeview.Text, Operation: Set, Target: boundary}},
	}
	model, err := New(definition, nil, nil)
	if err != nil {
		t.Fatalf("exact identity and UTF8 byte limits refused: %v", err)
	}
	dispatch(t, model, boundary, valuePointer(textValue(strings.Repeat("é", 2048))))
}

func TestSaveRequestIdentityExhaustion(t *testing.T) {
	model := newSettings(t)
	model.nextID = ^uint64(0) - 1
	dispatch(t, model, "rename", valuePointer(textValue("Jamie")))
	request := dispatch(t, model, "save", nil)
	if request.ID != ^uint64(0) {
		t.Fatalf("last identity=%d", request.ID)
	}
	if err := model.Complete(request.ID, Success); err != nil {
		t.Fatal(err)
	}
	dispatch(t, model, "rename", valuePointer(textValue("Other")))
	assertUnchanged(t, model, func() error {
		_, err := model.Dispatch("save", nil)
		return err
	})
}
