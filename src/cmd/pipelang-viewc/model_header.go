package main

import (
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	viewsource "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
)

func cppString(value string) string {
	var result strings.Builder
	result.WriteString("std::string(\"")
	for _, item := range []byte(value) {
		fmt.Fprintf(&result, "\\x%02x", item)
	}
	fmt.Fprintf(&result, "\", %d)", len(value))
	return result.String()
}

func modelHeader(program viewsource.Program) ([]byte, error) {
	if program.Definition == nil {
		return nil, nil
	}
	var result strings.Builder
	result.WriteString("\n")
	result.WriteString(state.NativeRuntime())
	result.WriteString("\nnamespace compiled_ui {\ninline pipelang::pipeview::state::Definition model_definition()\n{\n")
	result.WriteString("    namespace model = pipelang::pipeview::state;\n    model::Definition definition;\n")
	identities := make([]string, 0, len(program.Definition.Initial))
	for identity := range program.Definition.Initial {
		identities = append(identities, identity)
	}
	sort.Strings(identities)
	for _, identity := range identities {
		value := program.Definition.Initial[identity]
		literal := ""
		switch value.Kind {
		case pipeview.Boolean:
			literal = strconv.FormatBool(value.Boolean)
		case pipeview.Integer:
			integer := strconv.FormatInt(value.Integer, 10) + "LL"
			if value.Integer == -9223372036854775808 {
				integer = "(-9223372036854775807LL - 1LL)"
			}
			literal = "std::int64_t{" + integer + "}"
		case pipeview.Text:
			literal = cppString(value.Text)
		default:
			return nil, fmt.Errorf("unsupported model state kind %s", value.Kind)
		}
		fmt.Fprintf(&result, "    definition.initial.emplace(%s, %s);\n", cppString(identity), literal)
	}
	kinds := map[pipeview.ValueKind]string{pipeview.Boolean: "Boolean", pipeview.Integer: "Integer", pipeview.Text: "Text", pipeview.Invocation: "Invocation"}
	operations := map[state.Operation]string{state.Set: "Set", state.Save: "Save", state.Revert: "Revert"}
	for _, action := range program.Definition.Actions {
		kind, kindOK := kinds[action.Payload]
		operation, operationOK := operations[action.Operation]
		if !kindOK || !operationOK {
			return nil, fmt.Errorf("unsupported model action %s", action.Name)
		}
		fmt.Fprintf(&result, "    definition.actions.push_back(model::Action{%s, model::ValueKind::%s, model::Operation::%s, %s});\n", cppString(action.Name), kind, operation, cppString(action.Target))
	}
	result.WriteString("    return definition;\n}\ninline std::map<std::string, std::string> status_bindings()\n{\n    return {\n")
	identities = identities[:0]
	for identity := range program.StatusBindings {
		identities = append(identities, identity)
	}
	sort.Strings(identities)
	for _, identity := range identities {
		fmt.Fprintf(&result, "        {%s, %s},\n", cppString(identity), cppString(program.StatusBindings[identity]))
	}
	result.WriteString("    };\n}\n}\n")
	return []byte(result.String()), nil
}
