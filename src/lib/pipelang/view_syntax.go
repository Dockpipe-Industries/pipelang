package pipelang

import (
	"fmt"
	"strconv"
	"unicode/utf8"
)

// ViewDeclaration is the experimental single-file view syntax tree. Component,
// property, state type, event and action identities are interpreted by the consuming SDK.
// Ordinary PipeLang compiler entrypoints do not admit this grammar.
type ViewDeclaration struct {
	Name    string
	Span    Span
	State   []ViewState
	Events  []ViewEvent
	Actions []ViewAction
	Root    ViewElement
}

type ViewState struct {
	Name  string
	Type  string
	Value ViewValue
	Span  Span
}

type ViewEvent struct {
	Name    string
	Payload string
	Span    Span
}

// ViewAction is a closed declarative state handler, not a general method body.
// A set handler receives its single implicit payload; save and revert take none.
type ViewAction struct {
	Name      string
	Payload   string
	Operation string
	Target    string
	Span      Span
}

type ViewElement struct {
	Component  string
	Properties []ViewProperty
	Children   []ViewElement
	Span       Span
}

type ViewProperty struct {
	Name  string
	Value ViewValue
	Span  Span
}

// ViewValue kinds are text, boolean, integer, reference and binding. Only the
// field corresponding to Kind carries a value. Text stores decoded text or an
// unresolved symbolic reference; no expression evaluation occurs here.
type ViewValue struct {
	Kind    string
	Text    string
	Boolean bool
	Integer int64
	Span    Span
}

const (
	viewSourceLimit      = 65536
	viewElementLimit     = 128
	viewDepthLimit       = 8
	viewDeclarationLimit = 128
)

// ParseViewSource explicitly selects the experimental view grammar. A nil lexer
// selects the Go reference lexer. Foreign lexer/component failures are returned
// without fallback; source lexical failures retain their original diagnostics.
func ParseViewSource(file *SourceFile, sourceLexer SourceLexer) (*ViewDeclaration, error) {
	return ParseViewSourceWithParser(file, sourceLexer, nil)
}

// ParseViewSourceWithParser explicitly selects a bounded syntax component after
// source and lexical admission. Nil retains the reference Go parser.
func ParseViewSourceWithParser(file *SourceFile, sourceLexer SourceLexer, sourceParser ViewSourceParser) (*ViewDeclaration, error) {
	if file == nil || file.ID == "" {
		return nil, fmt.Errorf("view parser requires an admitted source file")
	}
	sources := &SourceSet{files: []*SourceFile{file}, byID: map[FileID]*SourceFile{file.ID: file}}
	span := Span{File: file.ID, End: len(file.Text)}
	if len(file.Text) == 0 || len(file.Text) > viewSourceLimit {
		return nil, oneDiagnostic(sources, CodeInvalidProgram, CategorySource, span, "view source must contain between 1 and 65536 bytes")
	}
	if !utf8.ValidString(file.Text) {
		start := firstInvalidUTF8([]byte(file.Text))
		span.Start = start
		span.End = start + 1
		return nil, oneDiagnostic(sources, CodeInvalidUTF8, CategorySource, span, "view source is not valid UTF-8")
	}
	if sourceLexer == nil {
		sourceLexer = TokenizeSource
	}
	result, err := sourceLexer(file)
	if err != nil {
		return nil, fmt.Errorf("view lexer: %w", err)
	}
	tokens, err := tokensFromLexicalResult(sources, file, result)
	if err != nil {
		return nil, err
	}
	if sourceParser != nil {
		syntax, err := sourceParser(file, append([]LexicalToken(nil), result.Tokens...))
		if err != nil {
			return nil, fmt.Errorf("view parser: %w", err)
		}
		if err := ValidateViewSyntaxResult(file, result.Tokens, syntax); err != nil {
			return nil, fmt.Errorf("view parser: %w", err)
		}
		if failure := syntax.Failure; failure != nil {
			return nil, oneDiagnostic(sources, failure.Code, CategorySyntax, failure.Span, failure.Message)
		}
		return syntax.Declaration, nil
	}
	parser := viewSyntaxParser{sources: sources, tokens: tokens}
	return parser.declaration()
}

type viewSyntaxParser struct {
	sources  *SourceSet
	tokens   []token
	index    int
	elements int
}

func (p *viewSyntaxParser) peek() token {
	return p.tokens[p.index]
}

func (p *viewSyntaxParser) take(kind tokenKind, description string) (token, error) {
	current := p.peek()
	if current.kind != kind {
		return token{}, p.failure(CodeExpectedToken, current.span, "expected "+description)
	}
	if kind != tokEOF {
		p.index++
	}
	return current, nil
}

