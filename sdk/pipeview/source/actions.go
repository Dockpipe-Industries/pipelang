package source

import (
	"strings"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/state"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

// ActionContract identifies the explicit typed state/action source profile.
// Contract continues to identify the event-only source profile.
const ActionContract = "pipelang.pipeview.source.v2"

// Program includes the portable declaration and its optional state runtime plan.
// StatusBindings maps computed binding identities to their declared save action.
// Legacy event views have no Definition or StatusBindings.
type Program struct {
	Document       pipeview.Document
	Definition     *state.Definition
	StatusBindings map[string]string
}

// CompileProgram admits either legacy events or closed typed state actions.
// Compilation performs no rendering, persistence or host effects.
func CompileProgram(file *pipelang.SourceFile, lexer pipelang.SourceLexer) (Program, error) {
	return CompileProgramWithParser(file, lexer, nil)
}

// CompileProgramWithParser selects an explicit syntax component; semantic checks are unchanged.
func CompileProgramWithParser(file *pipelang.SourceFile, lexer pipelang.SourceLexer, parser pipelang.ViewSourceParser) (Program, error) {
	declaration, err := pipelang.ParseViewSourceWithParser(file, lexer, parser)
	if err != nil {
		return Program{}, err
	}
	if len(declaration.Actions) == 0 {
		document, err := compileDeclaration(file, declaration, nil)
		if err != nil {
			return Program{}, err
		}
		return Program{Document: document}, nil
	}
	check := checker{file: file}
	if len(declaration.Events) != 0 {
		return Program{}, check.failure(declaration.Events[0].Span, "view cannot mix event and action declarations")
	}

	definition := state.Definition{Initial: make(state.Values)}
	declaredStates := make(map[string]pipelang.ViewState)
	for _, declared := range declaration.State {
		if _, duplicate := declaredStates[declared.Name]; duplicate {
			return Program{}, check.failure(declared.Span, "duplicate declaration "+declared.Name)
		}
		declaredStates[declared.Name] = declared
		definition.Initial[declared.Name] = initialValue(declared)
	}
	actions := make(map[string]pipelang.ViewAction)
	lowered := *declaration
	lowered.State = append([]pipelang.ViewState(nil), declaration.State...)
	for _, declared := range declaration.Actions {
		if _, collision := declaredStates[declared.Name]; collision {
			return Program{}, check.failure(declared.Span, "duplicate declaration "+declared.Name)
		}
		if _, duplicate := actions[declared.Name]; duplicate {
			return Program{}, check.failure(declared.Span, "duplicate declaration "+declared.Name)
		}
		actions[declared.Name] = declared
		payload, valid := payloadKind(declared.Payload)
		if !valid {
			return Program{}, check.failure(declared.Span, "unsupported action payload "+declared.Payload)
		}
		operation := state.Operation(declared.Operation)
		if operation == state.Set {
			target, found := declaredStates[declared.Target]
			if !found {
				return Program{}, check.failure(declared.Span, "unknown action state target "+declared.Target)
			}
			if declared.Payload == "" || declared.Payload != target.Type {
				return Program{}, check.failure(declared.Span, "set action payload must match target state type")
			}
		} else if declared.Payload != "" || declared.Target != "" {
			return Program{}, check.failure(declared.Span, "save and revert actions require no payload or target")
		}
		definition.Actions = append(definition.Actions, state.Action{
			Name: declared.Name, Payload: payload, Operation: operation, Target: declared.Target,
		})
		lowered.Events = append(lowered.Events, pipelang.ViewEvent{
			Name: declared.Name, Payload: declared.Payload, Span: declared.Span,
		})
	}

	statusBindings := make(map[string]string)
	readOnly := make(map[string]bool)
	if err := check.collectStatusBindings(declaration.Root, actions, &lowered, statusBindings, readOnly); err != nil {
		return Program{}, err
	}
	document, err := compileDeclaration(file, &lowered, readOnly)
	if err != nil {
		return Program{}, err
	}
	if _, err := state.New(definition, nil, nil); err != nil {
		return Program{}, check.failure(declaration.Span, err.Error())
	}
	snapshot, err := document.Inspect()
	if err != nil {
		return Program{}, check.failure(declaration.Root.Span, err.Error())
	}
	for _, action := range declaration.Actions {
		if action.Operation == "set" && snapshot.Events[action.Name].Target != action.Target {
			return Program{}, check.failure(action.Span, "set action target must match its control binding")
		}
	}
	return Program{Document: document, Definition: &definition, StatusBindings: statusBindings}, nil
}

func initialValue(declared pipelang.ViewState) pipeview.Value {
	switch declared.Type {
	case "bool":
		return pipeview.Value{Kind: pipeview.Boolean, Boolean: declared.Value.Boolean}
	case "int":
		return pipeview.Value{Kind: pipeview.Integer, Integer: declared.Value.Integer}
	case "string":
		return pipeview.Value{Kind: pipeview.Text, Text: declared.Value.Text}
	default:
		return pipeview.Value{}
	}
}

func payloadKind(payload string) (pipeview.ValueKind, bool) {
	switch payload {
	case "":
		return pipeview.Invocation, true
	case "bool":
		return pipeview.Boolean, true
	case "int":
		return pipeview.Integer, true
	case "string":
		return pipeview.Text, true
	default:
		return "", false
	}
}

func (check *checker) collectStatusBindings(element pipelang.ViewElement, actions map[string]pipelang.ViewAction,
	declaration *pipelang.ViewDeclaration, bindings map[string]string, readOnly map[string]bool) error {
	for _, property := range element.Properties {
		value := property.Value
		if value.Kind != "binding" || !strings.Contains(value.Text, ".") {
			continue
		}
		actionName, member, _ := strings.Cut(value.Text, ".")
		action, found := actions[actionName]
		if !found || action.Operation != "save" || member != "status" {
			return check.failure(value.Span, "computed binding requires a declared save action followed by .status")
		}
		if element.Component != "Label" || property.Name != "value" {
			return check.failure(value.Span, "computed status binding is read-only and requires Label value")
		}
		if readOnly[value.Text] {
			continue
		}
		readOnly[value.Text] = true
		bindings[value.Text] = actionName
		declaration.State = append(declaration.State, pipelang.ViewState{
			Name: value.Text, Type: "string", Span: value.Span,
			Value: pipelang.ViewValue{Kind: "text", Text: "Saved", Span: value.Span},
		})
	}
	for _, child := range element.Children {
		if err := check.collectStatusBindings(child, actions, declaration, bindings, readOnly); err != nil {
			return err
		}
	}
	return nil
}
