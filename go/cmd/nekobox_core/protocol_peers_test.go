package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	M "github.com/sagernet/sing/common/metadata"
)

// These fixtures exercise the pinned sing-box client and server over loopback.
// They do not use the host proxy, a TUN adapter, a public endpoint, or user nodes.
// Passing them is native sing-box interoperability, not official-server parity.
const protocolPeerPassword = "local-protocol-peer-0123456789"

func protocolPeerOptions(t *testing.T, protocol string) (map[string]any, map[string]any) {
	t.Helper()
	var port int
	if protocol == "hysteria2" {
		listener, err := net.ListenPacket("udp4", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		port = listener.LocalAddr().(*net.UDPAddr).Port
		listener.Close()
	} else {
		listener, err := net.Listen("tcp4", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		port = listener.Addr().(*net.TCPAddr).Port
		listener.Close()
	}
	in := map[string]any{"type": protocol, "tag": "peer", "listen": "127.0.0.1", "listen_port": port}
	out := map[string]any{"type": protocol, "tag": "proxy", "server": "127.0.0.1", "server_port": port}
	if protocol == "snell" {
		in["version"], out["version"] = 5, 4
		in["psk"], out["psk"] = protocolPeerPassword, protocolPeerPassword
	} else {
		in["users"] = []any{map[string]any{"name": "peer", "password": protocolPeerPassword}}
		out["password"] = protocolPeerPassword
		certificate, key := loopbackCertificate(t)
		protocolPeerTLS(in, out, certificate, key)
	}
	return in, out
}

func protocolPeerTLS(in, out map[string]any, certificate, key string) {
	in["tls"] = map[string]any{"enabled": true, "certificate": []string{certificate}, "key": []string{key}}
	out["tls"] = map[string]any{"enabled": true, "server_name": "localhost", "certificate": []string{certificate}}
}

func protocolPeerCore(options map[string]any, inbound bool) (*BoxInstance, error) {
	config := map[string]any{"log": map[string]any{"level": "error"}}
	if inbound {
		config["inbounds"] = []any{options}
		config["outbounds"] = []any{map[string]any{"type": "direct", "tag": "direct"}}
	} else {
		config["outbounds"] = []any{options}
	}
	content, err := json.Marshal(config)
	if err != nil {
		return nil, err
	}
	return NewBoxInstance(content, nil)
}

func startProtocolPeerCore(t *testing.T, options map[string]any, inbound bool) *BoxInstance {
	t.Helper()
	core, err := protocolPeerCore(options, inbound)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := core.Close(); err != nil {
			t.Errorf("close protocol peer: %v", err)
		}
	})
	return core
}

func protocolPeerHTTPClient(t *testing.T, core *BoxInstance) *http.Client {
	t.Helper()
	// Disable HTTP connection reuse so repeated requests open distinct protocol
	// streams, allowing the carrier observer to verify protocol-level reuse.
	transport := &http.Transport{DialContext: core.DialContext, DisableKeepAlives: true}
	t.Cleanup(transport.CloseIdleConnections)
	return &http.Client{Transport: &startupRetryTransport{base: transport, instance: core}, Timeout: 4 * time.Second}
}

func verifyProtocolPeerHTTP(t *testing.T, core *BoxInstance, requests int) {
	t.Helper()
	payload := []byte(strings.Repeat("peer-body-"+t.Name()+"\n", 64))
	var hits atomic.Int64
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "payload mismatch", http.StatusBadRequest)
			return
		}
		hits.Add(1)
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Write(append([]byte("received:"), payload...))
	}))
	defer target.Close()
	client := protocolPeerHTTPClient(t, core)
	for index := 0; index < requests; index++ {
		response, err := client.Get(target.URL)
		if err != nil {
			t.Fatal(err)
		}
		body, readErr := io.ReadAll(response.Body)
		response.Body.Close()
		if readErr != nil || response.StatusCode != http.StatusOK || !bytes.Equal(body, append([]byte("received:"), payload...)) {
			t.Fatalf("TCP request %d: status=%d bytes=%d read=%v", index, response.StatusCode, len(body), readErr)
		}
	}
	if got := hits.Load(); got < int64(requests) {
		t.Fatalf("actual TCP target requests=%d, want at least %d", got, requests)
	}
}

