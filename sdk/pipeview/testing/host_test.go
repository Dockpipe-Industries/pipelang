package testing_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/inspect"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	viewtest "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/testing"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func settingsProgram(t *testing.T) source.Program {
	t.Helper()
	path := filepath.Join("..", "..", "..", "examples", "settings", "SettingsView.pipe")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	sources, diagnostics := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: "SettingsView.pipe", Data: data}})
	if diagnostics.HasErrors() {
		t.Fatal(diagnostics)
	}
	program, err := source.CompileProgram(sources.Files()[0], nil)
	if err != nil {
		t.Fatal(err)
	}
	return program
}

type settingsHarness struct {
	host   *viewtest.Host
	name   viewtest.TextField
	vsync  viewtest.Toggle
	save   viewtest.Button
	revert viewtest.Button
}

func mountSettings(t *testing.T, options viewtest.Options) settingsHarness {
	t.Helper()
	program := settingsProgram(t)
	host, err := viewtest.Mount(program.Document, *program.Definition, program.StatusBindings, options)
	if err != nil {
		t.Fatal(err)
	}
	name, err := host.TextField("name-field")
	if err != nil {
		t.Fatal(err)
	}
	vsync, err := host.Toggle("vsync-toggle")
	if err != nil {
		t.Fatal(err)
	}
	save, err := host.Button("save-button")
	if err != nil {
		t.Fatal(err)
	}
	revert, err := host.Button("revert-button")
	if err != nil {
		t.Fatal(err)
	}
	return settingsHarness{host: host, name: name, vsync: vsync, save: save, revert: revert}
}

func findControl(t *testing.T, root inspect.Node, identity string) inspect.Node {
	t.Helper()
	var find func(inspect.Node) (inspect.Node, bool)
	find = func(node inspect.Node) (inspect.Node, bool) {
		if node.ID == identity {
			return node, true
		}
		for _, child := range node.Children {
			if match, found := find(child); found {
				return match, true
			}
		}
		return inspect.Node{}, false
	}
	node, found := find(root)
	if !found {
		t.Fatalf("control %q is absent from inspection", identity)
	}
	return node
}

