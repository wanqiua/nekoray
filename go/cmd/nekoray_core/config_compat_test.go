package main

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/xtls/xray-core/core"
	"github.com/xtls/xray-core/proxy/shadowsocks"
)

func TestLegacyConfigurationPreserved(t *testing.T) {
	input := `{"outbounds":[{"tag":"proxy","protocol":"shadowsocks","domainStrategy":"AsIs","settings":{"plugin":"v2ray-plugin","pluginOpts":"mode=websocket;host=localhost","servers":[{"address":"127.0.0.1","port":1234,"method":"rc4-md5","password":"local-test"}]},"streamSettings":{"security":"tls","tlsSettings":{"allowInsecure":true,"serverName":"localhost"}}}]}`
	got, err := normalizeXrayConfig(input)
	if err != nil {
		t.Fatal(err)
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

func TestOutboundStrategyMigration(t *testing.T) {
	input := `{"routing":{"domainStrategy":"IPIfNonMatch"},"outbounds":[{"protocol":"freedom","domainStrategy":"UseIPv6","settings":{"domainStrategy":"UseIPv4"},"streamSettings":{"sockopt":{"domainStrategy":"UseIP"}}}]}`
	got, err := normalizeXrayConfig(input)
	if err != nil {
		t.Fatal(err)
	}
	var config map[string]any
	if err := json.Unmarshal([]byte(got), &config); err != nil {
		t.Fatal(err)
	}
	out := config["outbounds"].([]any)[0].(map[string]any)
	if out["targetStrategy"] != "UseIPv6" || out["domainStrategy"] != nil ||
		out["settings"].(map[string]any)["domainStrategy"] != "UseIPv4" ||
		out["streamSettings"].(map[string]any)["sockopt"].(map[string]any)["domainStrategy"] != "UseIP" ||
		config["routing"].(map[string]any)["domainStrategy"] != "IPIfNonMatch" {
		t.Fatalf("strategy migration changed unrelated settings: %s", got)
	}
	if _, err := checkXrayConfig(got); err != nil {
		t.Fatal(err)
	}
	if _, err := normalizeXrayConfig(`{"outbounds":[{"domainStrategy":"UseIPv6","targetStrategy":"UseIPv4"}]}`); err == nil {
		t.Fatal("conflicting strategies were accepted")
	}
}

func TestModernFieldsUseNativeValidation(t *testing.T) {
	for _, field := range []string{
		`"targetStrategy":"ForceIPv6"`,
		`"streamSettings":{"security":"tls","tlsSettings":{"pinnedPeerCertSha256":"` + strings.Repeat("ab", 32) + `"}}`,
		`"streamSettings":{"security":"tls","tlsSettings":{"verifyPeerCertByName":"localhost"}}`,
	} {
		input := `{"outbounds":[{"protocol":"freedom",` + field + `}]}`
		got, err := normalizeXrayConfig(input)
		if err != nil || got != input {
			t.Fatalf("modern config was changed: %v", err)
		}
		if _, err := checkXrayConfig(got); err != nil {
			t.Fatal(err)
		}
	}
}
