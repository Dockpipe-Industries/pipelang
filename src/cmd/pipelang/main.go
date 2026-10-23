package main

import (
	"fmt"
	"os"

	"github.com/Dockpipe-Industries/pipelang/src/lib/application"
)

var releaseVersion = "development"
var releaseSource = "unknown"

func main() {
	if len(os.Args) == 2 && os.Args[1] == "--version" {
		fmt.Printf("%s %s\n", releaseVersion, releaseSource)
		return
	}

	if err := application.Run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