func verifyProtocolPeerHTTPFailure(t *testing.T, core *BoxInstance, errorContains string) {
	t.Helper()
	var hits atomic.Int64
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		io.WriteString(w, "must-not-be-reached")
	}))
	defer target.Close()
	client := protocolPeerHTTPClient(t, core)
	client.Timeout = 2 * time.Second
	response, err := client.Get(target.URL)
	if response != nil {
		response.Body.Close()
	}
	if err == nil {
		t.Fatal("invalid credentials/options unexpectedly reached the HTTP target")
	}
	if errorContains != "" && !strings.Contains(strings.ToLower(err.Error()), strings.ToLower(errorContains)) {
		t.Fatalf("wrong failure: %v; want %q", err, errorContains)
	}
	if hits.Load() != 0 {
		t.Fatalf("invalid peer contacted the HTTP target %d times", hits.Load())
	}
}

func verifyProtocolPeerUDP(t *testing.T, core *BoxInstance, extraSizes ...int) {
	t.Helper()
	echo, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		buffer := make([]byte, 65536)
		for {
			count, source, err := echo.ReadFrom(buffer)
			if err != nil {
				return
			}
			if _, err := echo.WriteTo(buffer[:count], source); err != nil {
				return
			}
		}
	}()
	defer func() { echo.Close(); <-done }()
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()
	// UoT v2 encodes an initial destination even for unconnected datagrams.
	// Match the normal sing-box router, which supplies the real destination;
	// the legacy BoxInstance.DialUDP hook passes a zero address instead.
	conn, err := core.Outbound().Default().ListenPacket(ctx, M.SocksaddrFromNet(echo.LocalAddr()))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	for _, size := range append([]int{1, 64, 1200}, extraSizes...) {
		payload := make([]byte, size)
		if _, err := rand.Read(payload); err != nil {
			t.Fatal(err)
		}
		if _, err := conn.WriteTo(payload, echo.LocalAddr()); err != nil {
			t.Fatalf("UDP write %d bytes: %v", size, err)
		}
		if err := conn.SetReadDeadline(time.Now().Add(4 * time.Second)); err != nil {
			t.Fatal(err)
		}
		buffer := make([]byte, 65536)
		count, source, err := conn.ReadFrom(buffer)
		if err != nil || !bytes.Equal(buffer[:count], payload) {
			t.Fatalf("UDP echo %d bytes: received=%d source=%v error=%v", size, count, source, err)
		}
		if source.String() != echo.LocalAddr().String() {
			t.Fatalf("UDP response source=%s, want %s", source, echo.LocalAddr())
		}
	}
}

// tcpProtocolPeerRelay observes only test traffic, counting the number of
// transport connections while keeping each protocol's real server handshake.
type tcpProtocolPeerRelay struct {
	port        int
	connections atomic.Int64
}

func newTCPProtocolPeerRelay(t *testing.T, destination int) *tcpProtocolPeerRelay {
	t.Helper()
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	relay := &tcpProtocolPeerRelay{port: listener.Addr().(*net.TCPAddr).Port}
	var mu sync.Mutex
	var connections []net.Conn
	var workers sync.WaitGroup
	acceptDone := make(chan struct{})
	go func() {
		defer close(acceptDone)
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			relay.connections.Add(1)
			mu.Lock()
			connections = append(connections, conn)
			mu.Unlock()
			workers.Add(1)
			go func() {
				defer workers.Done()
				defer conn.Close()
				upstream, err := net.DialTimeout("tcp4", net.JoinHostPort("127.0.0.1", strconv.Itoa(destination)), time.Second)
				if err != nil {
					return
				}
				defer upstream.Close()
				done := make(chan struct{})
				go func() { io.Copy(upstream, conn); upstream.Close(); close(done) }()
				io.Copy(conn, upstream)
				conn.Close()
				<-done
			}()
		}
	}()
	t.Cleanup(func() {
		listener.Close()
		<-acceptDone
		mu.Lock()
		for _, conn := range connections {
			conn.Close()
		}
		mu.Unlock()
		workers.Wait()
	})
	return relay
}

