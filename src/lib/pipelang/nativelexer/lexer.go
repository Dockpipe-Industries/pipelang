// Package nativelexer adapts an explicitly selected C++ lexer to the compiler's
// source-token boundary. It does not own parsing, semantic admission or fallback.
package nativelexer

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/internal/nativeprocess"
)

const requestContract = "pipelang.lexer.request.v1"
const responseContract = "pipelang.lexer.response.v1"
const maximumSourceBytes = 65536
const maximumRequestBytes = 512 << 10
const maximumResponseBytes = 16 << 20

// Lexer selects one checksum-bound native implementation for an entire analysis.
// Context may be omitted for command line callers; API callers can cancel it.
type Lexer struct {
	Path    string
	SHA256  string
	Context context.Context
}

type request struct {
	Contract string `json:"contract"`
	Source   string `json:"source"`
}

func (lexer Lexer) Tokenize(file *pipelang.SourceFile) (pipelang.LexicalResult, error) {
	if file == nil || file.ID == "" || !utf8.ValidString(file.Text) || len(file.Text) > maximumSourceBytes {
		return pipelang.LexicalResult{}, errors.New("native lexer requires an admitted UTF-8 source within 64 KiB")
	}
	payload, err := json.Marshal(request{Contract: requestContract, Source: file.Text})
	if err != nil || len(payload) > maximumRequestBytes {
		return pipelang.LexicalResult{}, errors.New("native lexer request exceeds its encoding contract")
	}
	ctx := lexer.Context
	if ctx == nil {
		ctx = context.Background()
	}
	executable := nativeprocess.Executable{Path: lexer.Path, SHA256: lexer.SHA256, Label: "native lexer"}
	output, err := nativeprocess.Run(ctx, executable, payload, maximumResponseBytes)
	if err != nil {
		return pipelang.LexicalResult{}, err
	}
	result, err := decodeResponse(output)
	if err != nil {
		return pipelang.LexicalResult{}, fmt.Errorf("decode native lexer response: %w", err)
	}
	if err := pipelang.ValidateLexicalResult(file, result); err != nil {
		return pipelang.LexicalResult{}, fmt.Errorf("invalid native lexer response: %w", err)
	}
	return result, nil
}
