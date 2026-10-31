package pipelang

import (
	"fmt"
	"unicode/utf8"
)

// LexicalToken is the value boundary shared by the reference lexer and explicitly
// selected experimental lexers. Offsets are half-open source byte positions.
type LexicalToken struct {
	Kind    string `json:"kind"`
	Literal string `json:"literal"`
	Start   int    `json:"start"`
	End     int    `json:"end"`
}

type LexicalFailure struct {
	Code    DiagnosticCode `json:"code"`
	Message string         `json:"message"`
	Start   int            `json:"start"`
	End     int            `json:"end"`
}

type LexicalResult struct {
	Tokens  []LexicalToken  `json:"tokens"`
	Failure *LexicalFailure `json:"failure"`
}

// SourceLexer supplies tokens for one already admitted SourceFile. An error is
// a component failure; a source lexical error is represented by Failure. This
// experimental hook never selects a lexer through global or environment state.
type SourceLexer func(file *SourceFile) (LexicalResult, error)

var lexicalKindNames = [...]string{
	"eof", "identifier", "integer", "float", "text", "boolean", "interface",
	"class", "struct", "public", "private", "lbrace", "rbrace", "lparen",
	"rparen", "lbracket", "rbracket", "colon", "semicolon", "comma", "dot",
	"question", "assign", "arrow", "plus", "minus", "star", "slash", "bang",
	"equal", "not_equal", "less", "less_equal", "greater", "greater_equal", "and", "or",
}

func lexicalKind(name string) (tokenKind, bool) {
	for index, candidate := range lexicalKindNames {
		if candidate == name {
			return tokenKind(index), true
		}
	}
	return tokEOF, false
}

// TokenizeSource exposes the unchanged Go lexer as the reference value boundary.
// Ordinary compiler entrypoints continue calling lex directly.
func TokenizeSource(file *SourceFile) (LexicalResult, error) {
	if file == nil || file.ID == "" || !utf8.ValidString(file.Text) {
		return LexicalResult{}, fmt.Errorf("lexer requires an admitted UTF-8 source file")
	}
	sources := &SourceSet{files: []*SourceFile{file}, byID: map[FileID]*SourceFile{file.ID: file}}
	tokens, err := lex(sources, file)
	if err != nil {
		diagnostics, ok := AsDiagnostics(err)
		if !ok || len(diagnostics) != 1 || diagnostics[0].Category != CategoryLexical {
			return LexicalResult{}, err
		}
		diagnostic := diagnostics[0]
		return LexicalResult{Failure: &LexicalFailure{
			Code: diagnostic.Code, Message: diagnostic.Message,
			Start: diagnostic.Primary.Start, End: diagnostic.Primary.End,
		}}, nil
	}
	result := LexicalResult{Tokens: make([]LexicalToken, 0, len(tokens))}
	for _, token := range tokens {
		result.Tokens = append(result.Tokens, LexicalToken{
			Kind: lexicalKindNames[token.kind], Literal: token.lit,
			Start: token.span.Start, End: token.span.End,
		})
	}
	return result, nil
}

// ValidateLexicalResult checks the foreign token boundary without running the Go
// lexer again. The selected component owns tokenization; the compiler owns file
// identity, valid spans, known kinds, terminal EOF and diagnostic conversion.
func ValidateLexicalResult(file *SourceFile, result LexicalResult) error {
	if file == nil || file.ID == "" || !utf8.ValidString(file.Text) {
		return fmt.Errorf("lexer requires an admitted UTF-8 source file")
	}
	length := len(file.Text)
	if result.Failure != nil {
		failure := result.Failure
		if len(result.Tokens) != 0 || failure.Start < 0 || failure.End < failure.Start || failure.End > length {
			return fmt.Errorf("lexer failure has invalid tokens or source extent")
		}
		switch failure.Code {
		case CodeUnexpectedChar, CodeInvalidNumber, CodeInvalidEscape, CodeUnterminatedText:
		default:
			return fmt.Errorf("unsupported lexical diagnostic code %q", failure.Code)
		}
		if failure.Message == "" || !utf8.ValidString(failure.Message) || len(failure.Message) > 1024 {
			return fmt.Errorf("lexer failure has an invalid diagnostic message")
		}
		return nil
	}
	if len(result.Tokens) == 0 || len(result.Tokens) > length+1 {
		return fmt.Errorf("lexer result has an invalid token count")
	}
	previousEnd := 0
	for index, value := range result.Tokens {
		kind, ok := lexicalKind(value.Kind)
		if !ok || value.Start < previousEnd || value.End < value.Start || value.End > length || !utf8.ValidString(value.Literal) {
			return fmt.Errorf("lexer token %d has an invalid kind, literal or span", index)
		}
		if index == len(result.Tokens)-1 {
			if kind != tokEOF || value.Start != length || value.End != length || value.Literal != "" {
				return fmt.Errorf("lexer result is missing its canonical terminal EOF")
			}
		} else {
			if kind == tokEOF || value.Start == value.End {
				return fmt.Errorf("lexer token %d is empty or contains early EOF", index)
			}
			if kind != tokString && value.Literal != file.Text[value.Start:value.End] {
				return fmt.Errorf("lexer token %d does not retain its source bytes", index)
			}
		}
		previousEnd = value.End
	}
	return nil
}

func tokensFromLexicalResult(sources *SourceSet, file *SourceFile, result LexicalResult) ([]token, error) {
	if err := ValidateLexicalResult(file, result); err != nil {
		return nil, err
	}
	if failure := result.Failure; failure != nil {
		span := Span{File: file.ID, Start: failure.Start, End: failure.End}
		return nil, oneDiagnostic(sources, failure.Code, CategoryLexical, span, failure.Message)
	}
	tokens := make([]token, 0, len(result.Tokens))
	for _, value := range result.Tokens {
		kind, _ := lexicalKind(value.Kind)
		tokens = append(tokens, token{kind: kind, lit: value.Literal,
			span: Span{File: file.ID, Start: value.Start, End: value.End}})
	}
	return tokens, nil
}