func verifyProtocolPeerReuse(t *testing.T, core *BoxInstance, relay *tcpProtocolPeerRelay, reuse bool) {
	t.Helper()
	payload := []byte(strings.Repeat("reuse-payload-"+t.Name(), 128))
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(io.LimitReader(r.Body, 256*1024))
		if err != nil || !bytes.Equal(body, payload) {
			http.Error(w, "request payload mismatch", http.StatusBadRequest)
			return
		}
		w.Write(payload)
	}))
	defer target.Close()
	// The preceding normal HTTP test handles the initial interface callback.
	// Consume protocol EOF here: HTTP Body.Close alone can race the Snell
	// asynchronous drain and is not evidence that a stream is reusable yet.
	before := relay.connections.Load()
	for index := 0; index < 3; index++ {
		ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
		conn, err := core.DialContext(ctx, "tcp", target.Listener.Addr().String())
		if err != nil {
			cancel()
			t.Fatal(err)
		}
		conn.SetDeadline(time.Now().Add(4 * time.Second))
		request, err := http.NewRequest(http.MethodPost, target.URL, bytes.NewReader(payload))
		if err == nil {
			request.Close = true
			err = request.Write(conn)
		}
		var wire []byte
		if err == nil {
			wire, err = io.ReadAll(io.LimitReader(conn, 512*1024))
		}
		closeErr := conn.Close()
		cancel()
		// AnyTLS signals a peer's normal stream close as net.ErrClosed;
		// the complete HTTP response below still has to parse and match.
		if (err != nil && !errors.Is(err, net.ErrClosed)) || (closeErr != nil && !errors.Is(closeErr, net.ErrClosed)) {
			t.Fatalf("read reusable stream through EOF: read=%v close=%v", err, closeErr)
		}
		response, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(wire)), request)
		if err != nil {
			t.Fatal(err)
		}
		body, err := io.ReadAll(response.Body)
		response.Body.Close()
		if err != nil || response.StatusCode != http.StatusOK || !bytes.Equal(body, payload) {
			t.Fatalf("reuse HTTP payload: status=%d length=%d error=%v", response.StatusCode, len(body), err)
		}
	}
	opened := relay.connections.Load() - before
	if (reuse && opened > 1) || (!reuse && opened != 3) {
		t.Fatalf("reuse=%t: three sequential protocol streams opened %d new carriers", reuse, opened)
	}
}

func TestAnyTLSProtocolPeers(t *testing.T) {
	for _, advanced := range []bool{false, true} {
		t.Run(fmt.Sprintf("advanced=%t", advanced), func(t *testing.T) {
			in, out := protocolPeerOptions(t, "anytls")
			if advanced {
				out["idle_session_check_interval"] = "10s"
				out["idle_session_timeout"] = "15s"
				out["min_idle_session"] = 1
				out["client_metadata"] = "nekoray-peer-test"
			}
			startProtocolPeerCore(t, in, true)
			relay := newTCPProtocolPeerRelay(t, in["listen_port"].(int))
			out["server_port"] = relay.port
			client := startProtocolPeerCore(t, out, false)
			verifyProtocolPeerHTTP(t, client, 1)
			verifyProtocolPeerReuse(t, client, relay, true)
			verifyProtocolPeerUDP(t, client, 8192)
		})
	}
	for _, failure := range []string{"password", "certificate", "server_name"} {
		t.Run("reject_"+failure, func(t *testing.T) {
			in, out := protocolPeerOptions(t, "anytls")
			startProtocolPeerCore(t, in, true)
			verifyProtocolPeerHTTP(t, startProtocolPeerCore(t, out, false), 1)
			expected := ""
			switch failure {
			case "password":
				out["password"] = "incorrect-password"
			case "certificate":
				delete(out["tls"].(map[string]any), "certificate")
				expected = "certificate"
			case "server_name":
				out["tls"].(map[string]any)["server_name"] = "invalid-peer.example.test"
				expected = "certificate"
			}
			verifyProtocolPeerHTTPFailure(t, startProtocolPeerCore(t, out, false), expected)
		})
	}
	for _, option := range []string{"tls", "tcp_fast_open"} {
		t.Run("invalid_"+option, func(t *testing.T) {
			_, out := protocolPeerOptions(t, "anytls")
			if option == "tls" {
				delete(out, "tls")
			} else {
				out[option] = true
			}
			core, err := protocolPeerCore(out, false)
			if core != nil {
				core.Close()
			}
			if err == nil || !strings.Contains(strings.ToLower(err.Error()), option) {
				t.Fatalf("invalid AnyTLS %s: %v", option, err)
			}
		})
	}
}

