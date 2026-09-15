package testutil

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"grpc_server/gen"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

type batchCoreIdentity struct {
	Family         string `json:"family"`
	Version        string `json:"version"`
	BuildID        string `json:"build_id"`
	SourceRevision string `json:"source_revision"`
	StartupMS      int64  `json:"startup_ms"`
}

type batchProbeResult struct {
	ID               string `json:"id"`
	Core             string `json:"core"`
	Kind             string `json:"kind"`
	Expected         string `json:"expected"`
	Status           string `json:"status"`
	FailureCategory  string `json:"failure_category,omitempty"`
	RPCStatus        string `json:"rpc_status,omitempty"`
	Started          bool   `json:"started"`
	IdentityVerified bool   `json:"identity_verified"`
	LatencyMS        int32  `json:"latency_ms"`
	ElapsedMS        int64  `json:"elapsed_ms"`
	ProxyHits        int32  `json:"proxy_hits"`
}

type batchPhaseReport struct {
	Name             string             `json:"name"`
	Nodes            int                `json:"nodes"`
	Budget           int                `json:"budget"`
	Dispatched       int                `json:"dispatched"`
	Successes        int                `json:"successes"`
	Cancelled        int                `json:"cancelled"`
	ElapsedMS        int64              `json:"elapsed_ms"`
	RequestsPerSec   float64            `json:"requests_per_second"`
	ClientPeak       int                `json:"client_peak"`
	ServerPeak       int                `json:"server_peak"`
	ServerPeakByCore map[string]int     `json:"server_peak_by_core"`
	WrongProxyRoutes int                `json:"wrong_proxy_routes"`
	DrainMS          int64              `json:"drain_ms"`
	Drained          bool               `json:"drained"`
	Results          []batchProbeResult `json:"results"`
}

type batchAcceptanceReport struct {
	SchemaVersion     int                 `json:"schema_version"`
	Scope             string              `json:"scope"`
	StartedAtUTC      string              `json:"started_at_utc"`
	CompletedAtUTC    string              `json:"completed_at_utc"`
	Platform          string              `json:"platform"`
	GoVersion         string              `json:"go_version"`
	CPUCount          int                 `json:"cpu_count"`
	NodeCount         int                 `json:"node_count"`
	Concurrency       int                 `json:"concurrency"`
	DelayMS           int                 `json:"delay_ms"`
	Cores             []batchCoreIdentity `json:"cores"`
	Phases            []batchPhaseReport  `json:"phases"`
	ComparedResults   int                 `json:"compared_results"`
	MatchedResults    int                 `json:"matched_results"`
	ThroughputSpeedup float64             `json:"throughput_speedup"`
	MinimumSpeedup    float64             `json:"minimum_speedup"`
	CancelToReturnMS  int64               `json:"cancel_to_return_ms"`
	Status            string              `json:"status"`
	Issues            []string            `json:"issues"`
}

type batchCore struct {
	identity batchCoreIdentity
	address  string
	token    string
}

