// Package nativeviewparser adapts an explicitly selected C++ syntax parser to
// admitted PipeView tokens. SDK semantics and source admission remain in Go.
package nativeviewparser

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/internal/nativeprocess"
)

const requestContract = "pipelang.view-parser.request.v1"
const responseContract = "pipelang.view-parser.response.v1"
const maximumMessageBytes = 16 << 20

// Parser binds one executable identity for the complete compilation. Context
// supports cancellation; the shared process adapter also enforces its deadline.
type Parser struct {
	Path    string
	SHA256  string
	Context context.Context
}

type request struct {
	Contract    string                  `json:"contract"`
	File        pipelang.FileID         `json:"file"`
	SourceBytes int                     `json:"source_bytes"`
	Tokens      []pipelang.LexicalToken `json:"tokens"`
}

func (parser Parser) Parse(file *pipelang.SourceFile, tokens []pipelang.LexicalToken) (pipelang.ViewSyntaxResult, error) {
	if file == nil || len(file.Text) == 0 || len(file.Text) > 65536 {
		return pipelang.ViewSyntaxResult{}, fmt.Errorf("native view parser requires an admitted source within 64 KiB")
	}
	if err := pipelang.ValidateLexicalResult(file, pipelang.LexicalResult{Tokens: tokens}); err != nil {
		return pipelang.ViewSyntaxResult{}, err
	}
	payload, err := json.Marshal(request{Contract: requestContract, File: file.ID, SourceBytes: len(file.Text), Tokens: tokens})
	if err != nil || len(payload) > maximumMessageBytes {
		return pipelang.ViewSyntaxResult{}, fmt.Errorf("native view parser request exceeds its encoding contract")
	}
	ctx := parser.Context
	if ctx == nil {
		ctx = context.Background()
	}
	executable := nativeprocess.Executable{Path: parser.Path, SHA256: parser.SHA256, Label: "native view parser"}
	output, err := nativeprocess.Run(ctx, executable, payload, maximumMessageBytes)
	if err != nil {
		return pipelang.ViewSyntaxResult{}, err
	}
	result, err := decodeResponse(output)
	if err != nil {
		return pipelang.ViewSyntaxResult{}, fmt.Errorf("decode native view parser response: %w", err)
	}
	if err := pipelang.ValidateViewSyntaxResult(file, tokens, result); err != nil {
		return pipelang.ViewSyntaxResult{}, fmt.Errorf("invalid native view parser response: %w", err)
	}
	return result, nil
}
