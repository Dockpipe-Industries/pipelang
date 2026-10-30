// Package source checks the experimental PipeLang view syntax against the
// optional PipeView component profile. Compiler syntax stays compiler-owned;
// rendering, host effects and state mutation stay outside this package.
package source

import (
	"fmt"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

const Contract = "pipelang.pipeview.source.v1"

type checker struct {
	file          *pipelang.SourceFile
	states        map[string]pipelang.ViewState
	events        map[string]pipelang.ViewEvent
	usedStates    map[string]bool
	usedEvents    map[string]bool
	controls      map[string]bool
	readOnly      map[string]bool
	actionProfile bool
}

// Compile parses and checks a single explicit view source. The default lexer is
// the Go reference. Selecting a foreign lexer never supplies component semantics.
func Compile(file *pipelang.SourceFile, lexer pipelang.SourceLexer) (pipeview.Document, error) {
	return CompileWithParser(file, lexer, nil)
}

// CompileWithParser selects an explicit syntax component; semantic checks are unchanged.
func CompileWithParser(file *pipelang.SourceFile, lexer pipelang.SourceLexer, parser pipelang.ViewSourceParser) (pipeview.Document, error) {
	declaration, err := pipelang.ParseViewSourceWithParser(file, lexer, parser)
	if err != nil {
		return pipeview.Document{}, err
	}
	if len(declaration.Actions) != 0 {
		check := checker{file: file}
		return pipeview.Document{}, check.failure(declaration.Actions[0].Span, "action views require CompileProgram")
	}
	return compileDeclaration(file, declaration, nil)
}

func compileDeclaration(file *pipelang.SourceFile, declaration *pipelang.ViewDeclaration, readOnly map[string]bool) (pipeview.Document, error) {
	check := checker{file: file, readOnly: readOnly, actionProfile: len(declaration.Actions) != 0, states: make(map[string]pipelang.ViewState),
		events: make(map[string]pipelang.ViewEvent), usedStates: make(map[string]bool),
		usedEvents: make(map[string]bool), controls: make(map[string]bool)}
	names := make(map[string]bool)
	for _, state := range declaration.State {
		if names[state.Name] {
			return pipeview.Document{}, check.failure(state.Span, "duplicate declaration "+state.Name)
		}
		names[state.Name] = true
		expected := ""
		switch state.Type {
		case "bool":
			expected = "boolean"
		case "int":
			expected = "integer"
		case "string":
			expected = "text"
		default:
			return pipeview.Document{}, check.failure(state.Span, "unsupported view state type "+state.Type)
		}
		if state.Value.Kind != expected {
			return pipeview.Document{}, check.failure(state.Value.Span, "initial value does not match "+state.Type)
		}
		check.states[state.Name] = state
	}
	for _, event := range declaration.Events {
		if names[event.Name] {
			return pipeview.Document{}, check.failure(event.Span, "duplicate declaration "+event.Name)
		}
		names[event.Name] = true
		if event.Payload != "" && event.Payload != "bool" && event.Payload != "int" && event.Payload != "string" {
			return pipeview.Document{}, check.failure(event.Span, "unsupported event payload "+event.Payload)
		}
		check.events[event.Name] = event
	}
	root, err := check.element(declaration.Root)
	if err != nil {
		return pipeview.Document{}, err
	}
	for _, state := range declaration.State {
		if !check.usedStates[state.Name] {
			return pipeview.Document{}, check.failure(state.Span, "state is not bound by this view: "+state.Name)
		}
	}
	for _, event := range declaration.Events {
		if !check.usedEvents[event.Name] {
			return pipeview.Document{}, check.failure(event.Span, check.interactionName()+" is not used by this view: "+event.Name)
		}
	}
	document, err := pipeview.Compile(root)
	if err != nil {
		return pipeview.Document{}, check.failure(declaration.Root.Span, err.Error())
	}
	return document, nil
}

func (check *checker) failure(span pipelang.Span, message string) error {
	sources, _ := pipelang.NewSourceSet([]pipelang.SourceInput{{Path: string(check.file.ID), Data: []byte(check.file.Text)}})
	return &pipelang.DiagnosticError{Sources: sources, Diagnostics: pipelang.Diagnostics{{
		Code: pipelang.CodeExpressionType, Category: pipelang.CategorySemantic,
		Severity: pipelang.SeverityError, Message: message, Primary: span,
	}}}
}

func (check *checker) element(element pipelang.ViewElement) (pipeview.Node, error) {
	allowed := map[string]bool{"id": true}
	container := element.Component == "Column" || element.Component == "Row"
	switch element.Component {
	case "Column", "Row":
	case "Label":
		allowed["text"] = true
		allowed["value"] = true
	case "Button":
		allowed["label"] = true
		allowed["clicked"] = true
	case "Toggle", "PercentStepper":
		allowed["label"] = true
		allowed["value"] = true
		allowed["changed"] = true
	case "TextField":
		allowed["value"] = true
		allowed["changed"] = true
	default:
		return pipeview.Node{}, check.failure(element.Span, "unsupported PipeView component "+element.Component)
	}
	properties := make(map[string]pipelang.ViewValue)
	for _, property := range element.Properties {
		if !allowed[property.Name] {
			return pipeview.Node{}, check.failure(property.Span, "unsupported property "+property.Name+" on "+element.Component)
		}
		if _, duplicate := properties[property.Name]; duplicate {
			return pipeview.Node{}, check.failure(property.Span, "duplicate property "+property.Name)
		}
		properties[property.Name] = property.Value
	}
	text := func(name string, required bool) (string, error) {
		value, found := properties[name]
		if !found {
			if required {
				return "", check.failure(element.Span, "missing property "+name)
			}
			return "", nil
		}
		if value.Kind != "text" {
			return "", check.failure(value.Span, name+" requires a text literal")
		}
		return value.Text, nil
	}
	identity, err := text("id", true)
	if err != nil {
		return pipeview.Node{}, err
	}
	if check.controls[identity] {
		return pipeview.Node{}, check.failure(properties["id"].Span, "duplicate control identity "+identity)
	}
	check.controls[identity] = true
	if container {
		children := make([]pipeview.Node, 0, len(element.Children))
		for _, child := range element.Children {
			node, err := check.element(child)
			if err != nil {
				return pipeview.Node{}, err
			}
			children = append(children, node)
		}
		if element.Component == "Column" {
			return pipeview.Column(identity, children...), nil
		}
		return pipeview.Row(identity, children...), nil
	}
	if len(element.Children) != 0 {
		return pipeview.Node{}, check.failure(element.Span, "leaf component cannot contain children")
	}
	labelProperty := "label"
	if element.Component == "Label" {
		labelProperty = "text"
	}
	label, err := text(labelProperty, false)
	if err != nil {
		return pipeview.Node{}, err
	}
	if element.Component == "Button" {
		event, err := check.event(properties, "clicked", "", element.Span)
		if err != nil {
			return pipeview.Node{}, err
		}
		return pipeview.Button(identity, label, pipeview.Click(event.Name)), nil
	}
	value, hasBinding := properties["value"]
	if element.Component == "Label" && !hasBinding {
		return pipeview.StaticText(identity, label), nil
	}
	if !hasBinding || value.Kind != "binding" {
		span := element.Span
		if hasBinding {
			span = value.Span
		}
		return pipeview.Node{}, check.failure(span, "value requires bind followed by a declared state name")
	}
	state, found := check.states[value.Text]
	if !found {
		return pipeview.Node{}, check.failure(value.Span, "unknown state "+value.Text)
	}
	if check.readOnly[state.Name] && element.Component != "Label" {
		return pipeview.Node{}, check.failure(value.Span, "computed status binding is read-only and requires Label")
	}
	check.usedStates[state.Name] = true
	if element.Component == "Label" {
		switch state.Type {
		case "bool":
			return pipeview.Label(identity, label, pipeview.State(state.Name, state.Value.Boolean)), nil
		case "int":
			return pipeview.Label(identity, label, pipeview.State(state.Name, state.Value.Integer)), nil
		case "string":
			return pipeview.Label(identity, label, pipeview.State(state.Name, state.Value.Text)), nil
		}
	}
	expected := "bool"
	if element.Component == "TextField" {
		expected = "string"
	} else if element.Component == "PercentStepper" {
		expected = "int"
	}
	if state.Type != expected {
		return pipeview.Node{}, check.failure(value.Span, fmt.Sprintf("%s value requires %s state, got %s", element.Component, expected, state.Type))
	}
	event, err := check.event(properties, "changed", expected, element.Span)
	if err != nil {
		return pipeview.Node{}, err
	}
	if element.Component == "Toggle" {
		return pipeview.Toggle(identity, label, pipeview.State(state.Name, state.Value.Boolean), pipeview.Change[bool](event.Name)), nil
	}
	if element.Component == "PercentStepper" {
		return pipeview.PercentStepper(identity, label, pipeview.State(state.Name, state.Value.Integer), pipeview.Change[int64](event.Name)), nil
	}
	return pipeview.TextField(identity, pipeview.State(state.Name, state.Value.Text), pipeview.Change[string](event.Name)), nil
}

func (check *checker) event(properties map[string]pipelang.ViewValue, name, payload string, span pipelang.Span) (pipelang.ViewEvent, error) {
	value, found := properties[name]
	if !found || value.Kind != "reference" {
		if found {
			span = value.Span
		}
		return pipelang.ViewEvent{}, check.failure(span, name+" requires a declared "+check.interactionName()+" reference")
	}
	event, found := check.events[value.Text]
	if !found {
		return pipelang.ViewEvent{}, check.failure(value.Span, "unknown "+check.interactionName()+" "+value.Text)
	}
	if event.Payload != payload {
		return pipelang.ViewEvent{}, check.failure(value.Span, check.interactionName()+" payload does not match component interaction")
	}
	check.usedEvents[event.Name] = true
	return event, nil
}

func (check *checker) interactionName() string {
	if check.actionProfile {
		return "action"
	}
	return "event"
}
