package pipelang

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"
)

// ViewSourceParser consumes admitted lexical tokens. An error is a component
// failure; a syntax error is an exclusive Failure outcome. It never owns SDK
// component or state semantics, source admission, or automatic fallback.
type ViewSourceParser func(file *SourceFile, tokens []LexicalToken) (ViewSyntaxResult, error)

type ViewSyntaxResult struct {
	Declaration *ViewDeclaration   `json:"declaration"`
	Failure     *ViewSyntaxFailure `json:"failure"`
}

type ViewSyntaxFailure struct {
	Code    DiagnosticCode `json:"code"`
	Message string         `json:"message"`
	Span    Span           `json:"span"`
}

// ValidateViewSyntaxResult admits a foreign syntax tree without running the Go
// parser. Extents are token-aligned, values retain their admitted token values,
// and child ownership, closed unions and grammar resource limits are checked.
func ValidateViewSyntaxResult(file *SourceFile, tokens []LexicalToken, result ViewSyntaxResult) error {
	if file == nil || file.ID == "" || len(file.Text) == 0 || len(file.Text) > viewSourceLimit || !utf8.ValidString(file.Text) {
		return fmt.Errorf("view parser requires an admitted source within 64 KiB")
	}
	if err := ValidateLexicalResult(file, LexicalResult{Tokens: tokens}); err != nil {
		return err
	}
	if (result.Declaration == nil) == (result.Failure == nil) {
		return fmt.Errorf("view parser must select exactly one syntax outcome")
	}
	admission := viewSyntaxAdmission{file: file, tokens: tokens, starts: make(map[int]int), ends: make(map[int]int)}
	for index, token := range tokens {
		admission.starts[token.Start] = index
		admission.ends[token.End] = index
	}
	if failure := result.Failure; failure != nil {
		if failure.Code != CodeExpectedToken && failure.Code != CodeUnexpectedToken {
			return fmt.Errorf("unsupported view syntax diagnostic code %q", failure.Code)
		}
		// A quoted identifier can occupy most of the admitted source.
		if failure.Message == "" || len(failure.Message) > viewSourceLimit+128 || !utf8.ValidString(failure.Message) {
			return fmt.Errorf("invalid view syntax diagnostic message")
		}
		_, _, err := admission.extent(failure.Span, true)
		return err
	}
	return admission.declaration(result.Declaration)
}

type viewSyntaxAdmission struct {
	file     *SourceFile
	tokens   []LexicalToken
	starts   map[int]int
	ends     map[int]int
	elements int
}

func (a *viewSyntaxAdmission) extent(span Span, allowEOF bool) (int, int, error) {
	start, hasStart := a.starts[span.Start]
	end, hasEnd := a.ends[span.End]
	// EOF shares its end with the final non-whitespace token.
	if hasEnd && end == len(a.tokens)-1 && span.Start != span.End {
		end--
		if end < 0 || a.tokens[end].End != span.End {
			hasEnd = false
		}
	}
	if span.File != a.file.ID || !hasStart || !hasEnd || start > end ||
		(!allowEOF && (span.Start == span.End || end == len(a.tokens)-1)) {
		return 0, 0, fmt.Errorf("view syntax has an invalid token extent")
	}
	return start, end, nil
}

func (a *viewSyntaxAdmission) token(index int, kind, literal string) bool {
	return index >= 0 && index < len(a.tokens) && a.tokens[index].Kind == kind && a.tokens[index].Literal == literal
}

func (a *viewSyntaxAdmission) identifier(index int, value string) bool {
	return value != "" && a.token(index, "identifier", value)
}

func (a *viewSyntaxAdmission) value(value ViewValue, allowReference bool) error {
	start, end, err := a.extent(value.Span, false)
	if err != nil {
		return err
	}
	valid := false
	switch value.Kind {
	case "text":
		valid = start == end && !value.Boolean && value.Integer == 0 && a.token(start, "text", value.Text)
	case "boolean":
		valid = start == end && value.Text == "" && value.Integer == 0 && a.token(start, "boolean", strconv.FormatBool(value.Boolean))
	case "integer":
		literal := a.tokens[start].Literal
		shape := start == end && a.tokens[start].Kind == "integer"
		if end == start+1 && a.token(start, "minus", "-") && a.tokens[end].Kind == "integer" {
			literal += a.tokens[end].Literal
			shape = true
		}
		integer, parseError := strconv.ParseInt(literal, 10, 64)
		valid = shape && parseError == nil && value.Text == "" && !value.Boolean && value.Integer == integer
	case "reference":
		valid = allowReference && start == end && !value.Boolean && value.Integer == 0 && value.Text != "bind" && a.identifier(start, value.Text)
	case "binding":
		name, member, hasMember := strings.Cut(value.Text, ".")
		valid = allowReference && !value.Boolean && value.Integer == 0 && a.token(start, "identifier", "bind") && a.identifier(start+1, name)
		if hasMember {
			valid = valid && !strings.Contains(member, ".") && end == start+3 && a.token(start+2, "dot", ".") && a.identifier(start+3, member)
		} else {
			valid = valid && end == start+1
		}
	}
	if !valid {
		return fmt.Errorf("view syntax value has an invalid or incoherent union")
	}
	return nil
}