func startBatchCore(t *testing.T, directory, mode, family, version string) *batchCore {
	t.Helper()
	extension := ""
	if runtime.GOOS == "windows" {
		extension = ".exe"
	}
	program := filepath.Join(directory, mode+"_core"+extension)
	binary, err := os.Open(program)
	if err != nil {
		t.Fatal(err)
	}
	hash := sha256.New()
	_, err = io.Copy(hash, binary)
	binary.Close()
	if err != nil {
		t.Fatal(err)
	}
	token := make([]byte, 24)
	if _, err = rand.Read(token); err != nil {
		t.Fatal(err)
	}
	port := FreePort(t)
	core := &batchCore{address: fmt.Sprintf("127.0.0.1:%d", port), token: hex.EncodeToString(token)}
	logPath := filepath.Join(t.TempDir(), mode+".log")
	logFile, err := os.Create(logPath)
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(program, mode, "-port", strconv.Itoa(port), "-parent-pid", strconv.Itoa(os.Getpid()))
	command.Dir = t.TempDir() // No installed config, hook, or user node is consulted.
	command.Stdin = strings.NewReader(core.token + "\n")
	command.Stdout, command.Stderr = logFile, logFile
	started := time.Now()
	if err = command.Start(); err != nil {
		logFile.Close()
		t.Fatal(err)
	}
	done := make(chan struct{})
	var processError error
	go func() { processError = command.Wait(); close(done) }()
	connection, err := grpc.NewClient(core.address, grpc.WithTransportCredentials(insecure.NewCredentials()))
	t.Cleanup(func() {
		if connection != nil {
			ctx, cancel := context.WithTimeout(core.authenticate(context.Background()), 3*time.Second)
			_, _ = gen.NewLibcoreServiceClient(connection).Exit(ctx, &gen.EmptyReq{})
			cancel()
			connection.Close()
		}
		select {
		case <-done:
			if processError != nil {
				t.Errorf("%s did not exit cleanly: %v", family, processError)
			}
		case <-time.After(5 * time.Second):
			_ = command.Process.Kill() // Only the process created by this test.
			<-done
			t.Errorf("%s did not acknowledge graceful exit", family)
		}
		logFile.Close()
		if t.Failed() {
			contents, _ := os.ReadFile(logPath)
			t.Logf("%s synthetic fixture log:\n%s", family, contents)
		}
	})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(core.authenticate(context.Background()), 8*time.Second)
	defer cancel()
	capabilities, err := gen.NewLibcoreServiceClient(connection).Capabilities(ctx, &gen.EmptyReq{}, grpc.WaitForReady(true))
	if err != nil {
		t.Fatal(err)
	}
	identity := "sha256:" + hex.EncodeToString(hash.Sum(nil))
	if capabilities.CoreFamily != family || capabilities.CoreVersion != version || capabilities.BuildId != identity {
		t.Fatalf("unexpected %s executable identity/version: %s %s %s", family, capabilities.CoreFamily, capabilities.CoreVersion, capabilities.BuildId)
	}
	core.identity = batchCoreIdentity{family, version, identity, capabilities.SourceRevision, time.Since(started).Milliseconds()}
	return core
}

func (c *batchCore) authenticate(ctx context.Context) context.Context {
	return metadata.AppendToOutgoingContext(ctx, "nekoray_auth", c.token)
}

type batchMeter struct {
	mu          sync.Mutex
	active      int
	peak        int
	wrong       int
	activeCores map[string]int
	peakCores   map[string]int
	changed     chan struct{}
}

func newBatchMeter() *batchMeter {
	return &batchMeter{activeCores: map[string]int{}, peakCores: map[string]int{}, changed: make(chan struct{}, 1)}
}

func (m *batchMeter) update(core string, delta int) {
	m.mu.Lock()
	m.active += delta
	m.peak = max(m.peak, m.active)
	m.activeCores[core] += delta
	m.peakCores[core] = max(m.peakCores[core], m.activeCores[core])
	m.mu.Unlock()
	select {
	case m.changed <- struct{}{}:
	default:
	}
}

func (m *batchMeter) waitActive(want int, timeout time.Duration) bool {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	for {
		m.mu.Lock()
		active := m.active
		m.mu.Unlock()
		if active == want {
			return true
		}
		select {
		case <-m.changed:
		case <-timer.C:
			return false
		}
	}
}

type batchNode struct {
	id, kind, expected, config, url string
	core                            int
	timeout                         int32
	hits                            atomic.Int32
	meter                           atomic.Pointer[batchMeter]
}

