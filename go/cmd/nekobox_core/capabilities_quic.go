//go:build with_quic

package main

func init() {
	compiledFeatures = append(compiledFeatures, "outbound.hysteria", "outbound.hysteria2", "outbound.tuic", "transport.quic", "dns.quic", "dns.http3", "hysteria2.certificate-pin")
}
