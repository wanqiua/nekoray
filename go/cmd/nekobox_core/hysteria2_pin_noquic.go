//go:build !with_quic

package main

import "github.com/sagernet/sing-box/adapter/outbound"

func registerPinnedHysteria2(registry *outbound.Registry) {}
