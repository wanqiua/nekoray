package main

import (
	"context"
	"grpc_server/gen"
	"strings"
	"testing"
)

func TestCheckCurrentAndUnavailableFeatures(t *testing.T) {
	s := &server{}
	for _, test := range []struct {
		name     string
		config   string
		category gen.FailureCategory
	}{
		{"rc4-md5", `{"outbounds":[{"tag":"proxy","type":"shadowsocks","server":"127.0.0.1","server_port":12345,"method":"rc4-md5","password":"local-test"}]}`, gen.FailureCategory_FailureNone},
		{"legacy DNS and sniff", legacyGUIConfig(12345), gen.FailureCategory_FailureNone},
		{"AnyTLS", `{"outbounds":[{"type":"anytls","server":"127.0.0.1","server_port":12345,"password":"local-test","tls":{"enabled":true,"insecure":true}}]}`, gen.FailureCategory_FailureNone},
		{"Snell", `{"outbounds":[{"type":"snell","server":"127.0.0.1","server_port":12345,"psk":"local-test","version":4}]}`, gen.FailureCategory_FailureNone},
		{"bf-cfb", `{"outbounds":[{"type":"shadowsocks","server":"127.0.0.1","server_port":12345,"method":"bf-cfb","password":"local-test"}]}`, gen.FailureCategory_FailureUnsupported},
		{"unknown protocol", `{"outbounds":[{"type":"unsupported-test-protocol"}]}`, gen.FailureCategory_FailureUnsupported},
		{"SOCKS TLS unsupported", `{"outbounds":[{"type":"socks","server":"127.0.0.1","server_port":12345,"tls":{"enabled":true}}]}`, gen.FailureCategory_FailureConfig},
	} {
		t.Run(test.name, func(t *testing.T) {
			response, err := s.Check(context.Background(), &gen.LoadConfigReq{CoreConfig: test.config})
			if err != nil || response.FailureCategory != test.category || (response.Error == "") != (test.category == gen.FailureCategory_FailureNone) {
				t.Fatalf("Check: %v %v", response, err)
			}
		})
	}
}

func TestCheckHashIncludesLegacySniffOverride(t *testing.T) {
	config := legacyGUIConfig(12345)
	first, err := checkBoxConfig(context.Background(), []byte(config))
	if err != nil {
		t.Fatal(err)
	}
	second, err := checkBoxConfig(context.Background(), []byte(strings.Replace(config, `"sniff_override_destination":false`, `"sniff_override_destination":true`, 1)))
	if err != nil || first == second {
		t.Fatalf("legacy runtime override missing from config identity: %v", err)
	}
}

func TestCancelledRequestHasCategoryAndIdentity(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	response, err := (&server{}).Test(ctx, &gen.TestReq{Mode: gen.TestMode_TcpPing, Address: "127.0.0.1:1"})
	if err != nil || response.FailureCategory != gen.FailureCategory_FailureCancelled || response.CoreFamily != "sing-box" || !strings.HasPrefix(response.BuildId, "sha256:") {
		t.Fatalf("cancelled test: %v %v", response, err)
	}
}

func TestCapabilitiesMatchOptionalBuildFeatures(t *testing.T) {
	features := map[string]bool{}
	for _, feature := range coreCapabilities().Features {
		features[feature] = true
	}
	for _, test := range []struct {
		feature string
		config  string
	}{
		{"outbound.hysteria2", `{"outbounds":[{"type":"hysteria2","server":"127.0.0.1","server_port":12345,"password":"local-test","tls":{"enabled":true,"insecure":true}}]}`},
		{"tls.utls", `{"outbounds":[{"type":"vless","server":"127.0.0.1","server_port":12345,"uuid":"75f2a015-3611-4c7f-bf7c-a948276b179f","tls":{"enabled":true,"insecure":true,"utls":{"enabled":true,"fingerprint":"chrome"}}}]}`},
		{"endpoint.wireguard", `{"endpoints":[{"type":"wireguard","tag":"proxy","address":["10.0.0.2/32"],"private_key":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","peers":[{"address":"127.0.0.1","port":12345,"public_key":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","allowed_ips":["0.0.0.0/0"]}]}]}`},
	} {
		t.Run(test.feature, func(t *testing.T) {
			response, err := (&server{}).Check(context.Background(), &gen.LoadConfigReq{CoreConfig: test.config})
			if err != nil {
				t.Fatal(err)
			}
			if features[test.feature] {
				if response.Error != "" {
					t.Fatalf("advertised feature rejected: %v", response)
				}
			} else if response.FailureCategory != gen.FailureCategory_FailureUnsupported {
				t.Fatalf("absent optional feature not classified as unsupported: %v", response)
			}
		})
	}
}