func TestSettingsWidgetsDriveRealModelSaveAndRevert(t *testing.T) {
	widgets := mountSettings(t, viewtest.Options{})
	initial := widgets.host.Snapshot()
	if initial.Dirty || !initial.Valid || initial.Saving || initial.CanSave || initial.Status != "Saved" {
		t.Fatalf("incorrect initial state: %#v", initial)
	}
	if request, err := widgets.save.Invoke(); err == nil || request != nil {
		t.Fatal("clean state unexpectedly produced a save request")
	}
	if err := widgets.name.Change("Alex"); err != nil {
		t.Fatal(err)
	}
	if err := widgets.vsync.Change(false); err != nil {
		t.Fatal(err)
	}
	edited := widgets.host.Snapshot()
	if edited.Draft["playerName"].Text != "Alex" || edited.Draft["vsync"].Boolean || !edited.Dirty || !edited.Valid || !edited.CanSave || edited.Status != "Unsaved changes" {
		t.Fatalf("typed widget changes did not reach the model: %#v", edited)
	}
	request, err := widgets.save.Invoke()
	if err != nil || request == nil || request.ID == 0 || !reflect.DeepEqual(request.Snapshot, edited.Draft) {
		t.Fatalf("save did not capture its draft: %#v, %v", request, err)
	}
	pending := widgets.host.Snapshot()
	if !pending.Saving || !pending.Dirty || pending.CanSave || pending.Status != "Saving" {
		t.Fatalf("save did not enter pending state: %#v", pending)
	}
	if err := widgets.name.Change("Blocked"); err == nil {
		t.Fatal("text input changed during pending save")
	}
	if err := widgets.vsync.Change(true); err == nil {
		t.Fatal("toggle changed during pending save")
	}
	if _, err := widgets.save.Invoke(); err == nil {
		t.Fatal("second save started while pending")
	}
	if _, err := widgets.revert.Invoke(); err == nil {
		t.Fatal("revert ran during pending save")
	}
	if err := widgets.host.Complete(request.ID+1, state.Success); err == nil {
		t.Fatal("stale completion was accepted")
	}
	if err := widgets.host.Complete(request.ID, state.Outcome("unknown")); err == nil {
		t.Fatal("unknown completion outcome was accepted")
	}
	if !reflect.DeepEqual(widgets.host.Snapshot(), pending) {
		t.Fatal("rejected input or completion mutated the pending save")
	}
	frame, err := widgets.host.Inspect(inspect.Policy{ExposeBindings: map[string]bool{"save.status": true}})
	if err != nil {
		t.Fatal(err)
	}
	if !frame.Model.Saving || frame.Model.CanSave || frame.Bindings["save.status"].Value != "Saving" {
		t.Fatalf("inspection did not project pending save: %#v", frame)
	}
	for _, identity := range []string{"name-field", "vsync-toggle", "save-button", "revert-button"} {
		control := findControl(t, frame.Root, identity)
		if control.ActionAvailable == nil || *control.ActionAvailable {
			t.Fatalf("pending control action remained available: %#v", control)
		}
	}
	request.Snapshot["playerName"] = pipeview.Value{Kind: pipeview.Text, Text: "Caller mutation"}
	if err := widgets.host.Complete(request.ID, state.Success); err != nil {
		t.Fatal(err)
	}
	saved := widgets.host.Snapshot()
	if saved.Saved["playerName"].Text != "Alex" || saved.Saved["vsync"].Boolean || saved.Dirty || saved.Saving || saved.CanSave || saved.Status != "Saved" {
		t.Fatalf("successful save did not preserve its captured draft: %#v", saved)
	}
	if err := widgets.host.Complete(request.ID, state.Success); err == nil {
		t.Fatal("duplicate completion was accepted")
	}
	if err := widgets.name.Change("Later edit"); err != nil {
		t.Fatal(err)
	}
	if request, err := widgets.revert.Invoke(); err != nil || request != nil {
		t.Fatalf("revert unexpectedly produced a save request: %#v, %v", request, err)
	}
	if !reflect.DeepEqual(widgets.host.Snapshot(), saved) {
		t.Fatal("revert did not return to the last successful save")
	}
}

func TestSettingsSaveFailuresAndCancellationKeepSavedBaseline(t *testing.T) {
	for _, test := range []struct {
		outcome state.Outcome
		status  string
	}{
		{state.Failure, "Save failed"},
		{state.Cancelled, "Save cancelled"},
	} {
		t.Run(string(test.outcome), func(t *testing.T) {
			widgets := mountSettings(t, viewtest.Options{})
			if err := widgets.name.Change("Unsaved"); err != nil {
				t.Fatal(err)
			}
			request, err := widgets.save.Invoke()
			if err != nil {
				t.Fatal(err)
			}
			if err := widgets.host.Complete(request.ID, test.outcome); err != nil {
				t.Fatal(err)
			}
			snapshot := widgets.host.Snapshot()
			if snapshot.Saved["playerName"].Text != "Rider" || snapshot.Draft["playerName"].Text != "Unsaved" || !snapshot.Dirty || snapshot.Saving || !snapshot.CanSave || snapshot.Status != test.status {
				t.Fatalf("failed save changed baseline or lost draft: %#v", snapshot)
			}
			if _, err := widgets.revert.Invoke(); err != nil {
				t.Fatal(err)
			}
			if snapshot := widgets.host.Snapshot(); snapshot.Draft["playerName"].Text != "Rider" || snapshot.Dirty || snapshot.Status != "Saved" {
				t.Fatalf("revert failed after %s: %#v", test.outcome, snapshot)
			}
		})
	}
}