func TestSnellProtocolPeers(t *testing.T) {
	for _, variant := range []struct {
		name    string
		version int
		mode    string
	}{
		{"v4_none", 4, "none"}, {"v4_http", 4, "http"}, {"v4_tls", 4, "tls"},
		{"v6_default", 6, "default"}, {"v6_unshaped", 6, "unshaped"}, {"v6_unsafe_raw", 6, "unsafe-raw"},
	} {
		for _, reuse := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/reuse=%t", variant.name, reuse), func(t *testing.T) {
				in, out := protocolPeerOptions(t, "snell")
				out["reuse"] = reuse
				if variant.version == 4 {
					in["obfs_mode"], out["obfs_mode"] = variant.mode, variant.mode
					out["obfs_host"] = "peer.example.test"
				} else {
					in["version"], out["version"] = 6, 6
					in["mode"], out["mode"] = variant.mode, variant.mode
				}
				startProtocolPeerCore(t, in, true)
				relay := newTCPProtocolPeerRelay(t, in["listen_port"].(int))
				out["server_port"] = relay.port
				client := startProtocolPeerCore(t, out, false)
				verifyProtocolPeerHTTP(t, client, 1)
				verifyProtocolPeerReuse(t, client, relay, reuse)
				var extraSizes []int
				if variant.version == 6 {
					extraSizes = []int{8192}
				}
				verifyProtocolPeerUDP(t, client, extraSizes...)
			})
		}
	}
	for _, version := range []int{4, 6} {
		t.Run(fmt.Sprintf("v%d_multiuser", version), func(t *testing.T) {
			in, out := protocolPeerOptions(t, "snell")
			if version == 6 {
				in["version"], out["version"] = 6, 6
			}
			in["users"] = []any{map[string]any{"name": "peer", "userkey": "local-user-key"}}
			out["userkey"] = "local-user-key"
			out["reuse"] = true
			startProtocolPeerCore(t, in, true)
			client := startProtocolPeerCore(t, out, false)
			verifyProtocolPeerHTTP(t, client, 2)
			verifyProtocolPeerUDP(t, client)
			out["userkey"] = "invalid-user-key"
			verifyProtocolPeerHTTPFailure(t, startProtocolPeerCore(t, out, false), "")
			out["userkey"], out["psk"] = "local-user-key", "incorrect-psk-0123456789"
			verifyProtocolPeerHTTPFailure(t, startProtocolPeerCore(t, out, false), "")
		})
	}
	for _, size := range []int{12, 255} {
		t.Run(fmt.Sprintf("v6_psk_%d_bytes", size), func(t *testing.T) {
			in, out := protocolPeerOptions(t, "snell")
			in["version"], out["version"] = 6, 6
			in["psk"], out["psk"] = strings.Repeat("x", size), strings.Repeat("x", size)
			startProtocolPeerCore(t, in, true)
			verifyProtocolPeerHTTP(t, startProtocolPeerCore(t, out, false), 1)
		})
	}
}

