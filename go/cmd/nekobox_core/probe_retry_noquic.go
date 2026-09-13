//go:build !with_quic

package main

func isLocalQUICClose(error) bool { return false }
