// Package testutil exercises the public GUI-to-core protocol over gRPC and
// forwards real TCP and UDP traffic through a loopback SOCKS listener.
package testutil

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"grpc_server/auth"
	"grpc_server/gen"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"testing"
	"time"

	"golang.org/x/net/proxy"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
)

func FreePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	l.Close()
	return port
}

func RunLifecycle(t *testing.T, implementation gen.LibcoreServiceServer, makeConfig func(int) string, connections bool) {
	t.Helper()
	authenticator := auth.Authenticator{Token: "local-test-token"}
	rpc := grpc.NewServer(grpc.UnaryInterceptor(authenticator.UnaryServerInterceptor))
	gen.RegisterLibcoreServiceServer(rpc, implementation)
	listener := bufconn.Listen(1024 * 1024)
	go rpc.Serve(listener)
	defer rpc.Stop()
	conn, err := grpc.NewClient("passthrough:///loopback", grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return listener.Dial() }))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	client := gen.NewLibcoreServiceClient(conn)
	runClientLifecycle(t, client, makeConfig, connections)
	runParallelURLTests(t, client, makeConfig)
}

func runClientLifecycle(t *testing.T, client gen.LibcoreServiceClient, makeConfig func(int) string, connections bool) {
	t.Helper()
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/unavailable" {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		time.Sleep(3 * time.Millisecond)
		io.WriteString(w, "nekoray-core-upgrade")
	}))
	defer target.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	_, err := client.QueryStats(ctx, &gen.QueryStatsReq{Tag: "proxy", Direct: "uplink"})
	if status.Code(err) != codes.Unauthenticated {
		t.Fatalf("unauthenticated request: %v", err)
	}
	if _, err = client.Capabilities(ctx, &gen.EmptyReq{}); status.Code(err) != codes.Unauthenticated {
		t.Fatalf("unauthenticated capabilities request: %v", err)
	}
	if _, err = client.Check(ctx, &gen.LoadConfigReq{CoreConfig: "{"}); status.Code(err) != codes.Unauthenticated {
		t.Fatalf("unauthenticated check request: %v", err)
	}
	ctx = metadata.NewOutgoingContext(ctx, metadata.Pairs("nekoray_auth", "local-test-token"))
	capabilities, err := client.Capabilities(ctx, &gen.EmptyReq{})
	if err != nil || capabilities.CoreFamily == "" || capabilities.CoreVersion == "" || capabilities.BuildId == "" || len(capabilities.Features) == 0 {
		t.Fatalf("capabilities: %v %v", capabilities, err)
	}
	if response, err := client.Check(ctx, &gen.LoadConfigReq{CoreConfig: "{"}); err != nil || response.Error == "" || response.FailureCategory != gen.FailureCategory_FailureConfig {
		t.Fatalf("invalid check should be a configuration failure: %v %v", response, err)
	}
	if response, err := client.Start(ctx, &gen.LoadConfigReq{CoreConfig: "{"}); err != nil || response.Error == "" {
		t.Fatalf("invalid configuration should fail: %v %v", response, err)
	}
	port := FreePort(t)
	config := &gen.LoadConfigReq{CoreConfig: makeConfig(port), StatsOutbounds: []string{"proxy"}, EnableNekorayConnections: true}
	check := func() {
		t.Helper()
		response, err := client.Check(ctx, config)
		if err != nil || response.Error != "" || response.FailureCategory != gen.FailureCategory_FailureNone || response.ConfigHash == "" {
			t.Fatalf("Check: %v %v", response, err)
		}
		if response.BuildId != capabilities.BuildId || response.CoreFamily != capabilities.CoreFamily || response.CoreVersion != capabilities.CoreVersion {
			t.Fatal("Check returned another core's identity")
		}
	}
	// An occupied desired port is valid for a preflight: ownership transfers at
	// Start, never during Check. This also catches implementations calling Start.
	reserved, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)))
	if err != nil {
		t.Fatal(err)
	}
	defer reserved.Close()
	check()
	reserved.Close()
	start := func() {
		t.Helper()
		response, err := client.Start(ctx, config)
		if err != nil || response.Error != "" {
			t.Fatalf("Start: %v %v", response, err)
		}
	}
	stop := func() {
		t.Helper()
		response, err := client.Stop(ctx, &gen.EmptyReq{})
		if err != nil || response.Error != "" {
			t.Errorf("Stop: %v %v", response, err)
		}
	}
	start()
	defer stop()
	address := net.JoinHostPort("127.0.0.1", strconv.Itoa(port))
	getThroughProxy := func() {
		t.Helper()
		dialer, err := proxy.SOCKS5("tcp", address, nil, &net.Dialer{Timeout: 3 * time.Second})
		if err != nil {
			t.Fatal(err)
		}
		transport := &http.Transport{DialContext: dialer.(proxy.ContextDialer).DialContext, DisableKeepAlives: true}
		defer transport.CloseIdleConnections()
		httpClient := &http.Client{Transport: transport, Timeout: 5 * time.Second}
		response, err := httpClient.Get(target.URL)
		if err != nil {
			t.Fatal("SOCKS forwarding: ", err)
		}
		defer response.Body.Close()
		body, err := io.ReadAll(response.Body)
		if err != nil || string(body) != "nekoray-core-upgrade" {
			t.Fatalf("forwarded response %q: %v", body, err)
		}
	}
	getThroughProxy()
	check() // Checking the same public port must leave the active listener alone.
	if response, err := client.Check(ctx, &gen.LoadConfigReq{CoreConfig: "{"}); err != nil || response.Error == "" {
		t.Fatalf("invalid preflight while connected: %v %v", response, err)
	}
	getThroughProxy()
	if duplicate, err := client.Start(ctx, config); err != nil || duplicate.Error == "" {
		t.Fatalf("duplicate Start should fail: %v %v", duplicate, err)
	}
	getThroughProxy() // A rejected second Start must not orphan the running core.
	TestUDP(t, address)
	testResponse, err := client.Test(ctx, &gen.TestReq{Mode: gen.TestMode_UrlTest, Url: target.URL, Timeout: 3000})
	if err != nil || testResponse.Error != "" || testResponse.Ms <= 0 {
		t.Fatalf("URL test: %v %v", testResponse, err)
	}
	if testResponse.FailureCategory != gen.FailureCategory_FailureNone || testResponse.BuildId != capabilities.BuildId || testResponse.CoreFamily != capabilities.CoreFamily {
		t.Fatal("URL test lacks its actual core identity or success category")
	}
	for _, testCase := range []struct {
		name     string
		request  *gen.TestReq
		category gen.FailureCategory
	}{
		{"invalid test URL", &gen.TestReq{Mode: gen.TestMode_UrlTest, Url: "invalid-url", Timeout: 3000}, gen.FailureCategory_FailureConfig},
		{"invalid core config", &gen.TestReq{Mode: gen.TestMode_UrlTest, Config: &gen.LoadConfigReq{CoreConfig: "{"}, Url: target.URL}, gen.FailureCategory_FailureConfig},
		{"remote HTTP failure", &gen.TestReq{Mode: gen.TestMode_UrlTest, Url: target.URL + "/unavailable", Timeout: 3000}, gen.FailureCategory_FailureRemote},
		{"unknown test mode", &gen.TestReq{Mode: gen.TestMode(1000)}, gen.FailureCategory_FailureConfig},
	} {
		response, err := client.Test(ctx, testCase.request)
		if err != nil || response.Error == "" || response.FailureCategory != testCase.category || response.BuildId != capabilities.BuildId {
			t.Fatalf("%s: %v %v", testCase.name, response, err)
		}
	}
	for _, direction := range []string{"uplink", "downlink"} {
		response, err := client.QueryStats(ctx, &gen.QueryStatsReq{Tag: "proxy", Direct: direction})
		if err != nil || response.Traffic <= 0 {
			t.Fatalf("%s traffic counter: %v %v", direction, response, err)
		}
		reset, err := client.QueryStats(ctx, &gen.QueryStatsReq{Tag: "proxy", Direct: direction})
		if err != nil || reset.Traffic != 0 {
			t.Fatalf("%s counter reset: %v %v", direction, reset, err)
		}
	}
	connectionResponse, err := client.ListConnections(ctx, &gen.EmptyReq{})
	if err != nil {
		t.Fatal(err)
	}
	var items []map[string]any
	if err := json.Unmarshal([]byte(connectionResponse.NekorayConnectionsJson), &items); err != nil {
		t.Fatal(err)
	}
	if connections && len(items) == 0 {
		t.Fatal("connection list should contain forwarded traffic")
	}
	stop()
	socket, err := net.DialTimeout("tcp", address, 200*time.Millisecond)
	if err == nil {
		socket.Close()
		t.Fatal("SOCKS listener still open after Stop")
	}
	start()
	getThroughProxy()
}

