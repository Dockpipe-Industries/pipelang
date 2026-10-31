package pipelang

import (
	"fmt"
	"strconv"
	"strings"
	"unicode/utf8"
)

// FormatViewSource writes canonical source for the experimental view grammar.
// It validates bounded syntax and AST field structure, ignoring source spans.
// Component, state type and interaction semantics remain the consuming SDK's
// responsibility. Text preserves exact UTF-8 bytes, including embedded NUL.
func FormatViewSource(declaration *ViewDeclaration) ([]byte, error) {
	if declaration == nil {
		return nil, fmt.Errorf("view formatter requires a declaration")
	}
	if err := viewFormatIdentifier(declaration.Name); err != nil {
		return nil, err
	}
	if len(declaration.State) > viewDeclarationLimit || len(declaration.Events) > viewDeclarationLimit || len(declaration.Actions) > viewDeclarationLimit {
		return nil, fmt.Errorf("view formatter declaration limit exceeded")
	}
	formatter := viewSourceFormatter{}
	formatter.write("view ", declaration.Name, " {\n")
	for _, state := range declaration.State {
		if err := viewFormatIdentifier(state.Name); err != nil {
			return nil, err
		}
		if err := viewFormatIdentifier(state.Type); err != nil {
			return nil, err
		}
		formatter.write("    state ", state.Name, ": ", state.Type, " = ")
		if err := formatter.value(state.Value, false); err != nil {
			return nil, err
		}
		formatter.write(";\n")
	}
	for _, event := range declaration.Events {
		if err := viewFormatInteraction(event.Name, event.Payload); err != nil {
			return nil, err
		}
		formatter.write("    event ", event.Name, "(", event.Payload, ");\n")
	}
	for _, action := range declaration.Actions {
		if err := viewFormatInteraction(action.Name, action.Payload); err != nil {
			return nil, err
		}
		formatter.write("    action ", action.Name, "(", action.Payload, ") => ")
		switch action.Operation {
		case "set":
			if err := viewFormatIdentifier(action.Target); err != nil {
				return nil, err
			}
			formatter.write("set ", action.Target)
		case "save", "revert":
			if action.Target != "" {
				return nil, fmt.Errorf("view formatter save and revert handlers cannot have a target")
			}
			formatter.write(action.Operation)
		default:
			return nil, fmt.Errorf("view formatter requires a set, save or revert handler")
		}
		formatter.write(";\n")
	}
	if err := formatter.element(declaration.Root, 1); err != nil {
		return nil, err
	}
	formatter.write("}\n")
	if formatter.err != nil {
		return nil, formatter.err
	}
	data := []byte(formatter.source.String())
	sources, diagnostics := NewSourceSet([]SourceInput{{Path: "formatted-view.pipe", Data: data}})
	if diagnostics.HasErrors() {
		return nil, fmt.Errorf("view formatter produced invalid source text")
	}
	if _, err := ParseViewSource(sources.Files()[0], nil); err != nil {
		return nil, fmt.Errorf("view formatter declaration does not match the view grammar")
	}
	return data, nil
}

type viewSourceFormatter struct {
	source   strings.Builder
	elements int
	err      error
}

func (formatter *viewSourceFormatter) write(parts ...string) {
	if formatter.err != nil {
		return
	}
	for _, part := range parts {
		if len(part) > viewSourceLimit-formatter.source.Len() {
			formatter.err = fmt.Errorf("view formatter source exceeds 65536 bytes")
			return
		}
		formatter.source.WriteString(part)
	}
}

func viewFormatIdentifier(identity string) error {
	if len(identity) == 0 || len(identity) > viewSourceLimit || !isIdentStart(identity[0]) {
		return fmt.Errorf("view formatter requires a lexer identifier")
	}
	for index := 1; index < len(identity); index++ {
		if !isIdentPart(identity[index]) {
			return fmt.Errorf("view formatter requires a lexer identifier")
		}
	}
	switch identity {
	case "Interface", "Class", "Struct", "public", "private", "true", "false":
		return fmt.Errorf("view formatter identifier cannot be a lexer keyword")
	}
	return nil
}

func viewFormatInteraction(name, payload string) error {
	if err := viewFormatIdentifier(name); err != nil {
		return err
	}
	if payload != "" {
		return viewFormatIdentifier(payload)
	}
	return nil
}

