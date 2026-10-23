package main

import (
	"fmt"
	"os"

	"github.com/Dockpipe-Industries/pipelang/src/lib/application"
)

func main() {
	if err := application.Run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
