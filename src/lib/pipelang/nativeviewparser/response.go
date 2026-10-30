package nativeviewparser

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"reflect"
	"strconv"
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

// This private decoder derives the exact member set from the owned wire types.
// It does not use encoding/json's permissive struct member matching: every key
// is required once, scalar nulls and case aliases fail, and arrays stay arrays.
// A bounded recursive walk applies before allocating typed collections.
type responseDecoder struct {
	decoder *json.Decoder
	nodes   int
}

func decodeResponse(data []byte) (pipelang.ViewSyntaxResult, error) {
	if len(data) > maximumMessageBytes || !utf8.Valid(data) {
		return pipelang.ViewSyntaxResult{}, fmt.Errorf("response byte or UTF-8 limit")
	}
	if err := validateEscapedScalars(data); err != nil {
		return pipelang.ViewSyntaxResult{}, err
	}
	var response struct {
		Contract    string                      `json:"contract"`
		Declaration *pipelang.ViewDeclaration   `json:"declaration"`
		Failure     *pipelang.ViewSyntaxFailure `json:"failure"`
	}
	decoder := responseDecoder{decoder: json.NewDecoder(bytes.NewReader(data))}
	decoder.decoder.UseNumber()
	if err := decoder.read(reflect.ValueOf(&response).Elem(), 0); err != nil {
		return pipelang.ViewSyntaxResult{}, err
	}
	if _, err := decoder.decoder.Token(); err != io.EOF {
		return pipelang.ViewSyntaxResult{}, fmt.Errorf("trailing response data")
	}
	if response.Contract != responseContract || (response.Declaration == nil) == (response.Failure == nil) {
		return pipelang.ViewSyntaxResult{}, fmt.Errorf("unsupported response contract or nonexclusive outcome")
	}
	return pipelang.ViewSyntaxResult{Declaration: response.Declaration, Failure: response.Failure}, nil
}

func (decoder *responseDecoder) read(destination reflect.Value, depth int) error {
	decoder.nodes++
	if depth > 48 || decoder.nodes > 262144 {
		return fmt.Errorf("response nesting or node limit")
	}
	token, err := decoder.decoder.Token()
	if err != nil {
		return err
	}
	if destination.Kind() == reflect.Pointer {
		if token == nil {
			return nil
		}
		destination.Set(reflect.New(destination.Type().Elem()))
		destination = destination.Elem()
	}
	switch destination.Kind() {
	case reflect.Struct:
		if token != json.Delim('{') {
			return fmt.Errorf("expected response object")
		}
		fields := make(map[string]int, destination.NumField())
		for index := 0; index < destination.NumField(); index++ {
			field := destination.Type().Field(index)
			name := field.Tag.Get("json")
			if name == "" {
				name = field.Name
			}
			fields[name] = index
		}
		seen := make(map[string]bool, len(fields))
		for decoder.decoder.More() {
			key, err := decoder.decoder.Token()
			if err != nil {
				return err
			}
			name, ok := key.(string)
			index, exists := fields[name]
			if !ok || !exists || seen[name] {
				return fmt.Errorf("unknown or duplicate response member %q", name)
			}
			seen[name] = true
			if err := decoder.read(destination.Field(index), depth+1); err != nil {
				return err
			}
		}
		if len(seen) != len(fields) {
			return fmt.Errorf("missing response members")
		}
		closing, err := decoder.decoder.Token()
		if err != nil || closing != json.Delim('}') {
			return fmt.Errorf("incomplete response object")
		}
	case reflect.Slice:
		if token != json.Delim('[') {
			return fmt.Errorf("expected response array")
		}
		for decoder.decoder.More() {
			item := reflect.New(destination.Type().Elem()).Elem()
			if err := decoder.read(item, depth+1); err != nil {
				return err
			}
			destination.Set(reflect.Append(destination, item))
		}
		closing, err := decoder.decoder.Token()
		if err != nil || closing != json.Delim(']') {
			return fmt.Errorf("incomplete response array")
		}
	case reflect.String:
		value, ok := token.(string)
		if !ok {
			return fmt.Errorf("expected response string")
		}
		destination.SetString(value)
	case reflect.Bool:
		value, ok := token.(bool)
		if !ok {
			return fmt.Errorf("expected response boolean")
		}
		destination.SetBool(value)
	case reflect.Int, reflect.Int64:
		number, ok := token.(json.Number)
		if !ok {
			return fmt.Errorf("expected response integer")
		}
		value, err := strconv.ParseInt(number.String(), 10, destination.Type().Bits())
		if err != nil {
			return err
		}
		destination.SetInt(value)
	default:
		return fmt.Errorf("unsupported response type")
	}
	return nil
}

// Reject malformed UTF-16 escapes before encoding/json can replace them.
func validateEscapedScalars(data []byte) error {
	for index := 0; index < len(data); index++ {
		if data[index] != '"' {
			continue
		}
		index++
		for index < len(data) && data[index] != '"' {
			if data[index] != '\\' {
				index++
				continue
			}
			index++
			if index >= len(data) {
				return fmt.Errorf("truncated string escape")
			}
			if data[index] != 'u' {
				index++
				continue
			}
			if index+4 >= len(data) {
				return fmt.Errorf("truncated Unicode escape")
			}
			code, err := strconv.ParseUint(string(data[index+1:index+5]), 16, 16)
			if err != nil {
				return err
			}
			index += 5
			if code >= 0xdc00 && code <= 0xdfff {
				return fmt.Errorf("unpaired low surrogate")
			}
			if code < 0xd800 || code > 0xdbff {
				continue
			}
			if index+5 >= len(data) || (data[index] != '\\' || data[index+1] != 'u') {
				return fmt.Errorf("unpaired high surrogate")
			}
			low, err := strconv.ParseUint(string(data[index+2:index+6]), 16, 16)
			if err != nil || low < 0xdc00 || low > 0xdfff {
				return fmt.Errorf("invalid surrogate pair")
			}
			index += 6
		}
	}
	return nil
}
