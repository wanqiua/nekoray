//go:build with_wireguard

package main

func init() {
	compiledFeatures = append(compiledFeatures, "endpoint.wireguard")
}
