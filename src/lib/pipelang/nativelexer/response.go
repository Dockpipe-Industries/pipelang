package nativelexer

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

// closedObject admits exact keys before typed decoding. encoding/json alone
// permits duplicate keys, case aliases, missing fields and null scalar defaults.
func closedObject(data []byte, names ...string) (map[string]json.RawMessage, error) {
	decoder := json.NewDecoder(bytes.NewReader(data))
	opening, err := decoder.Token()
	if err != nil || opening != json.Delim('{') {
		return nil, errors.New("expected a response object")
	}
	allowed := make(map[string]bool, len(names))
	for _, name := range names {
		allowed[name] = true
	}
	fields := make(map[string]json.RawMessage, len(names))
	for decoder.More() {
		key, err := decoder.Token()
		if err != nil {
			return nil, err
		}
		name, ok := key.(string)
		if !ok || !allowed[name] {
			return nil, fmt.Errorf("unsupported response field %q", name)
		}
		if _, exists := fields[name]; exists {
			return nil, fmt.Errorf("duplicate response field %q", name)
		}
		var value json.RawMessage
		if err := decoder.Decode(&value); err != nil {
			return nil, err
		}
		fields[name] = value
	}
	if closing, err := decoder.Token(); err != nil || closing != json.Delim('}') {
		return nil, errors.New("response object is not complete")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return nil, errors.New("response object contains trailing data")
	}
	if len(fields) != len(names) {
		return nil, errors.New("response object is missing required fields")
	}
	return fields, nil
}

func wireString(raw json.RawMessage) (string, error) {
	data := bytes.TrimSpace(raw)
	if len(data) == 0 || data[0] != '"' {
		return "", errors.New("expected a response string")
	}
	var value string
	if err := json.Unmarshal(data, &value); err != nil {
		return "", err
	}
	// Reject unpaired JSON surrogates instead of accepting encoding/json's
	// replacement rune; PipeLang text preserves Unicode scalar values exactly.
	for index := 1; index < len(data)-1; index++ {
		if data[index] != '\\' {
			continue
		}
		index++
		if data[index] != 'u' {
			continue
		}
		code, err := strconv.ParseUint(string(data[index+1:index+5]), 16, 16)
		if err != nil {
			return "", err
		}
		index += 4
		if code >= 0xdc00 && code <= 0xdfff {
			return "", errors.New("unpaired low surrogate in response string")
		}
		if code < 0xd800 || code > 0xdbff {
			continue
		}
		if index+6 >= len(data) || data[index+1] != '\\' || data[index+2] != 'u' {
			return "", errors.New("unpaired high surrogate in response string")
		}
		low, err := strconv.ParseUint(string(data[index+3:index+7]), 16, 16)
		if err != nil || low < 0xdc00 || low > 0xdfff {
			return "", errors.New("invalid surrogate pair in response string")
		}
		index += 6
	}
	return value, nil
}

func wirePosition(raw json.RawMessage) (int, error) {
	data := bytes.TrimSpace(raw)
	if len(data) == 0 || (data[0] != '-' && (data[0] < '0' || data[0] > '9')) {
		return 0, errors.New("expected an integer response offset")
	}
	var position int
	if err := json.Unmarshal(data, &position); err != nil {
		return 0, err
	}
	return position, nil
}

func decodeResponse(data []byte) (pipelang.LexicalResult, error) {
	fields, err := closedObject(data, "contract", "tokens", "failure")
	if err != nil {
		return pipelang.LexicalResult{}, err
	}
	contract, err := wireString(fields["contract"])
	if err != nil || contract != responseContract {
		return pipelang.LexicalResult{}, errors.New("unsupported native lexer response contract")
	}
	tokensNull := bytes.Equal(bytes.TrimSpace(fields["tokens"]), []byte("null"))
	failureNull := bytes.Equal(bytes.TrimSpace(fields["failure"]), []byte("null"))
	if tokensNull == failureNull {
		return pipelang.LexicalResult{}, errors.New("native lexer response must select exactly one nullable outcome")
	}
	if tokensNull {
		failure, err := closedObject(fields["failure"], "code", "message", "start", "end")
		if err != nil {
			return pipelang.LexicalResult{}, err
		}
		code, codeError := wireString(failure["code"])
		message, messageError := wireString(failure["message"])
		start, startError := wirePosition(failure["start"])
		end, endError := wirePosition(failure["end"])
		if err := errors.Join(codeError, messageError, startError, endError); err != nil {
			return pipelang.LexicalResult{}, err
		}
		return pipelang.LexicalResult{Failure: &pipelang.LexicalFailure{
			Code: pipelang.DiagnosticCode(code), Message: message, Start: start, End: end,
		}}, nil
	}
	var rawTokens []json.RawMessage
	if err := json.Unmarshal(fields["tokens"], &rawTokens); err != nil {
		return pipelang.LexicalResult{}, err
	}
	result := pipelang.LexicalResult{Tokens: make([]pipelang.LexicalToken, 0, len(rawTokens))}
	for _, raw := range rawTokens {
		token, err := closedObject(raw, "kind", "literal", "start", "end")
		if err != nil {
			return pipelang.LexicalResult{}, err
		}
		kind, kindError := wireString(token["kind"])
		literal, literalError := wireString(token["literal"])
		start, startError := wirePosition(token["start"])
		end, endError := wirePosition(token["end"])
		if err := errors.Join(kindError, literalError, startError, endError); err != nil {
			return pipelang.LexicalResult{}, err
		}
		result.Tokens = append(result.Tokens, pipelang.LexicalToken{
			Kind: kind, Literal: literal, Start: start, End: end,
		})
	}
	return result, nil
}
