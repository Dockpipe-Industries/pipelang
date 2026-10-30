package state_test

import (
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	"github.com/Dockpipe-Industries/pipelang/tests/containedexec"
)

func TestPercentNativeModelTrace(t *testing.T) {
	compiler := os.Getenv("PIPEVIEW_PERCENT_CXX")
	if compiler == "" {
		t.Skip("explicit contained C++ compiler required")
	}
	if !filepath.IsAbs(compiler) {
		t.Fatal("absolute C++ compiler required")
	}
	definition := state.Definition{Initial: state.Values{"volume": {Kind: pipeview.Integer, Integer: 100}}, Actions: []state.Action{
		{Name: "edit", Payload: pipeview.Integer, Operation: state.Set, Target: "volume"},
		{Name: "save", Payload: pipeview.Invocation, Operation: state.Save},
		{Name: "revert", Payload: pipeview.Invocation, Operation: state.Revert},
	}}
	model, err := state.New(definition, nil, func(values state.Values) bool {
		value := values["volume"].Integer
		return value >= 0 && value <= 100
	})
	if err != nil {
		t.Fatal(err)
	}
	steps := []string{
		"save", "edit " + strconv.FormatInt(math.MinInt64, 10), "save", "edit -1", "revert",
		"edit 0", "save", "edit 99", "revert", "complete 2 success", "complete 1 failure",
		"save", "complete 1 success", "complete 2 cancelled", "save", "complete 3 success",
		"edit 101", "save", "edit " + strconv.FormatInt(math.MaxInt64, 10), "revert",
		"edit 37", "save", "complete 4 success", "edit 72", "revert",
	}
	var checks strings.Builder
	for index, step := range steps {
		fields := strings.Fields(step)
		var request *state.SaveRequest
		var operationError error
		operation := ""
		if fields[0] == "edit" {
			value, err := strconv.ParseInt(fields[1], 10, 64)
			if err != nil {
				t.Fatal(err)
			}
			payload := pipeview.Value{Kind: pipeview.Integer, Integer: value}
			request, operationError = model.Dispatch("edit", &payload)
			operation = `model->dispatch("edit", state::Value{static_cast<std::int64_t>(std::stoll("` + fields[1] + `"))}, request, error)`
		} else if fields[0] == "complete" {
			id, err := strconv.ParseUint(fields[1], 10, 64)
			if err != nil {
				t.Fatal(err)
			}
			operationError = model.Complete(id, state.Outcome(fields[2]))
			outcome := map[string]string{"success": "Success", "failure": "Failure", "cancelled": "Cancelled"}[fields[2]]
			operation = "model->complete(" + fields[1] + ", state::Outcome::" + outcome + ", error)"
		} else {
			request, operationError = model.Dispatch(fields[0], nil)
			operation = `model->dispatch("` + fields[0] + `", std::nullopt, request, error)`
		}
		snapshot := model.Snapshot()
		fmt.Fprintf(&checks, "    {\n        std::optional<state::SaveRequest> request;\n        const bool accepted = %s;\n        const auto snapshot = model->snapshot();\n", operation)
		fmt.Fprintf(&checks, "        if (accepted != %t || std::get<std::int64_t>(snapshot.draft.at(\"volume\")) != std::stoll(\"%d\")\n            || std::get<std::int64_t>(snapshot.saved.at(\"volume\")) != std::stoll(\"%d\")\n", operationError == nil, snapshot.Draft["volume"].Integer, snapshot.Saved["volume"].Integer)
		fmt.Fprintf(&checks, "            || snapshot.dirty != %t || snapshot.valid != %t || snapshot.saving != %t\n            || snapshot.canSave != %t || snapshot.status != %q)\n            return %d;\n", snapshot.Dirty, snapshot.Valid, snapshot.Saving, snapshot.CanSave, snapshot.Status, index+10)
		if request == nil {
			checks.WriteString("        if (request) return 90;\n")
		} else {
			fmt.Fprintf(&checks, "        if (!request || request->id != %d || std::get<std::int64_t>(request->snapshot.at(\"volume\")) != %d) return 91;\n", request.ID, request.Snapshot["volume"].Integer)
		}
		checks.WriteString("    }\n")
	}
	main := `#include "runtime.hpp"
namespace state = pipelang::pipeview::state;
int main() {
    state::Definition definition;
    definition.initial.emplace("volume", std::int64_t{100});
    definition.actions = {{"edit", state::ValueKind::Integer, state::Operation::Set, "volume"},
        {"save", state::ValueKind::Invocation, state::Operation::Save, ""},
        {"revert", state::ValueKind::Invocation, state::Operation::Revert, ""}};
    std::string error;
    auto model = state::Model::create(definition, std::nullopt, [](const state::Values& values) {
        const auto value = std::get<std::int64_t>(values.at("volume"));
        return value >= 0 && value <= 100;
    }, error);
    if (!model)
        return 1;
` + checks.String() + "}\n"
	directory := t.TempDir()
	for name, data := range map[string]string{"runtime.hpp": state.NativeRuntime(), "main.cpp": main} {
		if err := os.WriteFile(filepath.Join(directory, name), []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
	}
	binary := filepath.Join(directory, "model")
	if output, err := containedexec.CombinedOutput(exec.Command(compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", filepath.Join(directory, "main.cpp"), "-o", binary)); err != nil {
		t.Fatalf("native percent model compile: %v %s", err, output)
	}
	if output, err := containedexec.CombinedOutput(exec.Command(binary)); err != nil {
		t.Fatalf("native percent model differs from Go trace: %v %s", err, output)
	}
}