func newBatchNode(t *testing.T, id, kind string, core int, delay time.Duration) *batchNode {
	t.Helper()
	node := &batchNode{id: id, kind: kind, core: core, timeout: 3000, expected: "success",
		url: "http://127.0.0.1:1/acceptance/" + id}
	if kind == "invalid_config" {
		node.expected, node.config = "config_error", "{"
		return node
	}
	if kind == "connect_rejected" || kind == "endpoint_503" || kind == "stalled" {
		node.expected = "remote_failure"
	}
	if kind == "stalled" {
		node.timeout = 700
	}
	family := "xray"
	if core == 1 {
		family = "sing-box"
	}
	authorization := "Basic " + base64.StdEncoding.EncodeToString([]byte(id+":fixture-"+id))
	var connectionsMu sync.Mutex
	connections := map[net.Conn]struct{}{}
	var handlers sync.WaitGroup
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		handlers.Add(1)
		defer handlers.Done()
		meter := node.meter.Load()
		meter.update(family, 1)
		defer meter.update(family, -1)
		node.hits.Add(1)
		wrong := func(code int) {
			meter.mu.Lock()
			meter.wrong++
			meter.mu.Unlock()
			w.WriteHeader(code)
		}
		// It is a fixture, not an open forward proxy: only this synthetic node's
		// credentials and the unreachable loopback destination are accepted.
		if r.Method != http.MethodConnect || r.Host != "127.0.0.1:1" {
			wrong(http.StatusBadRequest)
			return
		}
		if r.Header.Get("Proxy-Authorization") != authorization {
			wrong(http.StatusProxyAuthRequired)
			return
		}
		if kind == "connect_rejected" {
			w.WriteHeader(http.StatusForbidden)
			return
		}
		connection, buffered, err := w.(http.Hijacker).Hijack()
		if err != nil {
			return
		}
		connectionsMu.Lock()
		connections[connection] = struct{}{}
		connectionsMu.Unlock()
		defer func() {
			connection.Close()
			connectionsMu.Lock()
			delete(connections, connection)
			connectionsMu.Unlock()
		}()
		connection.SetDeadline(time.Now().Add(25 * time.Second))
		fmt.Fprint(connection, "HTTP/1.1 200 Connection Established\r\n\r\n")
		probe, err := http.ReadRequest(buffered.Reader)
		if err != nil {
			return
		}
		probe.Body.Close()
		if probe.URL.Path != "/acceptance/"+id {
			meter.mu.Lock()
			meter.wrong++
			meter.mu.Unlock()
			fmt.Fprint(connection, "HTTP/1.1 409 Conflict\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
			return
		}
		if kind == "stalled" {
			var b [1]byte
			connection.Read(b[:]) // EOF proves cancellation reached the proxy socket.
			return
		}
		time.Sleep(delay)
		if kind == "endpoint_503" {
			fmt.Fprint(connection, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
		} else {
			fmt.Fprint(connection, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
		}
	}))
	t.Cleanup(func() {
		server.Close()
		connectionsMu.Lock()
		for connection := range connections {
			connection.Close()
		}
		connectionsMu.Unlock()
		handlers.Wait()
	})
	port := server.Listener.Addr().(*net.TCPAddr).Port
	var outbound map[string]any
	if core == 0 {
		outbound = map[string]any{"tag": "proxy", "protocol": "http", "settings": map[string]any{
			"servers": []any{map[string]any{"address": "127.0.0.1", "port": port,
				"users": []any{map[string]any{"user": id, "pass": "fixture-" + id}}}},
		}}
	} else {
		outbound = map[string]any{"tag": "proxy", "type": "http", "server": "127.0.0.1", "server_port": port,
			"username": id, "password": "fixture-" + id}
	}
	encoded, err := json.Marshal(map[string]any{"outbounds": []any{outbound}})
	if err != nil {
		t.Fatal(err)
	}
	node.config = string(encoded)
	return node
}

