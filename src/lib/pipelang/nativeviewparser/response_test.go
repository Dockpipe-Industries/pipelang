package nativeviewparser

import (
	"strings"
	"testing"
)

const validResponse = `{"contract":"pipelang.view-parser.response.v1","declaration":{"Name":"V","Span":{"file":"v.pipe","start":0,"end":21},"State":[],"Events":[],"Actions":[],"Root":{"Component":"Label","Properties":[{"Name":"text","Value":{"Kind":"text","Text":"hello","Boolean":false,"Integer":0,"Span":{"file":"v.pipe","start":10,"end":15}},"Span":{"file":"v.pipe","start":5,"end":15}}],"Children":[],"Span":{"file":"v.pipe","start":5,"end":20}}},"failure":null}`

func TestNativeViewResponseStrictDecoding(t *testing.T) {
	if result, err := decodeResponse([]byte(validResponse)); err != nil || result.Declaration == nil {
		t.Fatalf("valid response: %v", err)
	}
	failure := `{"contract":"pipelang.view-parser.response.v1","declaration":null,"failure":{"code":"PL2002","message":"expected view","span":{"file":"v.pipe","start":0,"end":0}}}`
	if result, err := decodeResponse([]byte(failure)); err != nil || result.Failure == nil {
		t.Fatalf("valid failure: %v", err)
	}
	cases := map[string]string{
		"duplicate root":     strings.Replace(validResponse, `"contract":`, `"contract":"other","contract":`, 1),
		"case alias":         strings.Replace(validResponse, `"Name":`, `"name":`, 1),
		"unknown field":      strings.Replace(validResponse, `"Root":`, `"Extra":true,"Root":`, 1),
		"missing field":      strings.Replace(validResponse, `"State":[],`, "", 1),
		"null array":         strings.Replace(validResponse, `"State":[]`, `"State":null`, 1),
		"null scalar":        strings.Replace(validResponse, `"Boolean":false`, `"Boolean":null`, 1),
		"null object":        strings.Replace(validResponse, `"Span":{"file":"v.pipe","start":0,"end":21}`, `"Span":null`, 1),
		"duplicate nested":   strings.Replace(validResponse, `"Text":"hello"`, `"Text":"first","Text":"hello"`, 1),
		"float":              strings.Replace(validResponse, `"Integer":0`, `"Integer":0.0`, 1),
		"exponent":           strings.Replace(validResponse, `"start":0`, `"start":0e0`, 1),
		"overflow":           strings.Replace(validResponse, `"Integer":0`, `"Integer":9223372036854775808`, 1),
		"string number":      strings.Replace(validResponse, `"Integer":0`, `"Integer":"0"`, 1),
		"wrong boolean":      strings.Replace(validResponse, `"Boolean":false`, `"Boolean":0`, 1),
		"wrong string":       strings.Replace(validResponse, `"Name":"V"`, `"Name":true`, 1),
		"low surrogate":      strings.Replace(validResponse, `"hello"`, `"\udc00"`, 1),
		"high surrogate":     strings.Replace(validResponse, `"hello"`, `"\ud800x"`, 1),
		"bad surrogate pair": strings.Replace(validResponse, `"hello"`, `"\ud800\u0041"`, 1),
		"invalid utf8":       strings.Replace(validResponse, "hello", "\xff", 1),
		"trailing":           validResponse + "{}",
		"unknown contract":   strings.Replace(validResponse, responseContract, "other", 1),
		"no outcome":         `{"contract":"pipelang.view-parser.response.v1","declaration":null,"failure":null}`,
		"both outcomes":      strings.Replace(failure, `"declaration":null`, `"declaration":`+strings.Split(strings.Split(validResponse, `"declaration":`)[1], `,"failure"`)[0], 1),
		"deep nesting":       strings.Replace(validResponse, `"Children":[]`, `"Children":[`+strings.Repeat(`{"Component":"A","Properties":[],"Children":[`, 50)+strings.Repeat(`]}`, 50)+`]`, 1),
		"oversized":          strings.Repeat(" ", maximumMessageBytes+1),
	}
	for name, data := range cases {
		t.Run(name, func(t *testing.T) {
			if result, err := decodeResponse([]byte(data)); err == nil {
				t.Fatalf("malformed response admitted: %#v", result)
			}
		})
	}
	for _, text := range []string{`"\ud83d\udeb2"`, `"\\ud800"`, `"é🚲"`, `"\u0000"`} {
		if _, err := decodeResponse([]byte(strings.Replace(validResponse, `"hello"`, text, 1))); err != nil {
			t.Fatalf("valid Unicode scalar %s refused: %v", text, err)
		}
	}
}