func (p *viewSyntaxParser) failure(code DiagnosticCode, span Span, message string) error {
	return oneDiagnostic(p.sources, code, CategorySyntax, span, message)
}

func (p *viewSyntaxParser) declaration() (*ViewDeclaration, error) {
	start, err := p.take(tokIdent, "view")
	if err != nil {
		return nil, err
	}
	if start.lit != "view" {
		return nil, p.failure(CodeUnexpectedToken, start.span, "expected view declaration")
	}
	name, err := p.take(tokIdent, "view name")
	if err != nil {
		return nil, err
	}
	if _, err := p.take(tokLBrace, "'{' after view name"); err != nil {
		return nil, err
	}
	view := &ViewDeclaration{Name: name.lit}
	for p.peek().kind == tokIdent {
		switch p.peek().lit {
		case "state":
			if len(view.State) >= viewDeclarationLimit {
				return nil, p.failure(CodeUnexpectedToken, p.peek().span, "view state declaration limit exceeded")
			}
			state, err := p.state()
			if err != nil {
				return nil, err
			}
			view.State = append(view.State, state)
		case "event":
			if len(view.Events) >= viewDeclarationLimit {
				return nil, p.failure(CodeUnexpectedToken, p.peek().span, "view event declaration limit exceeded")
			}
			event, err := p.event()
			if err != nil {
				return nil, err
			}
			view.Events = append(view.Events, event)
		case "action":
			if len(view.Actions) >= viewDeclarationLimit {
				return nil, p.failure(CodeUnexpectedToken, p.peek().span, "view action declaration limit exceeded")
			}
			action, err := p.action()
			if err != nil {
				return nil, err
			}
			view.Actions = append(view.Actions, action)
		default:
			goto root
		}
	}
root:
	view.Root, err = p.element(1)
	if err != nil {
		return nil, err
	}
	end, err := p.take(tokRBrace, "'}' after the single root element")
	if err != nil {
		return nil, err
	}
	if _, err := p.take(tokEOF, "end of file"); err != nil {
		return nil, err
	}
	view.Span = mergeSpans(start.span, end.span)
	return view, nil
}

func (p *viewSyntaxParser) state() (ViewState, error) {
	start := p.peek()
	p.index++
	name, err := p.take(tokIdent, "state name")
	if err != nil {
		return ViewState{}, err
	}
	if _, err := p.take(tokColon, "':' after state name"); err != nil {
		return ViewState{}, err
	}
	typeName, err := p.take(tokIdent, "state type")
	if err != nil {
		return ViewState{}, err
	}
	if _, err := p.take(tokAssign, "'=' before state initializer"); err != nil {
		return ViewState{}, err
	}
	value, err := p.value(false)
	if err != nil {
		return ViewState{}, err
	}
	end, err := p.take(tokSemi, "';' after state declaration")
	if err != nil {
		return ViewState{}, err
	}
	return ViewState{Name: name.lit, Type: typeName.lit, Value: value, Span: mergeSpans(start.span, end.span)}, nil
}

func (p *viewSyntaxParser) event() (ViewEvent, error) {
	start := p.peek()
	p.index++
	name, err := p.take(tokIdent, "event name")
	if err != nil {
		return ViewEvent{}, err
	}
	if _, err := p.take(tokLParen, "'(' after event name"); err != nil {
		return ViewEvent{}, err
	}
	payload := ""
	if p.peek().kind == tokIdent {
		payload = p.peek().lit
		p.index++
	}
	if _, err := p.take(tokRParen, "')' after optional event payload type"); err != nil {
		return ViewEvent{}, err
	}
	end, err := p.take(tokSemi, "';' after event declaration")
	if err != nil {
		return ViewEvent{}, err
	}
	return ViewEvent{Name: name.lit, Payload: payload, Span: mergeSpans(start.span, end.span)}, nil
}

func (p *viewSyntaxParser) action() (ViewAction, error) {
	start := p.peek()
	p.index++
	name, err := p.take(tokIdent, "action name")
	if err != nil {
		return ViewAction{}, err
	}
	if _, err := p.take(tokLParen, "'(' after action name"); err != nil {
		return ViewAction{}, err
	}
	payload := ""
	if p.peek().kind == tokIdent {
		payload = p.peek().lit
		p.index++
	}
	if _, err := p.take(tokRParen, "')' after optional action payload type"); err != nil {
		return ViewAction{}, err
	}
	if _, err := p.take(tokArrow, "'=>' before action handler"); err != nil {
		return ViewAction{}, err
	}
	operation, err := p.take(tokIdent, "set, save or revert handler")
	if err != nil {
		return ViewAction{}, err
	}
	target := ""
	switch operation.lit {
	case "set":
		state, err := p.take(tokIdent, "state target after set")
		if err != nil {
			return ViewAction{}, err
		}
		target = state.lit
	case "save", "revert":
	default:
		return ViewAction{}, p.failure(CodeUnexpectedToken, operation.span, "unsupported view action handler "+quoted(operation.lit))
	}
	end, err := p.take(tokSemi, "';' after action declaration")
	if err != nil {
		return ViewAction{}, err
	}
	return ViewAction{Name: name.lit, Payload: payload, Operation: operation.lit, Target: target, Span: mergeSpans(start.span, end.span)}, nil
}

