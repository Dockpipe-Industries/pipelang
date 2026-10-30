package inspect_test

import (
	"bytes"
	"encoding/json"
	"math"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/inspect"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
)

type fixture struct {
	document       pipeview.Document
	definition     state.Definition
	snapshot       state.Snapshot
	statusBindings map[string]string
	policy         inspect.Policy
}

func textValue(text string) pipeview.Value {
	return pipeview.Value{Kind: pipeview.Text, Text: text}
}

func newFixture(t *testing.T) fixture {
	t.Helper()
	definition := state.Definition{
		Initial: state.Values{
			"name":    textValue("Original rider"),
			"enabled": {Kind: pipeview.Boolean, Boolean: false},
			"minimum": {Kind: pipeview.Integer, Integer: math.MinInt64},
			"maximum": {Kind: pipeview.Integer, Integer: math.MaxInt64},
			"empty":   textValue(""),
			"unicode": textValue("秘密\x00"),
		},
		Actions: []state.Action{
			{Name: "nameEdited", Payload: pipeview.Text, Operation: state.Set, Target: "name"},
			{Name: "enabledEdited", Payload: pipeview.Boolean, Operation: state.Set, Target: "enabled"},
			{Name: "save", Payload: pipeview.Invocation, Operation: state.Save},
			{Name: "revert", Payload: pipeview.Invocation, Operation: state.Revert},
		},
	}
	root := pipeview.Column("root",
		pipeview.StaticText("title", "Visible settings"),
		pipeview.TextField("name-field", pipeview.State("name", "Original rider"), pipeview.Change[string]("nameEdited")),
		pipeview.Toggle("enabled-toggle", "Enabled", pipeview.State("enabled", false), pipeview.Change[bool]("enabledEdited")),
		pipeview.Label("minimum-label", "", pipeview.State("minimum", int64(math.MinInt64))),
		pipeview.Label("maximum-label", "", pipeview.State("maximum", int64(math.MaxInt64))),
		pipeview.Label("empty-label", "", pipeview.State("empty", "")),
		pipeview.Label("unicode-label", "", pipeview.State("unicode", "秘密\x00")),
		pipeview.Label("status", "", pipeview.State("save.status", "Saved")),
		pipeview.Button("save-button", "Save", pipeview.Click("save")),
		pipeview.Button("revert-button", "Revert", pipeview.Click("revert")),
	)
	document, err := pipeview.Compile(root)
	if err != nil {
		t.Fatal(err)
	}
	model, err := state.New(definition, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	name := textValue("Private draft rider")
	if _, err := model.Dispatch("nameEdited", &name); err != nil {
		t.Fatal(err)
	}
	return fixture{
		document:       document,
		definition:     definition,
		snapshot:       model.Snapshot(),
		statusBindings: map[string]string{"save.status": "save"},
	}
}

func capture(t *testing.T, source fixture) inspect.Frame {
	t.Helper()
	frame, err := inspect.Capture(source.document, source.definition, source.snapshot, source.statusBindings, source.policy)
	if err != nil {
		t.Fatal(err)
	}
	return frame
}

func marshal(t *testing.T, frame inspect.Frame) []byte {
	t.Helper()
	data, err := json.Marshal(frame)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestDefaultRedactionAndMetadata(t *testing.T) {
	source := newFixture(t)
	frame := capture(t, source)
	if frame.Schema != "pipelang.pipeview.inspect.v1" || frame.Root.ID != "root" || frame.Root.Kind != pipeview.ColumnKind {
		t.Fatalf("unexpected inspection identity: %+v", frame)
	}
	if len(frame.Bindings) != 7 || len(frame.Actions) != 4 || frame.Model.Status != "Unsaved changes" {
		t.Fatalf("incomplete frame metadata: %+v", frame)
	}
	for name, binding := range frame.Bindings {
		if !binding.Redacted || binding.Value != nil || binding.ReadOnly != (name == "save.status") {
			t.Fatalf("binding %s leaked or has wrong ownership: %+v", name, binding)
		}
	}
	if frame.Actions["nameEdited"] != (inspect.Action{Operation: state.Set, Payload: pipeview.Text, Target: "name"}) {
		t.Fatalf("typed action metadata differs: %+v", frame.Actions["nameEdited"])
	}
	if frame.Root.Children[0].Text != "Visible settings" {
		t.Fatal("static labels must remain visible")
	}
	data := marshal(t, frame)
	for _, secret := range []string{"Private draft rider", "Original rider", "秘密", "9223372036854775807", "-9223372036854775808"} {
		if bytes.Contains(data, []byte(secret)) {
			t.Fatalf("default capture leaked state value %q", secret)
		}
	}
	for _, forbidden := range []string{`"value":`, `"saved":`, `"storage_error":`, `"pending":`, `"snapshot":`} {
		if bytes.Contains(data, []byte(forbidden)) {
			t.Fatalf("inspection unexpectedly exposed %s", forbidden)
		}
	}
	var walk func(inspect.Node)
	walk = func(node inspect.Node) {
		if !node.Enabled {
			t.Fatalf("neutral control unexpectedly disabled: %s", node.ID)
		}
		if node.Action == "" && node.ActionAvailable != nil {
			t.Fatalf("non-action control has action availability: %s", node.ID)
		}
		for _, child := range node.Children {
			walk(child)
		}
	}
	walk(frame.Root)
}

func TestExplicitExposurePreservesScalarTypesAndPrecision(t *testing.T) {
	source := newFixture(t)
	source.policy.ExposeBindings = map[string]bool{
		"name": true, "enabled": true, "minimum": true, "maximum": true,
		"empty": true, "unicode": true, "save.status": true,
	}
	frame := capture(t, source)
	expected := map[string]any{
		"name": "Private draft rider", "enabled": false,
		"minimum": "-9223372036854775808", "maximum": "9223372036854775807",
		"empty": "", "unicode": "秘密\x00", "save.status": "Unsaved changes",
	}
	for name, value := range expected {
		binding := frame.Bindings[name]
		if binding.Redacted || !reflect.DeepEqual(binding.Value, value) {
			t.Fatalf("exposed %s=%#v, expected %#v", name, binding.Value, value)
		}
	}
	var decoded struct {
		Bindings map[string]map[string]json.RawMessage `json:"bindings"`
	}
	if err := json.Unmarshal(marshal(t, frame), &decoded); err != nil {
		t.Fatal(err)
	}
	if string(decoded.Bindings["enabled"]["value"]) != "false" || string(decoded.Bindings["empty"]["value"]) != `""` {
		t.Fatal("JSON omitted exposed false or empty scalar")
	}
	if string(decoded.Bindings["maximum"]["value"]) != `"9223372036854775807"` {
		t.Fatal("JSON integer must be a decimal string")
	}
	source.policy.ExposeBindings["name"] = false
	if binding := capture(t, source).Bindings["name"]; !binding.Redacted || binding.Value != nil {
		t.Fatal("explicit false exposure entry exposed a value")
	}
}

func TestActionAvailabilityUsesAuthoritativeFlags(t *testing.T) {
	cases := []struct {
		name    string
		prepare func(*fixture)
		save    bool
		other   bool
	}{
		{name: "dirty valid", save: true, other: true},
		{
			name: "invalid",
			prepare: func(source *fixture) {
				source.snapshot.Valid = false
				source.snapshot.CanSave = false
			},
			other: true,
		},
		{
			name: "saving",
			prepare: func(source *fixture) {
				source.snapshot.Saving = true
				source.snapshot.CanSave = false
			},
		},
		{
			name: "clean",
			prepare: func(source *fixture) {
				source.snapshot.Draft["name"] = source.snapshot.Saved["name"]
				source.snapshot.Dirty = false
				source.snapshot.CanSave = false
			},
			other: true,
		},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			source := newFixture(t)
			if test.prepare != nil {
				test.prepare(&source)
			}
			frame := capture(t, source)
			for _, node := range frame.Root.Children {
				if node.Action == "" {
					continue
				}
				expected := test.other
				if node.Action == "save" {
					expected = test.save
				}
				if node.ActionAvailable == nil || *node.ActionAvailable != expected {
					t.Fatalf("action %s availability=%v, expected %v", node.Action, node.ActionAvailable, expected)
				}
			}
		})
	}
}