func (formatter *viewSourceFormatter) element(element ViewElement, depth int) error {
	if depth > viewDepthLimit || formatter.elements >= viewElementLimit || len(element.Children) > viewElementLimit {
		return fmt.Errorf("view formatter element depth or node limit exceeded")
	}
	formatter.elements++
	if err := viewFormatIdentifier(element.Component); err != nil {
		return err
	}
	if len(element.Properties) > viewSourceLimit {
		return fmt.Errorf("view formatter property count exceeds source capacity")
	}
	indent := strings.Repeat("    ", depth)
	formatter.write(indent, element.Component, "(")
	properties := make(map[string]bool)
	for index, property := range element.Properties {
		if err := viewFormatIdentifier(property.Name); err != nil {
			return err
		}
		if properties[property.Name] {
			return fmt.Errorf("view formatter property names must be unique")
		}
		properties[property.Name] = true
		if index != 0 {
			formatter.write(", ")
		}
		formatter.write(property.Name, ": ")
		if err := formatter.value(property.Value, true); err != nil {
			return err
		}
	}
	formatter.write(")")
	if len(element.Children) == 0 {
		formatter.write(";\n")
		return formatter.err
	}
	formatter.write(" {\n")
	for _, child := range element.Children {
		if err := formatter.element(child, depth+1); err != nil {
			return err
		}
	}
	formatter.write(indent, "}\n")
	return formatter.err
}

func (formatter *viewSourceFormatter) value(value ViewValue, allowReference bool) error {
	if formatter.err != nil {
		return formatter.err
	}
	switch value.Kind {
	case "text":
		if value.Boolean || value.Integer != 0 {
			return fmt.Errorf("view formatter text has nonzero inactive fields")
		}
		return formatter.text(value.Text)
	case "boolean":
		if value.Text != "" || value.Integer != 0 {
			return fmt.Errorf("view formatter boolean has nonzero inactive fields")
		}
		formatter.write(strconv.FormatBool(value.Boolean))
	case "integer":
		if value.Text != "" || value.Boolean {
			return fmt.Errorf("view formatter integer has nonzero inactive fields")
		}
		formatter.write(strconv.FormatInt(value.Integer, 10))
	case "reference", "binding":
		if !allowReference {
			return fmt.Errorf("view formatter state initializer must be a literal")
		}
		if value.Boolean || value.Integer != 0 {
			return fmt.Errorf("view formatter reference has nonzero inactive fields")
		}
		if value.Kind == "reference" {
			if value.Text == "bind" {
				return fmt.Errorf("view formatter bind keyword cannot be a plain reference")
			}
			if err := viewFormatIdentifier(value.Text); err != nil {
				return err
			}
			formatter.write(value.Text)
			return formatter.err
		}
		if len(value.Text) > viewSourceLimit {
			return fmt.Errorf("view formatter binding exceeds source capacity")
		}
		parts := strings.Split(value.Text, ".")
		if len(parts) > 2 {
			return fmt.Errorf("view formatter binding allows at most one qualified member")
		}
		for _, part := range parts {
			if err := viewFormatIdentifier(part); err != nil {
				return err
			}
		}
		formatter.write("bind ", value.Text)
	default:
		return fmt.Errorf("view formatter requires a known value kind")
	}
	return formatter.err
}

func (formatter *viewSourceFormatter) text(value string) error {
	if len(value) > viewSourceLimit || !utf8.ValidString(value) {
		return fmt.Errorf("view formatter text must be bounded valid UTF-8")
	}
	formatter.write("\"")
	for index := 0; index < len(value); index++ {
		switch value[index] {
		case '\a':
			formatter.write(`\a`)
		case '\b':
			formatter.write(`\b`)
		case '\f':
			formatter.write(`\f`)
		case '\n':
			formatter.write(`\n`)
		case '\r':
			formatter.write(`\r`)
		case '\t':
			formatter.write(`\t`)
		case '\v':
			formatter.write(`\v`)
		case '\\':
			formatter.write(`\\`)
		case '"':
			formatter.write(`\"`)
		default:
			formatter.write(value[index : index+1])
		}
		if formatter.err != nil {
			return formatter.err
		}
	}
	formatter.write("\"")
	return formatter.err
}
