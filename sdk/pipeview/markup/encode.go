package markup

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"strconv"
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func encode(file *pipelang.SourceFile, declaration *pipelang.ViewDeclaration) ([]byte, error) {
	profile := source.Contract
	if len(declaration.Actions) != 0 {
		profile = source.ActionContract
	}
	var buffer bytes.Buffer
	encoder := xml.NewEncoder(&buffer)
	encoder.Indent("", "    ")
	start := xml.StartElement{Name: xml.Name{Local: "View"}, Attr: []xml.Attr{
		{Name: xml.Name{Local: "name"}, Value: declaration.Name},
		{Name: xml.Name{Local: "profile"}, Value: profile},
	}}
	if err := encodeStart(file, encoder, start); err != nil {
		return nil, err
	}
	for _, state := range declaration.State {
		literal := state.Value.Text
		if state.Value.Kind == "boolean" {
			literal = strconv.FormatBool(state.Value.Boolean)
		} else if state.Value.Kind == "integer" {
			literal = strconv.FormatInt(state.Value.Integer, 10)
		}
		if err := encodeLeaf(file, encoder, "State", []xml.Attr{
			{Name: xml.Name{Local: "name"}, Value: state.Name},
			{Name: xml.Name{Local: "type"}, Value: state.Type},
			{Name: xml.Name{Local: "value"}, Value: literal},
		}); err != nil {
			return nil, err
		}
	}
	for _, event := range declaration.Events {
		if err := encodeLeaf(file, encoder, "Event", []xml.Attr{
			{Name: xml.Name{Local: "name"}, Value: event.Name},
			{Name: xml.Name{Local: "payload"}, Value: event.Payload},
		}); err != nil {
			return nil, err
		}
	}
	for _, action := range declaration.Actions {
		attributes := []xml.Attr{
			{Name: xml.Name{Local: "name"}, Value: action.Name},
			{Name: xml.Name{Local: "payload"}, Value: action.Payload},
			{Name: xml.Name{Local: "operation"}, Value: action.Operation},
		}
		if action.Target != "" {
			attributes = append(attributes, xml.Attr{Name: xml.Name{Local: "target"}, Value: action.Target})
		}
		if err := encodeLeaf(file, encoder, "Action", attributes); err != nil {
			return nil, err
		}
	}
	if err := encodeComponent(file, encoder, declaration.Root); err != nil {
		return nil, err
	}
	if err := encoder.EncodeToken(start.End()); err != nil {
		return nil, err
	}
	if err := encoder.Flush(); err != nil {
		return nil, err
	}
	buffer.WriteByte('\n')
	if buffer.Len() > MaximumBytes {
		return nil, fmt.Errorf("%s: XML conversion exceeds 65536 bytes", sourcePath(file))
	}
	return buffer.Bytes(), nil
}

func encodeComponent(file *pipelang.SourceFile, encoder *xml.Encoder, element pipelang.ViewElement) error {
	start := xml.StartElement{Name: xml.Name{Local: element.Component}}
	order, supported := componentProperties(element.Component)
	if !supported {
		return fmt.Errorf("%s: unsupported checked component %q", sourcePath(file), element.Component)
	}
	properties := make(map[string]pipelang.ViewValue, len(element.Properties))
	for _, property := range element.Properties {
		properties[property.Name] = property.Value
	}
	for _, name := range order {
		value, exists := properties[name]
		if !exists {
			continue
		}
		literal := value.Text
		switch value.Kind {
		case "binding":
			literal = "{bind " + literal + "}"
		case "reference":
			literal = "{" + literal + "}"
		case "text":
		default:
			return fmt.Errorf("%s: unsupported checked component property kind %q", sourcePath(file), value.Kind)
		}
		start.Attr = append(start.Attr, xml.Attr{Name: xml.Name{Local: name}, Value: literal})
	}
	if err := encodeStart(file, encoder, start); err != nil {
		return err
	}
	for _, child := range element.Children {
		if err := encodeComponent(file, encoder, child); err != nil {
			return err
		}
	}
	return encoder.EncodeToken(start.End())
}

func encodeLeaf(file *pipelang.SourceFile, encoder *xml.Encoder, name string, attributes []xml.Attr) error {
	start := xml.StartElement{Name: xml.Name{Local: name}, Attr: attributes}
	if err := encodeStart(file, encoder, start); err != nil {
		return err
	}
	return encoder.EncodeToken(start.End())
}

func encodeStart(file *pipelang.SourceFile, encoder *xml.Encoder, element xml.StartElement) error {
	for _, attribute := range element.Attr {
		if !validXMLText(attribute.Value) {
			return fmt.Errorf("%s: XML 1.0 cannot represent attribute %s on %s without changing its value",
				sourcePath(file), attribute.Name.Local, element.Name.Local)
		}
	}
	return encoder.EncodeToken(element)
}

func validXMLText(text string) bool {
	if !utf8.ValidString(text) {
		return false
	}
	for _, character := range text {
		if validXMLCharacter(character) {
			continue
		}
		return false
	}
	return true
}

func validXMLCharacter(character rune) bool {
	return character == '\t' || character == '\n' || character == '\r' ||
		character >= 0x20 && character <= 0xd7ff ||
		character >= 0xe000 && character <= 0xfffd ||
		character >= 0x10000 && character <= 0x10ffff
}
