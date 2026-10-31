package markup

import (
	"encoding/xml"
	"fmt"
	"io"
	"strconv"
	"strings"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

type parser struct {
	file    *pipelang.SourceFile
	decoder *xml.Decoder
	offset  int64
	nodes   int
}

func parse(file *pipelang.SourceFile) (result *pipelang.ViewDeclaration, failure error) {
	reader := &parser{file: file, decoder: xml.NewDecoder(strings.NewReader(file.Text))}
	defer func() {
		if failure == io.EOF {
			failure = reader.failure("unexpected end of XML")
		}
	}()
	token, err := reader.significant()
	if err != nil {
		return nil, err
	}
	root, ok := token.(xml.StartElement)
	if !ok || root.Name.Local != "View" {
		return nil, reader.failure("expected one View root")
	}
	attributes, err := reader.attributes(root, "name", "profile")
	if err != nil {
		return nil, err
	}
	name, err := reader.required(attributes, "name")
	if err != nil {
		return nil, err
	}
	profile, err := reader.required(attributes, "profile")
	if err != nil {
		return nil, err
	}
	if profile != source.Contract && profile != source.ActionContract {
		return nil, reader.failure("unsupported view profile " + profile)
	}
	declaration := &pipelang.ViewDeclaration{Name: name, Span: reader.startTagSpan()}
	hasRoot := false
	for {
		token, err := reader.significant()
		if err != nil {
			return nil, err
		}
		switch typed := token.(type) {
		case xml.StartElement:
			switch typed.Name.Local {
			case "State", "Event", "Action":
				if hasRoot {
					return nil, reader.failure("declarations must precede the component root")
				}
				if err := reader.declaration(typed, declaration); err != nil {
					return nil, err
				}
			default:
				if hasRoot {
					return nil, reader.failure("View requires exactly one component root")
				}
				declaration.Root, err = reader.component(typed, 1)
				if err != nil {
					return nil, err
				}
				hasRoot = true
			}
		case xml.EndElement:
			if typed.Name.Local != "View" || !hasRoot {
				return nil, reader.failure("View requires exactly one component root")
			}
			if profile == source.Contract && len(declaration.Actions) != 0 ||
				profile == source.ActionContract && len(declaration.Actions) == 0 {
				return nil, reader.failure("XML profile must agree with its event or action declarations")
			}
			if _, err := reader.significant(); err != io.EOF {
				if err != nil {
					return nil, err
				}
				return nil, reader.failure("content follows the View root")
			}
			return declaration, nil
		default:
			return nil, reader.failure("expected a declaration or component")
		}
	}
}

func (reader *parser) next() (xml.Token, error) {
	for {
		reader.offset = reader.decoder.InputOffset()
		token, err := reader.decoder.Token()
		if err != nil {
			if err == io.EOF {
				return nil, err
			}
			return nil, reader.failure(err.Error())
		}
		switch typed := token.(type) {
		case xml.Comment:
			continue
		case xml.Directive:
			return nil, reader.failure("XML directives and DTDs are not supported")
		case xml.ProcInst:
			return nil, reader.failure("XML declarations and processing instructions are not supported")
		case xml.StartElement:
			// encoding/xml can replace invalid numeric scalar references with
			// U+FFFD. Check the authored token before trusting decoded values.
			raw := reader.file.Text[reader.offset:reader.decoder.InputOffset()]
			if err := reader.numericReferences(raw); err != nil {
				return nil, err
			}
			if typed.Name.Space != "" {
				return nil, reader.failure("XML namespaces are not supported")
			}
		case xml.EndElement:
			if typed.Name.Space != "" {
				return nil, reader.failure("XML namespaces are not supported")
			}
		}
		return token, nil
	}
}

func (reader *parser) significant() (xml.Token, error) {
	for {
		token, err := reader.next()
		if err != nil {
			return nil, err
		}
		if text, ok := token.(xml.CharData); ok {
			if strings.Trim(string(text), " \t\r\n") != "" {
				return nil, reader.failure("mixed text content is not supported")
			}
			continue
		}
		return token, nil
	}
}

func (reader *parser) attributes(element xml.StartElement, allowed ...string) (map[string]string, error) {
	result := make(map[string]string, len(element.Attr))
	for _, attribute := range element.Attr {
		if attribute.Name.Space != "" || attribute.Name.Local == "xmlns" {
			return nil, reader.failure("XML namespaces are not supported")
		}
		name := attribute.Name.Local
		if _, duplicate := result[name]; duplicate {
			return nil, reader.failure("duplicate attribute " + name)
		}
		known := false
		for _, candidate := range allowed {
			if name == candidate {
				known = true
				break
			}
		}
		if !known {
			return nil, reader.failure("unsupported attribute " + name + " on " + element.Name.Local)
		}
		result[name] = attribute.Value
	}
	return result, nil
}

func (reader *parser) required(attributes map[string]string, name string) (string, error) {
	value, exists := attributes[name]
	if !exists || value == "" {
		return "", reader.failure("missing attribute " + name)
	}
	return value, nil
}

func (reader *parser) declaration(element xml.StartElement, declaration *pipelang.ViewDeclaration) error {
	span := reader.startTagSpan()
	switch element.Name.Local {
	case "State":
		if len(declaration.State) >= 128 {
			return reader.failure("state declaration capacity exceeded")
		}
		attributes, err := reader.attributes(element, "name", "type", "value")
		if err != nil {
			return err
		}
		name, err := reader.required(attributes, "name")
		if err != nil {
			return err
		}
		typeName, err := reader.required(attributes, "type")
		if err != nil {
			return err
		}
		literal, exists := attributes["value"]
		if !exists {
			return reader.failure("missing attribute value")
		}
		value := pipelang.ViewValue{Span: span}
		switch typeName {
		case "string":
			value.Kind = "text"
			value.Text = literal
		case "bool":
			if literal != "true" && literal != "false" {
				return reader.failure("bool state value requires true or false")
			}
			value.Kind = "boolean"
			value.Boolean = literal == "true"
		case "int":
			integer, err := strconv.ParseInt(literal, 10, 64)
			if err != nil {
				return reader.failure("int state value requires a signed 64-bit decimal integer")
			}
			value.Kind = "integer"
			value.Integer = integer
		default:
			return reader.failure("unsupported state type " + typeName)
		}
		declaration.State = append(declaration.State, pipelang.ViewState{Name: name, Type: typeName, Value: value, Span: span})
	case "Event", "Action":
		allowed := []string{"name", "payload"}
		if element.Name.Local == "Action" {
			allowed = append(allowed, "operation", "target")
		}
		attributes, err := reader.attributes(element, allowed...)
		if err != nil {
			return err
		}
		name, err := reader.required(attributes, "name")
		if err != nil {
			return err
		}
		payload := attributes["payload"]
		if element.Name.Local == "Event" {
			if len(declaration.Events) >= 128 {
				return reader.failure("event declaration capacity exceeded")
			}
			declaration.Events = append(declaration.Events, pipelang.ViewEvent{Name: name, Payload: payload, Span: span})
		} else {
			if len(declaration.Actions) >= 128 {
				return reader.failure("action declaration capacity exceeded")
			}
			operation, err := reader.required(attributes, "operation")
			if err != nil {
				return err
			}
			declaration.Actions = append(declaration.Actions, pipelang.ViewAction{
				Name: name, Payload: payload, Operation: operation, Target: attributes["target"], Span: span,
			})
		}
	}
	token, err := reader.significant()
	if err != nil {
		return err
	}
	if end, ok := token.(xml.EndElement); !ok || end.Name.Local != element.Name.Local {
		return reader.failure("declarations cannot contain child elements")
	}
	return nil
}

func (reader *parser) component(element xml.StartElement, depth int) (pipelang.ViewElement, error) {
	span := reader.startTagSpan()
	reader.nodes++
	if reader.nodes > 128 || depth > 8 {
		return pipelang.ViewElement{}, reader.failure("component node or depth capacity exceeded")
	}
	allowed, supported := componentProperties(element.Name.Local)
	if !supported {
		return pipelang.ViewElement{}, reader.failure("unsupported component " + element.Name.Local)
	}
	attributes, err := reader.attributes(element, allowed...)
	if err != nil {
		return pipelang.ViewElement{}, err
	}
	result := pipelang.ViewElement{Component: element.Name.Local, Span: span}
	// Declaration order is canonical and independent of XML attribute order.
	for _, name := range allowed {
		literal, exists := attributes[name]
		if !exists {
			continue
		}
		value := pipelang.ViewValue{Kind: "text", Text: literal, Span: span}
		if name == "value" {
			if !strings.HasPrefix(literal, "{bind ") || !strings.HasSuffix(literal, "}") {
				return pipelang.ViewElement{}, reader.failure("value requires {bind name}")
			}
			value.Kind = "binding"
			value.Text = literal[len("{bind ") : len(literal)-1]
		} else if name == "changed" || name == "clicked" {
			if !strings.HasPrefix(literal, "{") || !strings.HasSuffix(literal, "}") || len(literal) < 3 {
				return pipelang.ViewElement{}, reader.failure(name + " requires {interactionName}")
			}
			value.Kind = "reference"
			value.Text = literal[1 : len(literal)-1]
		}
		result.Properties = append(result.Properties, pipelang.ViewProperty{Name: name, Value: value, Span: span})
	}
	for {
		token, err := reader.significant()
		if err != nil {
			return pipelang.ViewElement{}, err
		}
		switch typed := token.(type) {
		case xml.StartElement:
			if element.Name.Local != "Column" && element.Name.Local != "Row" {
				return pipelang.ViewElement{}, reader.failure("leaf component cannot contain children")
			}
			child, err := reader.component(typed, depth+1)
			if err != nil {
				return pipelang.ViewElement{}, err
			}
			result.Children = append(result.Children, child)
		case xml.EndElement:
			return result, nil
		default:
			return pipelang.ViewElement{}, reader.failure(fmt.Sprintf("unsupported XML component token %T", token))
		}
	}
}

func (reader *parser) failure(message string) error {
	return xmlFailure(reader.file, reader.offset, message)
}

// XML diagnostics deliberately identify the containing authored start tag.
// No attribute-level precision is inferred from normalized decoder values.
func (reader *parser) startTagSpan() pipelang.Span {
	return pipelang.Span{
		File:  reader.file.ID,
		Start: int(reader.offset),
		End:   int(reader.decoder.InputOffset()),
	}
}

func (reader *parser) numericReferences(raw string) error {
	position := 0
	for position < len(raw) {
		relative := strings.Index(raw[position:], "&#")
		if relative < 0 {
			return nil
		}
		start := position + relative
		digits := start + len("&#")
		base := 10
		if digits < len(raw) && raw[digits] == 'x' {
			base = 16
			digits++
		}
		length := strings.IndexByte(raw[digits:], ';')
		if length < 0 {
			return xmlFailure(reader.file, reader.offset+int64(start), "unterminated numeric character reference")
		}
		end := digits + length
		character, err := strconv.ParseUint(raw[digits:end], base, 64)
		if err != nil || character > 0x10ffff || !validXMLCharacter(rune(character)) {
			return xmlFailure(reader.file, reader.offset+int64(start), "numeric character reference is outside XML 1.0 characters")
		}
		position = end + 1
	}
	return nil
}

func componentProperties(component string) ([]string, bool) {
	switch component {
	case "Column", "Row":
		return []string{"id"}, true
	case "Label":
		return []string{"id", "text", "value"}, true
	case "Button":
		return []string{"id", "label", "clicked"}, true
	case "Toggle", "PercentStepper":
		return []string{"id", "label", "value", "changed"}, true
	case "TextField":
		return []string{"id", "value", "changed"}, true
	default:
		return nil, false
	}
}