func TestHysteria2ProtocolPeers(t *testing.T) {
	for _, variant := range []struct {
		name          string
		profile       string
		obfs          string
		customSizes   bool
		disableChrome bool
	}{
		{name: "defaults"},
		{name: "standard", profile: "standard"},
		{name: "conservative", profile: "conservative"},
		{name: "aggressive", profile: "aggressive"},
		{name: "salamander", obfs: "salamander"},
		{name: "gecko_defaults", obfs: "gecko"},
		{name: "gecko_custom", profile: "aggressive", obfs: "gecko", customSizes: true},
		{name: "chrome_disabled", disableChrome: true},
	} {
		t.Run(variant.name, func(t *testing.T) {
			in, out := protocolPeerOptions(t, "hysteria2")
			if variant.profile != "" {
				in["bbr_profile"], out["bbr_profile"] = variant.profile, variant.profile
			}
			if variant.obfs != "" {
				obfs := map[string]any{"type": variant.obfs, "password": "peer-obfs-password"}
				if variant.customSizes {
					obfs["min_packet_size"], obfs["max_packet_size"] = 600, 1300
				}
				in["obfs"], out["obfs"] = obfs, obfs
			}
			out["disable_chrome_parrot"] = variant.disableChrome
			startProtocolPeerCore(t, in, true)
			client := startProtocolPeerCore(t, out, false)
			verifyProtocolPeerHTTP(t, client, 3)
			verifyProtocolPeerUDP(t, client, 4096)
		})
	}
	for _, failure := range []string{"password", "obfs_password", "server_name"} {
		t.Run("reject_"+failure, func(t *testing.T) {
			in, out := protocolPeerOptions(t, "hysteria2")
			if failure == "obfs_password" {
				in["obfs"] = map[string]any{"type": "gecko", "password": "peer-obfs-password"}
				out["obfs"] = map[string]any{"type": "gecko", "password": "peer-obfs-password"}
			}
			startProtocolPeerCore(t, in, true)
			verifyProtocolPeerHTTP(t, startProtocolPeerCore(t, out, false), 1)
			expected := ""
			switch failure {
			case "password":
				out["password"] = "incorrect-password"
				expected = "authentication"
			case "obfs_password":
				out["obfs"].(map[string]any)["password"] = "incorrect-obfs-password"
			case "server_name":
				out["tls"].(map[string]any)["server_name"] = "invalid-peer.example.test"
				expected = "certificate"
			}
			verifyProtocolPeerHTTPFailure(t, startProtocolPeerCore(t, out, false), expected)
		})
	}
}

func ed25519ProtocolPeerCertificate(t *testing.T) (string, string) {
	t.Helper()
	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(2), Subject: pkix.Name{CommonName: "localhost"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		DNSNames: []string{"localhost"}, IsCA: true, BasicConstraintsValid: true,
		KeyUsage:    x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	certificate, err := x509.CreateCertificate(rand.Reader, template, template, publicKey, privateKey)
	if err != nil {
		t.Fatal(err)
	}
	key, err := x509.MarshalPKCS8PrivateKey(privateKey)
	if err != nil {
		t.Fatal(err)
	}
	return string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certificate})), string(pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: key}))
}

func TestHysteria2Ed25519ChromeCompatibility(t *testing.T) {
	in, out := protocolPeerOptions(t, "hysteria2")
	certificate, key := ed25519ProtocolPeerCertificate(t)
	protocolPeerTLS(in, out, certificate, key)
	startProtocolPeerCore(t, in, true)
	out["disable_chrome_parrot"] = true
	client := startProtocolPeerCore(t, out, false)
	verifyProtocolPeerHTTP(t, client, 1)
	verifyProtocolPeerUDP(t, client)
	out["disable_chrome_parrot"] = false
	verifyProtocolPeerHTTPFailure(t, startProtocolPeerCore(t, out, false), "")
}