func TestCaptureAdmissionRefusesInconsistentInputs(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*fixture)
	}{
		{
			name: "uncompiled document",
			mutate: func(source *fixture) {
				source.document = pipeview.Document{}
			},
		},
		{
			name: "different initial value",
			mutate: func(source *fixture) {
				source.definition.Initial["name"] = textValue("Different")
			},
		},
		{
			name: "missing definition action",
			mutate: func(source *fixture) {
				source.definition.Actions = source.definition.Actions[:3]
			},
		},
		{
			name: "unknown definition action",
			mutate: func(source *fixture) {
				source.definition.Actions[0].Name = "unknown"
			},
		},
		{
			name: "different control target",
			mutate: func(source *fixture) {
				source.definition.Actions[0].Target = "unicode"
			},
		},
		{
			name: "wrong payload kind",
			mutate: func(source *fixture) {
				source.definition.Actions[0].Payload = pipeview.Boolean
			},
		},
		{
			name: "missing draft",
			mutate: func(source *fixture) {
				source.snapshot.Draft = nil
			},
		},
		{
			name: "missing saved",
			mutate: func(source *fixture) {
				source.snapshot.Saved = nil
			},
		},
		{
			name: "unknown draft state",
			mutate: func(source *fixture) {
				delete(source.snapshot.Draft, "name")
				source.snapshot.Draft["unknown"] = textValue("Private")
			},
		},
		{
			name: "wrong saved type",
			mutate: func(source *fixture) {
				source.snapshot.Saved["name"] = pipeview.Value{Kind: pipeview.Boolean}
			},
		},
		{
			name: "noncanonical value",
			mutate: func(source *fixture) {
				source.snapshot.Draft["name"] = pipeview.Value{Kind: pipeview.Text, Integer: 1}
			},
		},
		{
			name: "invalid state UTF8",
			mutate: func(source *fixture) {
				source.snapshot.Draft["name"] = textValue(string([]byte{0xff}))
			},
		},
		{
			name: "excess state text",
			mutate: func(source *fixture) {
				source.snapshot.Draft["name"] = textValue(strings.Repeat("x", 4097))
			},
		},
		{
			name: "dirty contradiction",
			mutate: func(source *fixture) {
				source.snapshot.Dirty = false
			},
		},
		{
			name: "save availability contradiction",
			mutate: func(source *fixture) {
				source.snapshot.CanSave = false
			},
		},
		{
			name: "saving invalid state",
			mutate: func(source *fixture) {
				source.snapshot.Saving = true
				source.snapshot.Valid = false
				source.snapshot.CanSave = false
			},
		},
		{
			name: "saving clean state",
			mutate: func(source *fixture) {
				source.snapshot.Draft["name"] = source.snapshot.Saved["name"]
				source.snapshot.Dirty = false
				source.snapshot.Saving = true
				source.snapshot.CanSave = false
			},
		},
		{
			name: "excess status",
			mutate: func(source *fixture) {
				source.snapshot.Status = strings.Repeat("x", 4097)
			},
		},
		{
			name: "invalid status UTF8",
			mutate: func(source *fixture) {
				source.snapshot.Status = string([]byte{0xff})
			},
		},
		{
			name: "missing status binding",
			mutate: func(source *fixture) {
				source.statusBindings = nil
			},
		},
		{
			name: "unknown status owner",
			mutate: func(source *fixture) {
				source.statusBindings["save.status"] = "unknown"
			},
		},
		{
			name: "non-save status owner",
			mutate: func(source *fixture) {
				source.statusBindings["save.status"] = "revert"
			},
		},
		{
			name: "wrong derived identity",
			mutate: func(source *fixture) {
				source.statusBindings = map[string]string{"other.status": "save"}
			},
		},
		{
			name: "unknown exposure key",
			mutate: func(source *fixture) {
				source.policy.ExposeBindings = map[string]bool{"unknown": true}
			},
		},
		{
			name: "unknown false exposure key",
			mutate: func(source *fixture) {
				source.policy.ExposeBindings = map[string]bool{"unknown": false}
			},
		},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			source := newFixture(t)
			test.mutate(&source)
			frame, err := inspect.Capture(source.document, source.definition, source.snapshot, source.statusBindings, source.policy)
			if err == nil || !reflect.DeepEqual(frame, inspect.Frame{}) {
				t.Fatalf("bad input did not fail closed: frame=%+v error=%v", frame, err)
			}
		})
	}
}