func TestSettingsValidatorAndSavedOptionsUseOwningModelRules(t *testing.T) {
	saved := state.Values{
		"playerName": {Kind: pipeview.Text, Text: "Loaded rider"},
		"vsync":      {Kind: pipeview.Boolean, Boolean: false},
		"volume":     {Kind: pipeview.Integer, Integer: 100},
	}
	widgets := mountSettings(t, viewtest.Options{
		Saved: saved,
		Validate: func(values state.Values) bool {
			return values["playerName"].Text != ""
		},
	})
	saved["playerName"] = pipeview.Value{Kind: pipeview.Text, Text: "Caller mutation"}
	initial := widgets.host.Snapshot()
	if initial.Draft["playerName"].Text != "Loaded rider" || initial.Draft["vsync"].Boolean || initial.Dirty {
		t.Fatalf("saved option was ignored or aliased: %#v", initial)
	}
	if err := widgets.name.Change(""); err != nil {
		t.Fatal(err)
	}
	invalid := widgets.host.Snapshot()
	if invalid.Valid || invalid.CanSave || !invalid.Dirty || invalid.Status != "Invalid settings" {
		t.Fatalf("invalid name did not reach domain validation: %#v", invalid)
	}
	if request, err := widgets.save.Invoke(); err == nil || request != nil {
		t.Fatal("invalid draft started a save")
	}
	if !reflect.DeepEqual(widgets.host.Snapshot(), invalid) {
		t.Fatal("rejected save mutated the invalid draft")
	}
	if _, err := widgets.revert.Invoke(); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(widgets.host.Snapshot(), initial) {
		t.Fatal("revert ignored the loaded baseline")
	}
	for _, value := range []string{"\xff", strings.Repeat("x", pipeview.MaximumTextBytes+1)} {
		if err := widgets.name.Change(value); err == nil {
			t.Fatal("noncanonical or excessive text was accepted")
		}
		if !reflect.DeepEqual(widgets.host.Snapshot(), initial) {
			t.Fatal("malformed text mutated the model")
		}
	}
}

func TestWidgetLookupRefusesUnknownIDsWrongKindsAndZeroHandles(t *testing.T) {
	widgets := mountSettings(t, viewtest.Options{})
	for _, identity := range []string{"missing", "save-button", "vsync-toggle", "save-status", "root"} {
		if _, err := widgets.host.TextField(identity); err == nil {
			t.Fatalf("TextField admitted %q", identity)
		}
	}
	for _, identity := range []string{"missing", "name-field", "save-button", "save-status"} {
		if _, err := widgets.host.Toggle(identity); err == nil {
			t.Fatalf("Toggle admitted %q", identity)
		}
	}
	for _, identity := range []string{"missing", "name-field", "vsync-toggle", "save-status"} {
		if _, err := widgets.host.Button(identity); err == nil {
			t.Fatalf("Button admitted %q", identity)
		}
	}
	if err := (viewtest.TextField{}).Change("value"); err == nil {
		t.Fatal("zero TextField handle accepted input")
	}
	if err := (viewtest.Toggle{}).Change(true); err == nil {
		t.Fatal("zero Toggle handle accepted input")
	}
	if _, err := (viewtest.Button{}).Invoke(); err == nil {
		t.Fatal("zero Button handle accepted invocation")
	}
}