func runBatchAcceptancePhase(ctx context.Context, name string, nodes []*batchNode, cores []*batchCore, budget int, meter *batchMeter) batchPhaseReport {
	phase := batchPhaseReport{Name: name, Nodes: len(nodes), Budget: budget, Results: make([]batchProbeResult, len(nodes))}
	for index, node := range nodes {
		node.meter.Store(meter)
		phase.Results[index] = batchProbeResult{ID: node.id, Core: cores[node.core].identity.Family, Kind: node.kind, Expected: node.expected, Status: "not_started"}
	}
	var next, active, peak atomic.Int32
	var workers sync.WaitGroup
	started := time.Now()
	for worker := 0; worker < min(budget, len(nodes)); worker++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			clients := make([]gen.LibcoreServiceClient, len(cores))
			for index, core := range cores {
				connection, err := grpc.NewClient(core.address, grpc.WithTransportCredentials(insecure.NewCredentials()))
				if err == nil {
					defer connection.Close()
					clients[index] = gen.NewLibcoreServiceClient(connection)
				}
			}
			for ctx.Err() == nil {
				index := int(next.Add(1)) - 1
				if index >= len(nodes) || ctx.Err() != nil {
					return
				}
				node, record := nodes[index], &phase.Results[index]
				core := cores[node.core]
				record.Started = true
				if clients[node.core] == nil {
					record.Status = "core_error"
					continue
				}
				current := active.Add(1)
				for prior := peak.Load(); current > prior && !peak.CompareAndSwap(prior, current); prior = peak.Load() {
				}
				beforeHits := node.hits.Load()
				callStarted := time.Now()
				callCtx, cancel := context.WithTimeout(core.authenticate(ctx), time.Duration(node.timeout+2500)*time.Millisecond)
				response, err := clients[node.core].Test(callCtx, &gen.TestReq{Mode: gen.TestMode_UrlTest, Timeout: node.timeout,
					Url: node.url, Config: &gen.LoadConfigReq{CoreConfig: node.config}})
				cancel()
				active.Add(-1)
				record.ElapsedMS = time.Since(callStarted).Milliseconds()
				record.ProxyHits = node.hits.Load() - beforeHits
				record.RPCStatus = status.Code(err).String()
				if ctx.Err() != nil || status.Code(err) == codes.Canceled {
					record.Status = "cancelled"
				} else if err != nil {
					record.Status = "core_error"
				} else {
					record.LatencyMS = response.GetMs()
					record.FailureCategory = response.GetFailureCategory().String()
					record.IdentityVerified = response.GetCoreFamily() == core.identity.Family && response.GetBuildId() == core.identity.BuildID
					switch response.GetFailureCategory() {
					case gen.FailureCategory_FailureNone:
						record.Status = "success"
						if response.GetError() != "" {
							record.Status = "core_error"
						}
					case gen.FailureCategory_FailureRemote:
						record.Status = "remote_failure"
					case gen.FailureCategory_FailureConfig:
						record.Status = "config_error"
					case gen.FailureCategory_FailureUnsupported:
						record.Status = "unsupported"
					case gen.FailureCategory_FailureCancelled:
						record.Status = "cancelled"
					default:
						record.Status = "core_error"
					}
				}
			}
		}()
	}
	workers.Wait()
	phase.ElapsedMS = time.Since(started).Milliseconds()
	phase.ClientPeak = int(peak.Load())
	for index := range phase.Results {
		record := &phase.Results[index]
		if !record.Started && ctx.Err() != nil {
			record.Status = "cancelled_not_started"
		}
		if record.Started {
			phase.Dispatched++
		}
		if record.Status == "success" {
			phase.Successes++
		}
		if strings.HasPrefix(record.Status, "cancelled") {
			phase.Cancelled++
		}
	}
	phase.RequestsPerSec = float64(phase.Dispatched) / max(time.Since(started).Seconds(), 0.001)
	drainStarted := time.Now()
	phase.Drained = meter.waitActive(0, 3*time.Second)
	phase.DrainMS = time.Since(drainStarted).Milliseconds()
	meter.mu.Lock()
	phase.ServerPeak, phase.ServerPeakByCore, phase.WrongProxyRoutes = meter.peak, meter.peakCores, meter.wrong
	meter.mu.Unlock()
	return phase
}

func batchOption(t *testing.T, name string, fallback, low, high int) int {
	t.Helper()
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	number, err := strconv.Atoi(value)
	if err != nil || number < low || number > high {
		t.Fatalf("%s must be between %d and %d", name, low, high)
	}
	return number
}

