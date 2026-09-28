package main

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/pem"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"grpc_server/testutil"
)

// Exercise the exact native JSON field emitted by the GUI, through real
// VLESS connections. Both Go TLS and Chrome uTLS must enforce the pin even
// though allowInsecure disables CA and hostname verification.
func TestCertificatePinTLS(t *testing.T) {
	certificate, key := selfSignedCompatibilityCertificate(t)
	block, _ := pem.Decode([]byte(certificate))
	hash := sha256.Sum256(block.Bytes)
	pin := base64.StdEncoding.EncodeToString(hash[:])
	wrongHash := sha256.Sum256([]byte("wrong certificate"))
	wrong := base64.StdEncoding.EncodeToString(wrongHash[:])
	for _, network := range []string{"tcp", "ws"} {
		for _, chain := range []bool{false, true} {
			certificates := certificate
			if chain {
				// A second DER certificate changes the chain hash: a leaf pin
				// must fail closed, documenting the legacy core limitation.
				second, _ := selfSignedCompatibilityCertificate(t)
				certificates += second
			}
			port := testutil.FreePort(t)
			startCompatibilityInstance(t, compatibilityConfig(t,
				[]any{map[string]any{"tag": "server", "listen": "127.0.0.1", "port": port, "protocol": "vless",
					"settings": map[string]any{"clients": []any{map[string]any{"id": compatibilityUUID}}, "decryption": "none"},
					"streamSettings": map[string]any{"network": network, "security": "tls",
						"tlsSettings": map[string]any{"certificates": []any{map[string]any{
							"certificate": strings.Split(strings.TrimSpace(certificates), "\n"),
							"key":         strings.Split(strings.TrimSpace(key), "\n"),
						}}}}}},
				[]any{map[string]any{"tag": "direct", "protocol": "freedom", "settings": map[string]any{}}}))
			target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { io.WriteString(w, "pin-verified") }))
			t.Cleanup(target.Close)
			for _, fingerprint := range []string{"", "chrome"} {
				for _, tc := range []struct {
					name            string
					pins            []string
					insecure, works bool
				}{
					{"correct", []string{pin}, true, !chain},
					{"wrong", []string{wrong}, true, false},
					{"multiple", []string{wrong, pin}, true, !chain},
					{"ca-required", []string{pin}, false, false},
				} {
					chainName := "leaf"
					if chain {
						chainName = "chain"
					}
					t.Run(network+"/"+chainName+"/"+fingerprint+"/"+tc.name, func(t *testing.T) {
						config := compatibilityConfig(t, nil, []any{map[string]any{"tag": "proxy", "protocol": "vless",
							"settings": map[string]any{"vnext": []any{map[string]any{"address": "127.0.0.1", "port": port,
								"users": []any{map[string]any{"id": compatibilityUUID, "encryption": "none"}}}}},
							"streamSettings": map[string]any{"network": network, "security": "tls", "tlsSettings": map[string]any{
								"serverName": "disguised.example.invalid", "allowInsecure": tc.insecure,
								"fingerprint": fingerprint, "pinnedPeerCertificateChainSha256": tc.pins,
							}}}})
						if _, err := checkXrayConfig(config); err != nil {
							t.Fatal(err)
						}
						instance := startCompatibilityInstance(t, config)
						client := createProxyHttpClient(instance)
						client.Timeout = 3 * time.Second
						defer client.CloseIdleConnections()
						response, err := client.Get(target.URL)
						if err != nil {
							if tc.works {
								t.Fatal(err)
							}
							return
						}
						defer response.Body.Close()
						body, err := io.ReadAll(response.Body)
						if !tc.works {
							t.Fatal("connection accepted an untrusted certificate")
						}
						if err != nil || string(body) != "pin-verified" {
							t.Fatalf("unexpected response: %q, %v", body, err)
						}
					})
				}
			}
		}
	}
}
