// Package forgepipe projects PipeView's bounded declaration contract into the
// existing ForgePipe UI transport. It imports no engine or compiler code.
package forgepipe

import (
	"encoding/json"
	"fmt"
	"math"
	"regexp"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
)

const Schema = "forgepipe.ui.v1"
const PercentSchema = "forgepipe.ui.v2"

// SchemaFor identifies the exact native capability required by a checked view.
func SchemaFor(document pipeview.Document) (string, error) {
	snapshot, err := document.Inspect()
	if err != nil {
		return "", err
	}
	return schemaFor(snapshot), nil
}

func schemaFor(snapshot pipeview.Snapshot) string {
	if snapshot.Contract == pipeview.PercentContract {
		return PercentSchema
	}
	return Schema
}

// Options select the existing adapter's legacy style profile. This is not the
// future PipeView styles language.
type Options struct {
	Scale float64
	Focus string
}

type node struct {
	ID       string `json:"id"`
	Kind     string `json:"kind"`
	Text     string `json:"text,omitempty"`
	Binding  string `json:"binding,omitempty"`
	Action   string `json:"action,omitempty"`
	Children []node `json:"children,omitempty"`
}

var colorPattern = regexp.MustCompile(`^#[0-9A-Fa-f]{8}$`)

// Encode emits generated transport only after neutral and adapter admission.
func Encode(document pipeview.Document, options Options) ([]byte, error) {
	snapshot, err := document.Inspect()
	if err != nil {
		return nil, err
	}
	if math.IsNaN(options.Scale) || math.IsInf(options.Scale, 0) || options.Scale < 0.5 || options.Scale > 3 {
		return nil, fmt.Errorf("ForgePipe UI scale must be finite and within 0.5..3")
	}
	if !colorPattern.MatchString(options.Focus) {
		return nil, fmt.Errorf("ForgePipe UI focus color must use #RRGGBBAA")
	}
	state := make(map[string]any, len(snapshot.State))
	for identity, value := range snapshot.State {
		if len(identity) > 64 {
			return nil, fmt.Errorf("ForgePipe UI state identity exceeds 64 bytes: %s", identity)
		}
		switch value.Kind {
		case pipeview.Boolean:
			state[identity] = value.Boolean
		case pipeview.Integer:
			state[identity] = value.Integer
		case pipeview.Text:
			state[identity] = value.Text
		default:
			return nil, fmt.Errorf("ForgePipe UI cannot represent state type %q", value.Kind)
		}
	}
	root, err := project(snapshot.Root)
	if err != nil {
		return nil, err
	}
	schema := schemaFor(snapshot)
	result := struct {
		Schema string         `json:"schema"`
		Root   node           `json:"root"`
		Style  style          `json:"style"`
		State  map[string]any `json:"state"`
	}{Schema: schema, Root: root, Style: style{Scale: options.Scale, Focus: options.Focus}, State: state}
	encoded, err := json.Marshal(result)
	if err != nil {
		return nil, err
	}
	if len(encoded) > 65536 {
		return nil, fmt.Errorf("ForgePipe UI transport exceeds 65536 bytes")
	}
	return encoded, nil
}

type style struct {
	Scale float64 `json:"scale"`
	Focus string  `json:"focus"`
}

func project(element pipeview.Element) (node, error) {
	if len(element.ID) > 64 || len(element.Binding) > 64 || len(element.Event) > 64 {
		return node{}, fmt.Errorf("ForgePipe UI identities must fit 64 bytes")
	}
	switch element.Kind {
	case pipeview.ColumnKind, pipeview.RowKind, pipeview.LabelKind,
		pipeview.ButtonKind, pipeview.ToggleKind, pipeview.TextFieldKind, pipeview.PercentStepperKind:
	default:
		return node{}, fmt.Errorf("ForgePipe UI cannot represent component %q", element.Kind)
	}
	result := node{ID: element.ID, Kind: string(element.Kind), Text: element.Text,
		Binding: element.Binding, Action: element.Event}
	for _, child := range element.Children {
		projected, err := project(child)
		if err != nil {
			return node{}, err
		}
		result.Children = append(result.Children, projected)
	}
	return result, nil
}