// Optional acceptance test of the real executables and RPC transport. It is
// intentionally separate from the GUI scheduler regression suite: no GUI,
// installed node, system proxy, privileged process, or TUN is used here.
func TestMixedCoreBatchAcceptance(t *testing.T) {
	directory := os.Getenv("NEKORAY_CORE_BIN_DIR")
	if directory == "" {
		t.Skip("set NEKORAY_CORE_BIN_DIR to run real-core mixed batch acceptance")
	}
	directory, err := filepath.Abs(directory)
	if err != nil {
		t.Fatal(err)
	}
	nodeCount := batchOption(t, "NEKORAY_BATCH_NODES", 24, 8, 64)
	budget := batchOption(t, "NEKORAY_BATCH_CONCURRENCY", 6, 2, min(32, nodeCount))
	delay := batchOption(t, "NEKORAY_BATCH_DELAY_MS", 200, 80, 1000)
	report := batchAcceptanceReport{SchemaVersion: 1, Scope: "real-core-rpc-loopback; GUI scheduler tested separately",
		StartedAtUTC: time.Now().UTC().Format(time.RFC3339Nano), Platform: runtime.GOOS + "/" + runtime.GOARCH,
		GoVersion: runtime.Version(), CPUCount: runtime.NumCPU(), NodeCount: nodeCount, Concurrency: budget, DelayMS: delay,
		MinimumSpeedup: min(2.0, float64(budget)*0.6), Issues: []string{}}
	t.Cleanup(func() {
		report.CompletedAtUTC = time.Now().UTC().Format(time.RFC3339Nano)
		report.Status = "passed"
		if t.Failed() {
			report.Status = "failed"
		}
		if destination := os.Getenv("NEKORAY_BATCH_REPORT"); destination != "" {
			encoded, err := json.MarshalIndent(report, "", "  ")
			if err == nil {
				err = os.WriteFile(destination, append(encoded, '\n'), 0o600)
			}
			if err != nil {
				t.Errorf("write batch acceptance report: %v", err)
			}
		}
	})
	check := func(ok bool, format string, values ...any) {
		if !ok {
			message := fmt.Sprintf(format, values...)
			report.Issues = append(report.Issues, message)
			t.Error(message)
		}
	}
	cores := []*batchCore{
		startBatchCore(t, directory, "nekoray", "xray", "1.8.4"),
		startBatchCore(t, directory, "nekobox", "sing-box", "1.14.0"),
	}
	for _, core := range cores {
		report.Cores = append(report.Cores, core.identity)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 150*time.Second)
	defer cancel()
	var healthy []*batchNode
	for index := 0; index < nodeCount; index++ {
		healthy = append(healthy, newBatchNode(t, fmt.Sprintf("healthy-%02d", index), "healthy", index%2, time.Duration(delay)*time.Millisecond))
	}
	validate := func(phase batchPhaseReport) {
		check(phase.Drained, "%s left proxy requests open", phase.Name)
		check(phase.ClientPeak <= phase.Budget && phase.ServerPeak <= phase.Budget,
			"%s exceeded total budget %d: client=%d server=%d", phase.Name, phase.Budget, phase.ClientPeak, phase.ServerPeak)
		check(phase.WrongProxyRoutes == 0, "%s mixed node credentials, destination or URL: %d wrong routes", phase.Name, phase.WrongProxyRoutes)
		for _, result := range phase.Results {
			check(result.Status == result.Expected, "%s/%s expected %s, got %s (%s/%s)", phase.Name, result.ID, result.Expected, result.Status, result.RPCStatus, result.FailureCategory)
			check(result.IdentityVerified, "%s/%s returned the wrong executable identity", phase.Name, result.ID)
			if result.Kind != "invalid_config" {
				check(result.ProxyHits > 0, "%s/%s did not use its own proxy", phase.Name, result.ID)
			}
			if result.Status == "success" {
				check(result.LatencyMS > 0, "%s/%s has no latency measurement", phase.Name, result.ID)
			}
		}
		t.Logf("%s: %d/%d success, %d ms, %.1f requests/s, actual peak %d (budget %d)",
			phase.Name, phase.Successes, phase.Nodes, phase.ElapsedMS, phase.RequestsPerSec, phase.ServerPeak, phase.Budget)
	}
	run := func(name string, nodes []*batchNode, concurrency int) batchPhaseReport {
		phase := runBatchAcceptancePhase(ctx, name, nodes, cores, concurrency, newBatchMeter())
		report.Phases = append(report.Phases, phase)
		validate(phase)
		return phase
	}
	serial := run("healthy_serial", healthy, 1)
	parallel := run("healthy_mixed_batch", healthy, budget)
	report.ThroughputSpeedup = float64(serial.ElapsedMS) / float64(max(1, parallel.ElapsedMS))
	check(parallel.ServerPeak == budget, "healthy mixed batch did not fill total budget: peak=%d budget=%d", parallel.ServerPeak, budget)
	check(report.ThroughputSpeedup >= report.MinimumSpeedup, "mixed batch speedup %.2fx below %.2fx; possible serialization or overloaded test host", report.ThroughputSpeedup, report.MinimumSpeedup)
	var varied []*batchNode
	for _, kind := range []string{"fast", "slow", "connect_rejected", "endpoint_503", "stalled", "invalid_config"} {
		for core := 0; core < 2; core++ {
			latency := 10 * time.Millisecond
			if kind == "slow" {
				latency = 150 * time.Millisecond
			}
			varied = append(varied, newBatchNode(t, fmt.Sprintf("%s-%d", kind, core), kind, core, latency))
		}
	}
	accuracySerial := run("accuracy_serial", varied, 1)
	accuracyBatch := run("accuracy_mixed_batch", varied, budget)
	for _, pair := range [][2]batchPhaseReport{{serial, parallel}, {accuracySerial, accuracyBatch}} {
		for index, baseline := range pair[0].Results {
			result := pair[1].Results[index]
			report.ComparedResults++
			matches := baseline.ID == result.ID && baseline.Core == result.Core && baseline.Status == result.Status && baseline.FailureCategory == result.FailureCategory
			if matches {
				report.MatchedResults++
			}
			check(matches, "serial/batch mismatch for %s: %s/%s versus %s/%s", baseline.ID, baseline.Status, baseline.FailureCategory, result.Status, result.FailureCategory)
		}
	}
	var stalled []*batchNode
	for index := 0; index < max(nodeCount, budget*3); index++ {
		node := newBatchNode(t, fmt.Sprintf("cancel-%02d", index), "stalled", index%2, 0)
		node.timeout = 10000
		stalled = append(stalled, node)
	}
	cancelCtx, cancelBatch := context.WithCancel(ctx)
	defer cancelBatch()
	meter := newBatchMeter()
	completed := make(chan batchPhaseReport, 1)
	go func() {
		completed <- runBatchAcceptancePhase(cancelCtx, "cancel_mixed_batch", stalled, cores, budget, meter)
	}()
	filled := meter.waitActive(budget, 8*time.Second)
	cancelStarted := time.Now()
	cancelBatch()
	cancelled := <-completed
	report.CancelToReturnMS = time.Since(cancelStarted).Milliseconds()
	report.Phases = append(report.Phases, cancelled)
	check(filled, "cancellation batch could not reach total concurrency %d", budget)
	check(cancelled.Dispatched == budget, "cancellation admitted %d requests, expected only %d active workers", cancelled.Dispatched, budget)
	check(cancelled.Cancelled == len(stalled), "cancellation left %d/%d tasks without cancelled status", len(stalled)-cancelled.Cancelled, len(stalled))
	check(cancelled.WrongProxyRoutes == 0 && cancelled.ClientPeak <= budget,
		"cancellation mixed proxy routes or exceeded its client budget: wrong=%d peak=%d", cancelled.WrongProxyRoutes, cancelled.ClientPeak)
	check(cancelled.Drained && report.CancelToReturnMS < 3000, "cancellation did not drain within 3 seconds: %d ms, drained=%t", report.CancelToReturnMS, cancelled.Drained)
	check(cancelled.ServerPeak == budget && cancelled.ServerPeakByCore["xray"] > 0 && cancelled.ServerPeakByCore["sing-box"] > 0,
		"cancellation did not exercise both cores under one budget: peak=%d per-core=%v", cancelled.ServerPeak, cancelled.ServerPeakByCore)
	t.Logf("cancel_mixed_batch: %d active, %d queued, returned/drained in %d ms", cancelled.Dispatched, len(stalled)-cancelled.Dispatched, report.CancelToReturnMS)
	run("recovery_mixed_batch", healthy, budget) // Reuse the same two daemon processes.
	t.Logf("acceptance: %d/%d serial/batch statuses match; mixed throughput %.2fx; fixed Xray 1.8.4 + sing-box 1.14.0", report.MatchedResults, report.ComparedResults, report.ThroughputSpeedup)
}