func TestUDP(t *testing.T, socksAddress string) {
	t.Helper()
	echo, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer echo.Close()
	go func() {
		buffer := make([]byte, 2048)
		n, peer, err := echo.ReadFromUDP(buffer)
		if err == nil {
			echo.WriteToUDP(buffer[:n], peer)
		}
	}()
	control, err := net.DialTimeout("tcp", socksAddress, 3*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer control.Close()
	control.SetDeadline(time.Now().Add(5 * time.Second))
	control.Write([]byte{5, 1, 0})
	greeting := make([]byte, 2)
	if _, err := io.ReadFull(control, greeting); err != nil || greeting[1] != 0 {
		t.Fatalf("SOCKS greeting %v: %v", greeting, err)
	}
	control.Write([]byte{5, 3, 0, 1, 0, 0, 0, 0, 0, 0})
	header := make([]byte, 4)
	if _, err := io.ReadFull(control, header); err != nil || header[1] != 0 {
		t.Fatalf("UDP ASSOCIATE %v: %v", header, err)
	}
	var host net.IP
	switch header[3] {
	case 1:
		host = make(net.IP, 4)
	case 4:
		host = make(net.IP, 16)
	default:
		t.Fatalf("unsupported relay address type %d", header[3])
	}
	if _, err := io.ReadFull(control, host); err != nil {
		t.Fatal(err)
	}
	portBytes := make([]byte, 2)
	if _, err := io.ReadFull(control, portBytes); err != nil {
		t.Fatal(err)
	}
	if host.IsUnspecified() {
		host = net.IPv4(127, 0, 0, 1)
	}
	udp, err := net.DialUDP("udp", nil, &net.UDPAddr{IP: host, Port: int(binary.BigEndian.Uint16(portBytes))})
	if err != nil {
		t.Fatal(err)
	}
	defer udp.Close()
	udp.SetDeadline(time.Now().Add(5 * time.Second))
	port := echo.LocalAddr().(*net.UDPAddr).Port
	packet := []byte{0, 0, 0, 1, 127, 0, 0, 1, byte(port >> 8), byte(port)}
	payload := []byte("udp-core-upgrade")
	if _, err := udp.Write(append(packet, payload...)); err != nil {
		t.Fatal(err)
	}
	buffer := make([]byte, 2048)
	n, err := udp.Read(buffer)
	if err != nil {
		t.Fatal("UDP forwarding: ", err)
	}
	offset := 10
	if n >= 4 && buffer[3] == 4 {
		offset = 22
	}
	if n < offset || string(buffer[offset:n]) != string(payload) {
		t.Fatal(fmt.Sprintf("unexpected UDP reply %x", buffer[:n]))
	}
}
