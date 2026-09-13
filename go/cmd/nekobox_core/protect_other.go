//go:build !linux

package main

import "fmt"

func startProtection(path string, mark int) error {
	return fmt.Errorf("socket protection server is supported only on Linux")
}