func (a *viewSyntaxAdmission) declaration(view *ViewDeclaration) error {
	start, end, err := a.extent(view.Span, false)
	if err != nil {
		return err
	}
	if start != 0 || end != len(a.tokens)-2 || !a.token(start, "identifier", "view") || !a.identifier(start+1, view.Name) ||
		!a.token(start+2, "lbrace", "{") || !a.token(end, "rbrace", "}") ||
		len(view.State) > viewDeclarationLimit || len(view.Events) > viewDeclarationLimit || len(view.Actions) > viewDeclarationLimit {
		return fmt.Errorf("view syntax declaration has invalid identity, extent or count")
	}
	children := make([]Span, 0, len(view.State)+len(view.Events)+len(view.Actions)+1)
	for _, state := range view.State {
		first, last, err := a.extent(state.Span, false)
		if err != nil {
			return err
		}
		valueFirst, valueLast, err := a.extent(state.Value.Span, false)
		if err != nil {
			return err
		}
		if !a.token(first, "identifier", "state") || !a.identifier(first+1, state.Name) ||
			!a.token(first+2, "colon", ":") || !a.identifier(first+3, state.Type) ||
			!a.token(first+4, "assign", "=") || valueFirst != first+5 || valueLast != last-1 || !a.token(last, "semicolon", ";") {
			return fmt.Errorf("view state syntax is incoherent")
		}
		if err := a.value(state.Value, false); err != nil {
			return err
		}
		children = append(children, state.Span)
	}
	for _, event := range view.Events {
		if err := a.interaction(event.Span, "event", event.Name, event.Payload, "", ""); err != nil {
			return err
		}
		children = append(children, event.Span)
	}
	for _, action := range view.Actions {
		if err := a.interaction(action.Span, "action", action.Name, action.Payload, action.Operation, action.Target); err != nil {
			return err
		}
		children = append(children, action.Span)
	}
	if err := a.element(view.Root, 1); err != nil {
		return err
	}
	children = append(children, view.Root.Span)
	sort.Slice(children, func(left, right int) bool { return children[left].Start < children[right].Start })
	cursor := start + 3
	for _, child := range children {
		first, last, _ := a.extent(child, false)
		if first != cursor {
			return fmt.Errorf("view declarations have missing or overlapping extents")
		}
		cursor = last + 1
	}
	if cursor != end || children[len(children)-1] != view.Root.Span || !orderedDeclarations(view) {
		return fmt.Errorf("view declarations have invalid ordering")
	}
	return nil
}

func orderedDeclarations(view *ViewDeclaration) bool {
	previous := -1
	for _, state := range view.State {
		if state.Span.Start <= previous {
			return false
		}
		previous = state.Span.Start
	}
	previous = -1
	for _, event := range view.Events {
		if event.Span.Start <= previous {
			return false
		}
		previous = event.Span.Start
	}
	previous = -1
	for _, action := range view.Actions {
		if action.Span.Start <= previous {
			return false
		}
		previous = action.Span.Start
	}
	return true
}

func (a *viewSyntaxAdmission) interaction(span Span, kind, name, payload, operation, target string) error {
	first, last, err := a.extent(span, false)
	if err != nil {
		return err
	}
	cursor := first + 3
	valid := a.token(first, "identifier", kind) && a.identifier(first+1, name) && a.token(first+2, "lparen", "(")
	if payload != "" {
		valid = valid && a.identifier(cursor, payload)
		cursor++
	}
	valid = valid && a.token(cursor, "rparen", ")")
	cursor++
	if kind == "action" {
		valid = valid && a.token(cursor, "arrow", "=>") && a.identifier(cursor+1, operation)
		cursor += 2
		switch operation {
		case "set":
			valid = valid && a.identifier(cursor, target)
			cursor++
		case "save", "revert":
			valid = valid && target == ""
		default:
			valid = false
		}
	}
	if !valid || cursor != last || !a.token(last, "semicolon", ";") {
		return fmt.Errorf("view interaction syntax is incoherent")
	}
	return nil
}

func (a *viewSyntaxAdmission) element(element ViewElement, depth int) error {
	a.elements++
	if depth > viewDepthLimit || a.elements > viewElementLimit || len(element.Properties) > len(a.tokens) || len(element.Children) > viewElementLimit {
		return fmt.Errorf("view syntax element resource limit exceeded")
	}
	first, last, err := a.extent(element.Span, false)
	if err != nil {
		return err
	}
	if !a.identifier(first, element.Component) || !a.token(first+1, "lparen", "(") {
		return fmt.Errorf("view element identity is incoherent")
	}
	cursor := first + 2
	names := make(map[string]bool)
	for index, property := range element.Properties {
		start, end, err := a.extent(property.Span, false)
		if err != nil {
			return err
		}
		valueStart, valueEnd, err := a.extent(property.Value.Span, false)
		if err != nil {
			return err
		}
		if index != 0 {
			if !a.token(cursor, "comma", ",") {
				return fmt.Errorf("view property separator is incoherent")
			}
			cursor++
		}
		if start != cursor || names[property.Name] || !a.identifier(start, property.Name) || !a.token(start+1, "colon", ":") || valueStart != start+2 || valueEnd != end {
			return fmt.Errorf("view property syntax is incoherent")
		}
		names[property.Name] = true
		if err := a.value(property.Value, true); err != nil {
			return err
		}
		cursor = end + 1
	}
	if !a.token(cursor, "rparen", ")") {
		return fmt.Errorf("view element properties are incomplete")
	}
	cursor++
	if a.token(cursor, "lbrace", "{") {
		cursor++
		for _, child := range element.Children {
			start, end, err := a.extent(child.Span, false)
			if err != nil {
				return err
			}
			if start != cursor {
				return fmt.Errorf("view child extents are missing or overlapping")
			}
			if err := a.element(child, depth+1); err != nil {
				return err
			}
			cursor = end + 1
		}
		if !a.token(cursor, "rbrace", "}") {
			return fmt.Errorf("view element children are incomplete")
		}
	} else if len(element.Children) != 0 || !a.token(cursor, "semicolon", ";") {
		return fmt.Errorf("view leaf syntax is incoherent")
	}
	if cursor != last {
		return fmt.Errorf("view element extent is incoherent")
	}
	return nil
}