func TestInspectionDefaultsToRedactionAndHostOwnsAdmissionCopies(t *testing.T) {
	program := settingsProgram(t)
	host, err := viewtest.Mount(program.Document, *program.Definition, program.StatusBindings, viewtest.Options{})
	if err != nil {
		t.Fatal(err)
	}
	program.Definition.Initial["playerName"] = pipeview.Value{Kind: pipeview.Text, Text: "External mutation"}
	program.Definition.Actions[0].Target = "missing"
	program.StatusBindings["save.status"] = "missing"
	frame, err := host.Inspect(inspect.Policy{})
	if err != nil {
		t.Fatalf("caller mutated admitted host data: %v", err)
	}
	binding := frame.Bindings["playerName"]
	if binding.Type != pipeview.Text || !binding.Redacted || binding.Value != nil {
		t.Fatalf("binding was exposed by default: %#v", binding)
	}
	if frame.Model.Status != "Saved" || !frame.Bindings["save.status"].ReadOnly {
		t.Fatalf("model status or readonly semantics missing: %#v", frame)
	}
	field, err := host.TextField("name-field")
	if err != nil {
		t.Fatal(err)
	}
	if err := field.Change("Private rider"); err != nil {
		t.Fatal(err)
	}
	privateFrame, err := host.Inspect(inspect.Policy{})
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := json.Marshal(privateFrame)
	if err != nil || strings.Contains(string(encoded), "Private rider") {
		t.Fatalf("default inspection leaked private binding text: %s, %v", encoded, err)
	}
	frame, err = host.Inspect(inspect.Policy{ExposeBindings: map[string]bool{"playerName": true}})
	if err != nil {
		t.Fatal(err)
	}
	if frame.Bindings["playerName"].Value != "Private rider" || frame.Bindings["playerName"].Redacted || !frame.Bindings["vsync"].Redacted {
		t.Fatalf("explicit value policy did not remain scoped: %#v", frame.Bindings)
	}
	frame.Bindings["playerName"] = inspect.Binding{}
	frame.Actions["nameEdited"] = inspect.Action{}
	frame.Root.Children = nil
	snapshot := host.Snapshot()
	snapshot.Draft["playerName"] = pipeview.Value{Kind: pipeview.Text, Text: "Snapshot mutation"}
	fresh, err := host.Inspect(inspect.Policy{})
	if err != nil || len(fresh.Root.Children) == 0 || fresh.Actions["nameEdited"].Target != "playerName" || !fresh.Bindings["playerName"].Redacted {
		t.Fatalf("inspection output leaked mutation into host: %#v, %v", fresh, err)
	}
	if host.Snapshot().Draft["playerName"].Text != "Private rider" {
		t.Fatal("snapshot map leaked mutation into host")
	}
}

func TestMountRefusesMismatchedDeclarationsActionsAndStatusOwners(t *testing.T) {
	for _, test := range []string{"zero document", "hidden action", "wrong action payload", "wrong action target", "wrong status owner", "missing status map", "extra status map", "computed input"} {
		t.Run(test, func(t *testing.T) {
			program := settingsProgram(t)
			switch test {
			case "zero document":
				program.Document = pipeview.Document{}
			case "hidden action":
				program.Definition.Actions = append(program.Definition.Actions, state.Action{
					Name: "hidden", Payload: pipeview.Text, Operation: state.Set, Target: "playerName",
				})
			case "wrong action payload":
				program.Definition.Actions[0].Payload = pipeview.Boolean
				program.Definition.Actions[0].Target = "vsync"
			case "wrong action target":
				program.Definition.Initial["otherName"] = pipeview.Value{Kind: pipeview.Text, Text: "Other"}
				program.Definition.Actions[0].Target = "otherName"
			case "wrong status owner":
				program.StatusBindings["save.status"] = "revert"
			case "missing status map":
				program.StatusBindings = nil
			case "extra status map":
				program.StatusBindings["unknown.status"] = "save"
			case "computed input":
				document, err := pipeview.Compile(pipeview.Column("root",
					pipeview.Label("name", "", pipeview.State("playerName", "Rider")),
					pipeview.TextField("status-input", pipeview.State("save.status", "Saved"), pipeview.Change[string]("nameEdited")),
					pipeview.Toggle("toggle", "", pipeview.State("vsync", true), pipeview.Change[bool]("vsyncEdited")),
					pipeview.Button("save", "", pipeview.Click("save")),
					pipeview.Button("revert", "", pipeview.Click("revert")),
				))
				if err != nil {
					t.Fatal(err)
				}
				program.Document = document
			}
			if host, err := viewtest.Mount(program.Document, *program.Definition, program.StatusBindings, viewtest.Options{}); err == nil || host != nil {
				t.Fatalf("mismatched wiring was mounted: %#v, %v", host, err)
			}
		})
	}
}