func TestCaptureOwnsItsProjectionAndDoesNotMutateInputs(t *testing.T) {
	source := newFixture(t)
	source.policy.ExposeBindings = map[string]bool{"name": true}
	before := capture(t, source)
	expected := marshal(t, before)
	before.Root.Children[0].Text = "Changed control"
	*before.Root.Children[1].ActionAvailable = false
	before.Bindings["name"] = inspect.Binding{Value: "Changed binding"}
	before.Actions["nameEdited"] = inspect.Action{Operation: state.Revert}
	if actual := marshal(t, capture(t, source)); !bytes.Equal(actual, expected) {
		t.Fatal("editing a returned frame mutated source inputs")
	}
	owned := capture(t, source)
	source.snapshot.Draft["name"] = textValue("Later draft")
	source.snapshot.Saved["name"] = textValue("Later saved")
	source.definition.Initial["name"] = textValue("Later initial")
	source.definition.Actions[0].Name = "laterAction"
	source.policy.ExposeBindings["name"] = false
	source.statusBindings["save.status"] = "laterOwner"
	if actual := marshal(t, owned); !bytes.Equal(actual, expected) {
		t.Fatal("editing inputs mutated an existing frame")
	}
}

func TestDerivedStatusInitialAndWriteOwnership(t *testing.T) {
	definition := state.Definition{
		Initial: state.Values{"name": textValue("Rider")},
		Actions: []state.Action{
			{Name: "nameEdited", Payload: pipeview.Text, Operation: state.Set, Target: "name"},
			{Name: "save", Payload: pipeview.Invocation, Operation: state.Save},
		},
	}
	model, err := state.New(definition, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name   string
		status pipeview.Node
	}{
		{
			name:   "different initial status text",
			status: pipeview.Label("status", "", pipeview.State("save.status", "Different")),
		},
		{
			name:   "wrong derived type",
			status: pipeview.Label("status", "", pipeview.State("save.status", true)),
		},
		{
			name: "editable derived binding",
			status: pipeview.TextField("status", pipeview.State("save.status", "Saved"),
				pipeview.Change[string]("nameEdited")),
		},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			root := pipeview.Column("content",
				pipeview.Label("name", "", pipeview.State("name", "Rider")),
				test.status,
				pipeview.Button("save", "Save", pipeview.Click("save")),
			)
			// The non-editable status cases still declare the name-edit action.
			if test.name != "editable derived binding" {
				root = pipeview.Column("root", root,
					pipeview.TextField("name-field", pipeview.State("name", "Rider"),
						pipeview.Change[string]("nameEdited")),
				)
			}
			document, err := pipeview.Compile(root)
			if err != nil {
				t.Fatal(err)
			}
			frame, err := inspect.Capture(document, definition, model.Snapshot(),
				map[string]string{"save.status": "save"}, inspect.Policy{})
			if err == nil || !reflect.DeepEqual(frame, inspect.Frame{}) {
				t.Fatalf("invalid derived binding did not fail closed: frame=%+v error=%v", frame, err)
			}
		})
	}
}

func TestDeterministicJSONAndBoundedPresentationStatus(t *testing.T) {
	source := newFixture(t)
	source.snapshot.Status = "Presentation\x00状態"
	source.policy.ExposeBindings = map[string]bool{"name": true, "save.status": true, "enabled": false}
	first := marshal(t, capture(t, source))
	source.policy.ExposeBindings = map[string]bool{"enabled": false, "save.status": true, "name": true}
	second := marshal(t, capture(t, source))
	if !bytes.Equal(first, second) {
		t.Fatal("JSON changed with allowlist insertion order")
	}
	var decoded map[string]json.RawMessage
	if err := json.Unmarshal(first, &decoded); err != nil {
		t.Fatal(err)
	}
	if len(decoded) != 5 || decoded["schema"] == nil || decoded["root"] == nil || decoded["bindings"] == nil || decoded["actions"] == nil || decoded["model"] == nil {
		t.Fatal("unexpected top-level inspection schema")
	}
	source.snapshot.Status = strings.Repeat("é", 2048)
	if capture(t, source).Model.Status != source.snapshot.Status {
		t.Fatal("exact UTF8 status byte limit was not preserved")
	}
}
