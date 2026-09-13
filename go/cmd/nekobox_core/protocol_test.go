package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"grpc_server/testutil"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/sagernet/sing-box/adapter"
	R "github.com/sagernet/sing-box/route/rule"
	M "github.com/sagernet/sing/common/metadata"
)

func loopbackCertificate(t *testing.T) (string, string) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "localhost"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		DNSNames: []string{"localhost"}, IPAddresses: []net.IP{net.IPv4(127, 0, 0, 1)},
		IsCA: true, BasicConstraintsValid: true,
		KeyUsage:    x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	cert, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	keyBytes, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	return string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: cert})), string(pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyBytes}))
}

func TestCurrentProxyProtocols(t *testing.T) {
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { io.WriteString(w, "sing-box-latest") }))
	defer target.Close()
	certificate, key := loopbackCertificate(t)
	for _, protocol := range []string{"shadowsocks", "vmess", "vless", "trojan", "hysteria2", "tuic"} {
		t.Run(protocol, func(t *testing.T) {
			port := testutil.FreePort(t)
			id := "75f2a015-3611-4c7f-bf7c-a948276b179f"
			in := map[string]any{"type": protocol, "tag": "server", "listen": "127.0.0.1", "listen_port": port}
			out := map[string]any{"type": protocol, "tag": "proxy", "server": "127.0.0.1", "server_port": port}
			switch protocol {
			case "shadowsocks":
				in["method"] = "aes-128-gcm"
				in["password"] = "test-password"
				out["method"] = "aes-128-gcm"
				out["password"] = "test-password"
			case "vmess", "vless":
				in["users"] = []any{map[string]any{"uuid": id}}
				out["uuid"] = id
				if protocol == "vmess" {
					out["security"] = "auto"
				}
			case "trojan", "hysteria2":
				in["users"] = []any{map[string]any{"password": "test-password"}}
				out["password"] = "test-password"
			case "tuic":
				in["users"] = []any{map[string]any{"uuid": id, "password": "test-password"}}
				out["uuid"] = id
				out["password"] = "test-password"
				out["congestion_control"] = "cubic"
			}
			if protocol == "trojan" || protocol == "hysteria2" || protocol == "tuic" {
				in["tls"] = map[string]any{"enabled": true, "certificate": []string{certificate}, "key": []string{key}, "alpn": []string{"h3"}}
				out["tls"] = map[string]any{"enabled": true, "server_name": "localhost", "certificate": []string{certificate}, "alpn": []string{"h3"}}
			}
			if protocol == "hysteria2" {
				// Exercise the old GUI's port-hopping schema against the new core.
				out["hop_ports"] = strconv.Itoa(port)
				out["hop_interval"] = 30
			}
			serverConfig := map[string]any{"log": map[string]any{"level": "error"}, "inbounds": []any{in}, "outbounds": []any{map[string]any{"type": "direct", "tag": "direct"}}}
			bytes, _ := json.Marshal(serverConfig)
			server, err := NewBoxInstance(bytes, nil)
			if err != nil {
				t.Fatal(err)
			}
			defer server.Close()
			clientConfig := map[string]any{"log": map[string]any{"level": "error"}, "outbounds": []any{out}}
			bytes, _ = json.Marshal(clientConfig)
			clientCore, err := NewBoxInstance(bytes, nil)
			if err != nil {
				t.Fatal(err)
			}
			defer clientCore.Close()
			client := createProxyHttpClient(clientCore)
			client.Timeout = 8 * time.Second
			defer client.CloseIdleConnections()
			response, err := client.Get(target.URL)
			if err != nil {
				t.Fatal(err)
			}
			defer response.Body.Close()
			payload, err := io.ReadAll(response.Body)
			if err != nil || string(payload) != "sing-box-latest" {
				t.Fatalf("%q: %v", payload, err)
			}
		})
	}
}

func TestLegacySniffOverridePreserved(t *testing.T) {
	config := strings.Replace(legacyGUIConfig(testutil.FreePort(t)), `"sniff_override_destination":false`, `"sniff_override_destination":true`, 1)
	core, err := NewBoxInstance([]byte(config), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer core.Close()
	action, ok := core.Router().Rules()[0].Action().(*R.RuleActionSniff)
	if !ok || !action.OverrideDestination {
		t.Fatal("legacy sniff destination override was lost")
	}
}

func TestOriginalReleaseGeoDatabases(t *testing.T) {
	directory := os.Getenv("NEKORAY_TEST_ASSET_DIR")
	if directory == "" {
		t.Skip("set NEKORAY_TEST_ASSET_DIR to the original 3.26 release directory")
	}
	config := map[string]any{
		"log":       map[string]any{"level": "error"},
		"outbounds": []any{map[string]any{"type": "direct", "tag": "proxy"}},
		"route": map[string]any{
			"geosite": map[string]any{"path": filepath.Join(directory, "geosite.db")},
			"geoip":   map[string]any{"path": filepath.Join(directory, "geoip.db")},
			"rules": []any{
				map[string]any{"geosite": []string{"cn"}, "outbound": "proxy"},
				map[string]any{"geoip": []string{"cn"}, "outbound": "proxy"},
			},
		},
	}
	bytes, _ := json.Marshal(config)
	core, err := NewBoxInstance(bytes, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer core.Close()
	if !core.Router().Rules()[0].Match(&adapter.InboundContext{Destination: M.ParseSocksaddr("www.baidu.com:443")}) {
		t.Fatal("geosite CN matcher lost its domain data")
	}
	if !core.Router().Rules()[1].Match(&adapter.InboundContext{Destination: M.ParseSocksaddr("223.5.5.5:443")}) {
		t.Fatal("geoip CN matcher lost its address data")
	}
}
