// This optional build-time example authors a bounded UI through the experimental
// Go SDK. PipeLang view syntax and reactive state are subsequent slices.
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/forgepipe"
)

func declaration() (pipeview.Document, error) {
	count := pipeview.State("count", int64(0))
	name := pipeview.State("name", "Rider")
	enabled := pipeview.State("enabled", true)
	return pipeview.Compile(pipeview.Column("root",
		pipeview.StaticText("title", "ForgePipe UI"),
		pipeview.StaticText("description", "Compiled PipeLang / same declaration"),
		pipeview.Label("count-label", "Count: ", count),
		pipeview.TextField("name-field", name, pipeview.Change[string]("name-edited")),
		pipeview.Toggle("enabled-toggle", "Enable updates", enabled, pipeview.Change[bool]("enabled-edited")),
		pipeview.Row("actions",
			pipeview.Button("increment-button", "Increment", pipeview.Click("increment")),
			pipeview.Button("reset-button", "Reset", pipeview.Click("reset")),
		),
	))
}

func run(arguments []string) error {
	flags := flag.NewFlagSet("pipelang-pipeview-demo", flag.ContinueOnError)
	output := flags.String("output", "", "Absolute generated PipeViewDocument.hpp path")
	if err := flags.Parse(arguments); err != nil {
		return err
	}
	if !filepath.IsAbs(*output) || flags.NArg() != 0 {
		return fmt.Errorf("an absolute --output path and no positional arguments are required")
	}
	document, err := declaration()
	if err != nil {
		return err
	}
	transport, err := forgepipe.Encode(document, forgepipe.Options{Scale: 1.125, Focus: "#FFAD45FF"})
	if err != nil {
		return err
	}
	header := "#pragma once\n#include <string>\nnamespace compiled_ui {\ninline std::string view()\n{\n    return " + strconv.Quote(string(transport)) + ";\n}\n}\n"
	if info, err := os.Lstat(*output); err == nil && !info.Mode().IsRegular() {
		return fmt.Errorf("output must be a regular file")
	} else if err != nil && !os.IsNotExist(err) {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(*output), ".pipeview-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err := file.WriteString(header); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), *output)
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
}