func (p *viewSyntaxParser) element(depth int) (ViewElement, error) {
	if depth > viewDepthLimit || p.elements >= viewElementLimit {
		return ViewElement{}, p.failure(CodeUnexpectedToken, p.peek().span, "view element depth or node limit exceeded")
	}
	p.elements++
	component, err := p.take(tokIdent, "component name")
	if err != nil {
		return ViewElement{}, err
	}
	if _, err := p.take(tokLParen, "'(' after component name"); err != nil {
		return ViewElement{}, err
	}
	element := ViewElement{Component: component.lit}
	propertyNames := make(map[string]struct{})
	for p.peek().kind != tokRParen {
		name, err := p.take(tokIdent, "property name")
		if err != nil {
			return ViewElement{}, err
		}
		if _, exists := propertyNames[name.lit]; exists {
			return ViewElement{}, p.failure(CodeUnexpectedToken, name.span, "duplicate view property "+quoted(name.lit))
		}
		propertyNames[name.lit] = struct{}{}
		if _, err := p.take(tokColon, "':' after property name"); err != nil {
			return ViewElement{}, err
		}
		value, err := p.value(true)
		if err != nil {
			return ViewElement{}, err
		}
		element.Properties = append(element.Properties, ViewProperty{Name: name.lit, Value: value, Span: mergeSpans(name.span, value.Span)})
		if p.peek().kind != tokComma {
			break
		}
		p.index++
		if p.peek().kind == tokRParen {
			return ViewElement{}, p.failure(CodeExpectedToken, p.peek().span, "expected property after ','")
		}
	}
	end, err := p.take(tokRParen, "')' after component properties")
	if err != nil {
		return ViewElement{}, err
	}
	if p.peek().kind == tokLBrace {
		p.index++
		for p.peek().kind != tokRBrace {
			child, err := p.element(depth + 1)
			if err != nil {
				return ViewElement{}, err
			}
			element.Children = append(element.Children, child)
		}
		end, err = p.take(tokRBrace, "'}' after child elements")
	} else {
		end, err = p.take(tokSemi, "';' after leaf element")
	}
	if err != nil {
		return ViewElement{}, err
	}
	element.Span = mergeSpans(component.span, end.span)
	return element, nil
}

func (p *viewSyntaxParser) value(allowReference bool) (ViewValue, error) {
	current := p.peek()
	value := ViewValue{Span: current.span}
	switch current.kind {
	case tokString:
		value.Kind = "text"
		value.Text = current.lit
		p.index++
	case tokBool:
		value.Kind = "boolean"
		value.Boolean = current.lit == "true"
		p.index++
	case tokInt, tokMinus:
		literal := current.lit
		p.index++
		if current.kind == tokMinus {
			digits, err := p.take(tokInt, "integer after '-'")
			if err != nil {
				return ViewValue{}, err
			}
			literal += digits.lit
			value.Span = mergeSpans(current.span, digits.span)
		}
		integer, err := strconv.ParseInt(literal, 10, 64)
		if err != nil {
			return ViewValue{}, p.failure(CodeUnexpectedToken, value.Span, "view integer is outside signed 64-bit range")
		}
		value.Kind = "integer"
		value.Integer = integer
	case tokIdent:
		if !allowReference {
			return ViewValue{}, p.failure(CodeUnexpectedToken, current.span, "expected literal state initializer")
		}
		p.index++
		value.Kind = "reference"
		value.Text = current.lit
		if current.lit == "bind" {
			name, err := p.take(tokIdent, "state name after bind")
			if err != nil {
				return ViewValue{}, err
			}
			value.Kind = "binding"
			value.Text = name.lit
			value.Span = mergeSpans(current.span, name.span)
			if p.peek().kind == tokDot {
				p.index++
				property, err := p.take(tokIdent, "property name after binding '.'")
				if err != nil {
					return ViewValue{}, err
				}
				value.Text += "." + property.lit
				value.Span = mergeSpans(current.span, property.span)
			}
		}
	default:
		return ViewValue{}, p.failure(CodeUnexpectedToken, current.span, "expected view literal, reference or binding")
	}
	return value, nil
}