func TestProtocolPeerOptionConstraints(t *testing.T) {
	for _, test := range []struct {
		name     string
		protocol string
		options  map[string]any
		message  string
	}{
		{"snell_missing_psk", "snell", map[string]any{"psk": ""}, "pre-shared key"},
		{"snell_v5_client", "snell", map[string]any{"version": 5}, "unsupported version"},
		{"snell_unknown_obfs", "snell", map[string]any{"obfs_mode": "invalid"}, "obfs mode"},
		{"snell_v6_unknown_mode", "snell", map[string]any{"version": 6, "mode": "invalid"}, "v6 mode"},
		{"snell_v6_obfs_conflict", "snell", map[string]any{"version": 6, "obfs_mode": "http"}, "obfs_mode"},
		{"snell_userkey_256_bytes", "snell", map[string]any{"userkey": strings.Repeat("x", 256)}, "user key too long"},
		{"hy2_unknown_bbr_profile", "hysteria2", map[string]any{"bbr_profile": "invalid"}, "profile"},
		{"hy2_missing_obfs_password", "hysteria2", map[string]any{"obfs": map[string]any{"type": "gecko"}}, "obfs password"},
		{"hy2_unknown_obfs", "hysteria2", map[string]any{"obfs": map[string]any{"type": "invalid", "password": "test"}}, "obfs type"},
		{"hy2_gecko_negative_size", "hysteria2", map[string]any{"obfs": map[string]any{"type": "gecko", "password": "test", "min_packet_size": -1}}, "packet size"},
		{"hy2_gecko_min_above_max", "hysteria2", map[string]any{"obfs": map[string]any{"type": "gecko", "password": "test", "min_packet_size": 1000, "max_packet_size": 900}}, "packet size"},
		{"hy2_gecko_max_2049", "hysteria2", map[string]any{"obfs": map[string]any{"type": "gecko", "password": "test", "max_packet_size": 2049}}, "packet size"},
	} {
		t.Run(test.name, func(t *testing.T) {
			_, out := protocolPeerOptions(t, test.protocol)
			for key, value := range test.options {
				out[key] = value
			}
			core, err := protocolPeerCore(out, false)
			if core != nil {
				core.Close()
			}
			if err == nil || !strings.Contains(strings.ToLower(err.Error()), strings.ToLower(test.message)) {
				t.Fatalf("invalid option error=%v, want %q", err, test.message)
			}
		})
	}
	for _, size := range []int{11, 256} {
		t.Run(fmt.Sprintf("snell_v6_server_rejects_psk_%d_bytes", size), func(t *testing.T) {
			in, _ := protocolPeerOptions(t, "snell")
			in["version"], in["psk"] = 6, strings.Repeat("x", size)
			core, err := protocolPeerCore(in, true)
			if core != nil {
				core.Close()
			}
			if err == nil || !strings.Contains(strings.ToLower(err.Error()), "psk") {
				t.Fatalf("invalid Snell v6 server PSK: %v", err)
			}
		})
	}
}

func TestHysteria2HopOptionConstraintsAtDial(t *testing.T) {
	for _, test := range []struct {
		name, minimum, maximum, message string
	}{
		{"below_5_seconds", "4s", "6s", "at least 5 seconds"},
		{"missing_minimum", "", "6s", "must both be set"},
		{"minimum_above_maximum", "7s", "6s", "greater than max"},
	} {
		t.Run(test.name, func(t *testing.T) {
			in, out := protocolPeerOptions(t, "hysteria2")
			startProtocolPeerCore(t, in, true)
			verifyProtocolPeerHTTP(t, startProtocolPeerCore(t, out, false), 1)
			port := strconv.Itoa(in["listen_port"].(int))
			out["server_ports"] = []string{port + ":" + port}
			if test.minimum != "" {
				out["hop_interval"] = test.minimum
			}
			out["hop_interval_max"] = test.maximum
			// Upstream checks these constraints only in NewHopConn on first
			// dial. A successful config parse/start does not validate them.
			verifyProtocolPeerHTTPFailure(t, startProtocolPeerCore(t, out, false), test.message)
		})
	}
}

type udpProtocolPeerObservation struct {
	port   int
	source string
	at     time.Time
}

type udpProtocolPeerRelay struct {
	ports        []string
	mu           sync.Mutex
	observations []udpProtocolPeerObservation
}

func (r *udpProtocolPeerRelay) snapshot() []udpProtocolPeerObservation {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]udpProtocolPeerObservation(nil), r.observations...)
}

