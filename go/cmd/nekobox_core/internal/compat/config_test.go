package compat

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLegacyGeositeDatabase(t *testing.T) {
	var fixture bytes.Buffer
	fixture.WriteByte(0)
	writeVarint := func(value uint64) { var b [10]byte; n := binary.PutUvarint(b[:], value); fixture.Write(b[:n]) }
	writeString := func(value string) { writeVarint(uint64(len(value))); fixture.WriteString(value) }
	writeVarint(1)
	writeString("test")
	writeVarint(0)
	writeVarint(2)
	fixture.WriteByte(0)
	writeString("example.test")
	fixture.WriteByte(1)
	writeString(".example.org")
	path := filepath.Join(t.TempDir(), "geosite.db")
	if err := os.WriteFile(path, fixture.Bytes(), 0600); err != nil {
		t.Fatal(err)
	}
	config := object{"route": object{"geosite": object{"path": path}, "rules": []any{object{"geosite": []any{"test"}, "outbound": "proxy"}}}, "outbounds": []any{object{"type": "direct", "tag": "proxy"}}}
	input, _ := json.Marshal(config)
	output, err := Normalize(input)
	if err != nil {
		t.Fatal(err)
	}
	var result object
	json.Unmarshal(output, &result)
	rule := obj(list(obj(result["route"])["rules"])[0])
	if str(list(rule["domain"])[0]) != "example.test" || str(list(rule["domain_suffix"])[0]) != ".example.org" {
		t.Fatalf("incorrect database migration: %s", output)
	}
	if _, exists := rule["geosite"]; exists {
		t.Fatal("legacy matcher remained")
	}
}

func TestLegacyDNSAddresses(t *testing.T) {
	cases := []struct{ address, kind, host string }{
		{"1.1.1.1", "udp", "1.1.1.1"},
		{"2001:4860:4860::8888", "udp", "2001:4860:4860::8888"},
		{"tcp://[::1]:5353", "tcp", "::1"},
		{"tls://dns.example", "tls", "dns.example"},
		{"https://dns.example/dns-query", "https", "dns.example"},
		{"h3://dns.example/dns-query", "h3", "dns.example"},
		{"underlying://0.0.0.0", "local", ""},
	}
	for _, c := range cases {
		t.Run(c.address, func(t *testing.T) {
			server := object{"address": c.address}
			if err := migrateDNSServer(server); err != nil {
				t.Fatal(err)
			}
			if server["type"] != c.kind || str(server["server"]) != c.host {
				t.Fatalf("%v", server)
			}
		})
	}
}

func TestMissingGeoCategoryFailsInsteadOfDroppingRule(t *testing.T) {
	input := []byte(`{"route":{"geosite":{"path":"does-not-exist.db"},"rules":[{"geosite":["cn"],"outbound":"block"}]},"outbounds":[{"type":"block","tag":"block"}]}`)
	if _, err := Normalize(input); err == nil || !strings.Contains(err.Error(), "geosite") {
		t.Fatalf("expected missing database error, got %v", err)
	}
}

func TestNativeBlockOutboundPreserved(t *testing.T) {
	output, err := Normalize([]byte(`{"outbounds":[{"type":"selector","tag":"select","outbounds":["blocked"]},{"type":"block","tag":"blocked"}]}`))
	if err != nil {
		t.Fatal(err)
	}
	var root object
	json.Unmarshal(output, &root)
	if len(list(root["outbounds"])) != 2 {
		t.Fatalf("native selectable block outbound was removed: %s", output)
	}
}

func TestNestedRulesAndDirectOverride(t *testing.T) {
	input := []byte(`{"outbounds":[{"tag":"proxy","type":"direct","override_address":"127.0.0.1","override_port":8080}],"route":{"final":"proxy","rules":[{"type":"logical","mode":"or","rules":[{"geoip":["private"]},{"domain":["example.test"]}],"outbound":"proxy"}]}}`)
	output, err := Normalize(input)
	if err != nil {
		t.Fatal(err)
	}
	var root object
	json.Unmarshal(output, &root)
	rules := list(obj(root["route"])["rules"])
	if len(rules) != 2 {
		t.Fatalf("duplicated fallback override: %s", output)
	}
	if obj(rules[1])["override_address"] != "127.0.0.1" {
		t.Fatalf("lost direct destination override: %s", output)
	}
}

func TestPortHoppingMigration(t *testing.T) {
	output, err := Normalize([]byte(`{"outbounds":[{"type":"hysteria2","tag":"proxy","server":"127.0.0.1","server_port":443,"hop_ports":"1000-2000,3000","hop_interval":30,"password":"test","tls":{"enabled":true}}]}`))
	if err != nil {
		t.Fatal(err)
	}
	var root object
	json.Unmarshal(output, &root)
	out := obj(list(root["outbounds"])[0])
	if out["hop_interval"] != "30s" || str(list(out["server_ports"])[0]) != "1000:2000" || str(list(out["server_ports"])[1]) != "3000:3000" {
		t.Fatalf("%s", output)
	}
}
