package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"grpc_server/gen"
	"grpc_server/testutil"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/xtls/xray-core/core"
)

func compatibilityConfig(t *testing.T, inbounds, outbounds []any) string {
	t.Helper()
	config := map[string]any{"log": map[string]any{"loglevel": "none"}, "outbounds": outbounds}
	if inbounds != nil {
		config["inbounds"] = inbounds
	}
	data, err := json.Marshal(config)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func startCompatibilityInstance(t *testing.T, config string) *NekoV2RayInstance {
	t.Helper()
	i, err := NewNekoV2rayInstance(config)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { i.Close() })
	if err = i.Start(); err != nil {
		t.Fatal(err)
	}
	return i
}

func legacySSClient(t *testing.T, password string, port int) string {
	t.Helper()
	return compatibilityConfig(t, nil, []any{map[string]any{
		"tag": "proxy", "protocol": "shadowsocks",
		"settings": map[string]any{"servers": []any{map[string]any{
			"address": "127.0.0.1", "port": port, "method": "rc4-md5", "password": password,
		}}},
	}})
}

func legacySSServer(t *testing.T) int {
	t.Helper()
	port := testutil.FreePort(t)
	startCompatibilityInstance(t, compatibilityConfig(t,
		[]any{map[string]any{"tag": "server", "listen": "127.0.0.1", "port": port,
			"protocol": "shadowsocks", "settings": map[string]any{
				"method": "rc4-md5", "password": "local-rc4-password", "network": "tcp,udp",
			}}},
		[]any{map[string]any{"tag": "direct", "protocol": "freedom", "settings": map[string]any{}}}))
	return port
}

func selfSignedCompatibilityCertificate(t *testing.T) (string, string) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "localhost"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		DNSNames: []string{"localhost"}, BasicConstraintsValid: true,
		KeyUsage:    x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	cert, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	keyData, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	return string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: cert})),
		string(pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyData}))
}

const compatibilityUUID = "75f2a015-3611-4c7f-bf7c-a948276b179f"

func legacyTLSServer(t *testing.T) int {
	t.Helper()
	certificate, key := selfSignedCompatibilityCertificate(t)
	port := testutil.FreePort(t)
	startCompatibilityInstance(t, compatibilityConfig(t,
		[]any{map[string]any{"tag": "server", "listen": "127.0.0.1", "port": port, "protocol": "vless",
			"settings": map[string]any{"clients": []any{map[string]any{"id": compatibilityUUID}}, "decryption": "none"},
			"streamSettings": map[string]any{"network": "ws", "wsSettings": map[string]any{"path": "/compatibility"},
				"security": "tls", "tlsSettings": map[string]any{"certificates": []any{map[string]any{
					"certificate": strings.Split(strings.TrimSpace(certificate), "\n"),
					"key":         strings.Split(strings.TrimSpace(key), "\n"),
				}}}}}},
		[]any{map[string]any{"tag": "direct", "protocol": "freedom", "settings": map[string]any{}}}))
	return port
}

func legacyTLSClient(t *testing.T, port int, insecure bool, fingerprint string) string {
	t.Helper()
	return compatibilityConfig(t, nil, []any{map[string]any{"tag": "proxy", "protocol": "vless",
		"settings": map[string]any{"vnext": []any{map[string]any{"address": "127.0.0.1", "port": port,
			"users": []any{map[string]any{"id": compatibilityUUID, "encryption": "none"}}}}},
		"streamSettings": map[string]any{"network": "ws", "wsSettings": map[string]any{"path": "/compatibility"},
			"security": "tls", "tlsSettings": map[string]any{"serverName": "localhost",
				"allowInsecure": insecure, "fingerprint": fingerprint, "alpn": []string{"http/1.1"}}},
	}})
}

func TestRC4MD5TCPAndUDP(t *testing.T) {
	port := legacySSServer(t)
	i := startCompatibilityInstance(t, legacySSClient(t, "local-rc4-password", port))
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "rc4-md5-tcp-passed")
	}))
	defer target.Close()
	client := createProxyHttpClient(i)
	client.Timeout = 3 * time.Second
	defer client.CloseIdleConnections()
	response, err := client.Get(target.URL)
	if err != nil {
		t.Fatal(err)
	}
	data, err := io.ReadAll(response.Body)
	response.Body.Close()
	if err != nil || string(data) != "rc4-md5-tcp-passed" {
		t.Fatalf("RC4-MD5 TCP: %q, %v", data, err)
	}
	echo, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer echo.Close()
	go func() {
		buffer := make([]byte, 1024)
		for {
			n, addr, err := echo.ReadFrom(buffer)
			if err != nil {
				return
			}
			echo.WriteTo(buffer[:n], addr)
		}
	}()
	conn, err := core.DialUDP(context.Background(), i.Instance)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err = conn.WriteTo([]byte("rc4-md5-udp-passed"), echo.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	buffer := make([]byte, 1024)
	n, _, err := conn.ReadFrom(buffer)
	if err != nil || string(buffer[:n]) != "rc4-md5-udp-passed" {
		t.Fatalf("RC4-MD5 UDP: %q, %v", buffer[:n], err)
	}
}

func TestLegacyNodesSerialAndBatchURL(t *testing.T) {
	ssPort, tlsPort := legacySSServer(t), legacyTLSServer(t)
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))
	defer target.Close()
	nodes := []struct {
		name, config string
		works        bool
	}{
		{"rc4-md5", legacySSClient(t, "local-rc4-password", ssPort), true},
		{"rc4-md5-wrong-password", legacySSClient(t, "wrong-password", ssPort), false},
		{"ws-tls-skip-verify", legacyTLSClient(t, tlsPort, true, ""), true},
		{"ws-tls-chrome-skip-verify", legacyTLSClient(t, tlsPort, true, "chrome"), true},
		{"ws-tls-verify-self-signed", legacyTLSClient(t, tlsPort, false, ""), false},
		{"ws-tls-chrome-verify-self-signed", legacyTLSClient(t, tlsPort, false, "chrome"), false},
	}
	probe := func(index int) error {
		node := nodes[index]
		response, err := (&server{}).Test(context.Background(), &gen.TestReq{Mode: gen.TestMode_UrlTest,
			Url: target.URL, Timeout: 1800, Config: &gen.LoadConfigReq{CoreConfig: node.config}})
		if err != nil {
			return err
		}
		if (response.Error == "" && response.Ms > 0) != node.works {
			return fmt.Errorf("%s: expected working=%t, got %v", node.name, node.works, response)
		}
		return nil
	}
	for index := range nodes {
		if err := probe(index); err != nil {
			t.Fatalf("serial: %v", err)
		}
	}
	errors := make(chan error, len(nodes))
	for index := range nodes {
		go func(index int) { errors <- probe(index) }(index)
	}
	for range nodes {
		if err := <-errors; err != nil {
			t.Errorf("batch: %v", err)
		}
	}
}