// Multiple loopback ports feed one real QUIC server, like a local port mapping.
// The server sees a stable backend socket; this tests client port hopping and
// continued forwarding, not public NAT behavior or server-side QUIC migration.
func newUDPProtocolPeerRelay(t *testing.T, destination, portCount int) *udpProtocolPeerRelay {
	t.Helper()
	upstream, err := net.DialUDP("udp4", nil, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: destination})
	if err != nil {
		t.Fatal(err)
	}
	var frontends []*net.UDPConn
	var workers sync.WaitGroup
	t.Cleanup(func() {
		for _, conn := range frontends {
			conn.Close()
		}
		upstream.Close()
		workers.Wait()
	})
	relay := &udpProtocolPeerRelay{}
	type replyPath struct {
		conn *net.UDPConn
		addr *net.UDPAddr
	}
	var reply replyPath
	seen := make(map[string]bool)
	for index := 0; index < portCount; index++ {
		frontend, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
		if err != nil {
			t.Fatal(err)
		}
		frontends = append(frontends, frontend)
		port := frontend.LocalAddr().(*net.UDPAddr).Port
		// This dependency expects ranges, including for singleton ports.
		relay.ports = append(relay.ports, fmt.Sprintf("%d:%d", port, port))
		workers.Add(1)
		go func() {
			defer workers.Done()
			buffer := make([]byte, 65536)
			for {
				count, source, err := frontend.ReadFromUDP(buffer)
				if err != nil {
					return
				}
				relay.mu.Lock()
				reply = replyPath{frontend, source}
				identity := strconv.Itoa(port) + "/" + source.String()
				if !seen[identity] {
					seen[identity] = true
					relay.observations = append(relay.observations, udpProtocolPeerObservation{port, source.String(), time.Now()})
				}
				relay.mu.Unlock()
				if _, err := upstream.Write(buffer[:count]); err != nil {
					return
				}
			}
		}()
	}
	workers.Add(1)
	go func() {
		defer workers.Done()
		buffer := make([]byte, 65536)
		for {
			count, err := upstream.Read(buffer)
			if err != nil {
				return
			}
			relay.mu.Lock()
			path := reply
			relay.mu.Unlock()
			if path.conn != nil {
				if _, err := path.conn.WriteToUDP(buffer[:count], path.addr); err != nil {
					return
				}
			}
		}
	}()
	return relay
}

func TestHysteria2PortHoppingWithIntervalRange(t *testing.T) {
	in, out := protocolPeerOptions(t, "hysteria2")
	startProtocolPeerCore(t, in, true)
	// Many ephemeral ports make the randomized selector very unlikely to
	// choose the same port throughout the bounded observation window.
	relay := newUDPProtocolPeerRelay(t, in["listen_port"].(int), 64)
	delete(out, "server_port")
	out["server_ports"] = relay.ports
	out["hop_interval"], out["hop_interval_max"] = "5s", "6s"
	clientCore := startProtocolPeerCore(t, out, false)
	verifyProtocolPeerHTTP(t, clientCore, 1)
	verifyProtocolPeerUDP(t, clientCore)
	baseline := relay.snapshot()
	if len(baseline) == 0 {
		t.Fatal("QUIC peer bypassed the observed UDP ports")
	}
	last := baseline[len(baseline)-1]
	previous := last
	processed := len(baseline)
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "hopping-peer")
	}))
	defer target.Close()
	client := protocolPeerHTTPClient(t, clientCore)
	start := time.Now()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for time.Since(start) < 26*time.Second {
		<-ticker.C
		response, err := client.Get(target.URL)
		if err != nil {
			t.Fatalf("forwarding during port hop: %v", err)
		}
		body, err := io.ReadAll(response.Body)
		response.Body.Close()
		if err != nil || string(body) != "hopping-peer" {
			t.Fatalf("port-hop response=%q error=%v", body, err)
		}
		observations := relay.snapshot()
		for _, observation := range observations[processed:] {
			interval := observation.at.Sub(previous.at)
			previous = observation
			processed++
			// The first interface notification can create a replacement
			// socket during startup. It is not evidence of timed hopping.
			if observation.at.Sub(clientCore.startedAt) < 2*time.Second {
				last = observation
				continue
			}
			// Permit one second for Windows scheduling and the observer's
			// polling delay. This checks the configured interval's runtime
			// envelope, not the statistical distribution of random draws.
			if interval < 4*time.Second || interval > 7*time.Second {
				t.Fatalf("observed hop interval=%s, expected 5s..6s with 1s observer tolerance", interval)
			}
			if observation.port != last.port && observation.source != last.source {
				t.Logf("verified UDP destination port %d -> %d and source socket change after %s", last.port, observation.port, observation.at.Sub(start).Round(time.Millisecond))
				verifyProtocolPeerUDP(t, clientCore)
				return
			}
		}
	}
	t.Fatalf("no post-startup destination/source change after 26s: observations=%v", relay.snapshot())
}
