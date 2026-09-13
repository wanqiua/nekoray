package main

import (
	"strings"
	"testing"

	"github.com/xtls/xray-core/core"
	"github.com/xtls/xray-core/proxy/shadowsocks"
)

func TestLegacyConfigurationPreserved(t *testing.T) {
	input := `{"outbounds":[{"tag":"proxy","protocol":"shadowsocks","domainStrategy":"AsIs","settings":{"plugin":"v2ray-plugin","pluginOpts":"mode=websocket;host=localhost","servers":[{"address":"127.0.0.1","port":1234,"method":"rc4-md5","password":"local-test"}]},"streamSettings":{"security":"tls","tlsSettings":{"allowInsecure":true,"serverName":"localhost"}}}]}`
	got, err := normalizeXrayConfig(input)
	if err != nil || got != input {
		t.Fatalf("legacy config was changed: %v", err)
	}
	parsed, err := core.LoadConfig("json", strings.NewReader(got))
	if err != nil {
		t.Fatal(err)
	}
	proxy, err := parsed.Outbound[0].ProxySettings.GetInstance()
	if err != nil {
		t.Fatal(err)
	}
	settings := proxy.(*shadowsocks.ClientConfig)
	if settings.Plugin != "v2ray-plugin" || settings.PluginOpts != "mode=websocket;host=localhost" {
		t.Fatal("legacy plugin fields were not preserved")
	}
}

func TestModernFieldsAreNotSilentlyIgnored(t *testing.T) {
	for _, input := range []string{
		`{"outbounds":[{"targetStrategy":"ForceIPv6"}]}`,
		`{"outbounds":[{"streamSettings":{"tlsSettings":{"pinnedPeerCertSha256":"trusted-pin"}}}]}`,
		`{"outbounds":[{"streamSettings":{"tlsSettings":{"verifyPeerCertByName":"localhost"}}}]}`,
	} {
		if _, err := normalizeXrayConfig(input); err == nil {
			t.Fatal("newer-only field was silently ignored")
		}
	}
}
