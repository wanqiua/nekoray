package main

import (
	"context"
	"grpc_server/testutil"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/xtls/xray-core/core"
)

func TestVLESSXudpTCPAndUDP(t *testing.T) {
	for _, variant := range []string{"tcp", "ws", "vision"} {
		t.Run(variant, func(t *testing.T) {
			certificate, key := selfSignedCompatibilityCertificate(t)
			port := testutil.FreePort(t)
			network, flow := "tcp", ""
			if variant == "ws" {
				network = "ws"
			} else if variant == "vision" {
				flow = "xtls-rprx-vision"
			}
			serverStream := map[string]any{"network": network, "security": "tls", "tlsSettings": map[string]any{
				"certificates": []any{map[string]any{
					"certificate": strings.Split(strings.TrimSpace(certificate), "\n"),
					"key":         strings.Split(strings.TrimSpace(key), "\n"),
				}},
			}}
			clientStream := map[string]any{"network": network, "security": "tls", "tlsSettings": map[string]any{
				"serverName": "localhost", "allowInsecure": true, "fingerprint": "safari",
			}}
			if network == "ws" {
				serverStream["wsSettings"] = map[string]any{"path": "/xudp"}
				clientStream["wsSettings"] = map[string]any{"path": "/xudp"}
			}
			startCompatibilityInstance(t, compatibilityConfig(t, []any{map[string]any{
				"listen": "127.0.0.1", "port": port, "protocol": "vless", "streamSettings": serverStream,
				"settings": map[string]any{"decryption": "none", "clients": []any{map[string]any{"id": compatibilityUUID, "flow": flow}}},
			}}, []any{map[string]any{"protocol": "freedom", "settings": map[string]any{}}}))
			client := startCompatibilityInstance(t, compatibilityConfig(t, nil, []any{map[string]any{
				"tag": "proxy", "protocol": "vless", "streamSettings": clientStream,
				"settings": map[string]any{"vnext": []any{map[string]any{
					"address": "127.0.0.1", "port": port,
					"users": []any{map[string]any{"id": compatibilityUUID, "encryption": "none", "flow": flow}},
				}}},
				"mux": map[string]any{"enabled": false, "concurrency": -1, "xudpConcurrency": 8, "xudpProxyUDP443": "allow"},
			}}))
			target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				io.WriteString(w, "vless-xudp-tcp")
			}))
			defer target.Close()
			httpClient := createProxyHttpClient(client)
			httpClient.Timeout = 4 * time.Second
			defer httpClient.CloseIdleConnections()
			response, err := httpClient.Get(target.URL)
			if err != nil {
				t.Fatal(err)
			}
			body, err := io.ReadAll(response.Body)
			response.Body.Close()
			if err != nil || string(body) != "vless-xudp-tcp" {
				t.Fatalf("TCP through XUDP-enabled VLESS: %q, %v", body, err)
			}
			echo, err := net.ListenPacket("udp4", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			defer echo.Close()
			go func() {
				buffer := make([]byte, 2048)
				for {
					count, address, err := echo.ReadFrom(buffer)
					if err != nil {
						return
					}
					echo.WriteTo(buffer[:count], address)
				}
			}()
			conn, err := core.DialUDP(context.Background(), client.Instance)
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			conn.SetDeadline(time.Now().Add(4 * time.Second))
			payload := []byte(strings.Repeat("xudp", 128))
			if _, err = conn.WriteTo(payload, echo.LocalAddr()); err != nil {
				t.Fatal(err)
			}
			buffer := make([]byte, 2048)
			count, _, err := conn.ReadFrom(buffer)
			if err != nil || string(buffer[:count]) != string(payload) {
				t.Fatalf("XUDP echo: bytes=%d, error=%v", count, err)
			}
		})
	}
}
