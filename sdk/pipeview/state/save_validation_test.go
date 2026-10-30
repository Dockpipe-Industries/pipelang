package state_test

import (
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
)

func TestSaveRevalidatesCapturedCandidate(t *testing.T) {
	definition := state.Definition{
		Initial: state.Values{"name": {Kind: pipeview.Text, Text: "Rider"}},
		Actions: []state.Action{
			{Name: "edit", Payload: pipeview.Text, Operation: state.Set, Target: "name"},
			{Name: "save", Payload: pipeview.Invocation, Operation: state.Save},
		},
	}
	calls := 0
	model, err := state.New(definition, nil, func(values state.Values) bool {
		calls++
		return values["name"].Text != ""
	})
	if err != nil {
		t.Fatal(err)
	}
	edited := pipeview.Value{Kind: pipeview.Text, Text: "RiderX"}
	if _, err := model.Dispatch("edit", &edited); err != nil {
		t.Fatal(err)
	}
	beforeSave := calls
	request, err := model.Dispatch("save", nil)
	if err != nil || request == nil {
		t.Fatalf("Save failed: %v", err)
	}
	if calls != beforeSave+1 || request.Snapshot["name"].Text != "RiderX" {
		t.Fatalf("Save did not revalidate the captured candidate: calls=%d before=%d", calls, beforeSave)
	}
}
