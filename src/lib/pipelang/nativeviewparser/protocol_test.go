package nativeviewparser

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

func TestNativeViewRequestRefusals(t *testing.T) {
	parser := configuredParser(t)
	file := &pipelang.SourceFile{ID: "v.pipe", Text: `view V { Label(); }`}
	lexical, _ := pipelang.TokenizeSource(file)
	payload, err := json.Marshal(request{Contract: requestContract, File: file.ID, SourceBytes: len(file.Text), Tokens: lexical.Tokens})
	if err != nil {
		t.Fatal(err)
	}
	valid := string(payload)
	cases := map[string]string{
		"unknown":         strings.Replace(valid, `"file":`, `"extra":false,"file":`, 1),
		"duplicate":       strings.Replace(valid, `"file":`, `"file":"x","file":`, 1),
		"alias":           strings.Replace(valid, `"tokens":`, `"Tokens":`, 1),
		"missing":         strings.Replace(valid, `"file":"v.pipe",`, "", 1),
		"null string":     strings.Replace(valid, `"file":"v.pipe"`, `"file":null`, 1),
		"null tokens":     strings.Replace(valid, `"tokens":[`, `"tokens":null,"extra":[`, 1),
		"null number":     strings.Replace(valid, fmt.Sprintf(`"source_bytes":%d`, len(file.Text)), `"source_bytes":null`, 1),
		"float":           strings.Replace(valid, `"start":0`, `"start":0.0`, 1),
		"exponent":        strings.Replace(valid, `"start":0`, `"start":0e0`, 1),
		"overflow":        strings.Replace(valid, `"start":0`, `"start":18446744073709551616`, 1),
		"negative":        strings.Replace(valid, `"start":0`, `"start":-1`, 1),
		"token duplicate": strings.Replace(valid, `"kind":`, `"kind":"other","kind":`, 1),
		"token alias":     strings.Replace(valid, `"kind":`, `"Kind":`, 1),
		"unknown kind":    strings.Replace(valid, `"kind":"identifier"`, `"kind":"other"`, 1),
		"early eof":       strings.Replace(valid, `"kind":"identifier"`, `"kind":"eof"`, 1),
		"missing eof":     strings.Replace(valid, `"kind":"eof"`, `"kind":"identifier"`, 1),
		"overlap":         strings.Replace(valid, `"start":5`, `"start":0`, 1),
		"surrogate":       strings.Replace(valid, `"v.pipe"`, `"\ud800"`, 1),
		"utf8":            strings.Replace(valid, `v.pipe`, "\xff", 1),
		"trailing":        valid + "{}",
		"nesting":         `{"contract":` + strings.Repeat("[", 8) + strings.Repeat("]", 8) + `}`,
		"size":            strings.Repeat(" ", maximumMessageBytes+1),
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, parser.Path)
			command.Stdin = strings.NewReader(input)
			var output, diagnostic bytes.Buffer
			command.Stdout = &output
			command.Stderr = &diagnostic
			if err := command.Run(); err == nil || output.Len() != 0 || diagnostic.Len() == 0 {
				t.Fatalf("protocol refusal was not empty/unsuccessful: err=%v stdout=%q stderr=%q", err, output.String(), diagnostic.String())
			}
		})
	}
}

func TestNativeViewAdapterNoFallback(t *testing.T) {
	configuredParser(t)
	file := &pipelang.SourceFile{ID: "v.pipe", Text: `view V { Label(); }`}
	for name, body := range map[string]string{
		"process failure":    "exit 7\n",
		"malformed response": "printf '%s' '{\"contract\":null}'\n",
		"foreign syntax":     "cat <<'END'\n" + validResponse + "\nEND\n",
		"empty response":     "exit 0\n",
	} {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "parser")
			data := []byte("#!/bin/sh\n" + body)
			if err := os.WriteFile(path, data, 0700); err != nil {
				t.Fatal(err)
			}
			hash := sha256.Sum256(data)
			parser := Parser{Path: path, SHA256: hex.EncodeToString(hash[:])}
			if tree, err := pipelang.ParseViewSourceWithParser(file, nil, parser.Parse); err == nil || tree != nil {
				t.Fatalf("component failure returned fallback/partial tree: %#v / %v", tree, err)
			}
		})
	}
}
