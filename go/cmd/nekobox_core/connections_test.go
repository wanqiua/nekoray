package main

import (
	"context"
	"encoding/json"
	"fmt"
	"grpc_server/gen"
	"grpc_server/testutil"
	"io"
	"net"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/sagernet/sing-box/adapter"
	tun "github.com/sagernet/sing-tun"
	M "github.com/sagernet/sing/common/metadata"
	"golang.org/x/net/proxy"
)

func connectionEcho(test *testing.T) string {
	test.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		test.Fatal(err)
	}
	test.Cleanup(func() { listener.Close() })
	go func() {
		for {
			connection, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer connection.Close()
				connection.SetDeadline(time.Now().Add(10 * time.Second))
				io.Copy(connection, connection)
			}()
		}
	}()
	return listener.Addr().String()
}

func trackedSOCKSConnection(test *testing.T, address, target string) net.Conn {
	test.Helper()
	dialer, err := proxy.SOCKS5("tcp", address, nil, &net.Dialer{Timeout: 3 * time.Second})
	if err != nil {
		test.Fatal(err)
	}
	connection, err := dialer.Dial("tcp", target)
	if err != nil {
		test.Fatal(err)
	}
	test.Cleanup(func() { connection.Close() })
	connection.SetDeadline(time.Now().Add(5 * time.Second))
	payload := []byte("connection-list")
	if _, err := connection.Write(payload); err != nil {
		test.Fatal(err)
	}
	reply := make([]byte, len(payload))
	if _, err := io.ReadFull(connection, reply); err != nil || string(reply) != string(payload) {
		test.Fatalf("SOCKS echo %q: %v", reply, err)
	}
	return connection
}

func awaitConnections(test *testing.T, implementation *server, match func([]connectionInfo) bool) []connectionInfo {
	test.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for {
		response, err := implementation.ListConnections(context.Background(), &gen.EmptyReq{})
		if err != nil {
			test.Fatal(err)
		}
		var items []connectionInfo
		if err := json.Unmarshal([]byte(response.NekorayConnectionsJson), &items); err != nil || items == nil {
			test.Fatalf("invalid connection array %q: %v", response.NekorayConnectionsJson, err)
		}
		if match(items) {
			return items
		}
		if time.Now().After(deadline) {
			test.Fatalf("unexpected connection list: %+v", items)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestSingBoxConnectionListing(test *testing.T) {
	for _, clashAPI := range []bool{false, true} {
		for _, enabled := range []bool{false, true} {
			test.Run(fmt.Sprintf("clash=%t/enabled=%t", clashAPI, enabled), func(test *testing.T) {
				implementation := &server{}
				ctx := context.Background()
				empty := func(items []connectionInfo) bool { return len(items) == 0 }
				awaitConnections(test, implementation, empty)
				port := testutil.FreePort(test)
				config := legacyGUIConfig(port)
				if clashAPI {
					config = strings.Replace(config, `"log":`, `"experimental":{"clash_api":{}},"log":`, 1)
				}
				request := &gen.LoadConfigReq{CoreConfig: config, StatsOutbounds: []string{"proxy"}, EnableNekorayConnections: enabled}
				start := func() {
					test.Helper()
					response, err := implementation.Start(ctx, request)
					if err != nil || response.Error != "" {
						test.Fatalf("Start: %v %v", response, err)
					}
				}
				stop := func() {
					test.Helper()
					response, err := implementation.Stop(ctx, &gen.EmptyReq{})
					if err != nil || response.Error != "" {
						test.Errorf("Stop: %v %v", response, err)
					}
				}
				start()
				test.Cleanup(stop)
				awaitConnections(test, implementation, empty)
				target := connectionEcho(test)
				address := fmt.Sprintf("127.0.0.1:%d", port)
				connection := trackedSOCKSConnection(test, address, target)
				if enabled {
					active := awaitConnections(test, implementation, func(items []connectionInfo) bool {
						return len(items) == 1 && items[0].End == 0
					})[0]
					if active.ID == 0 || active.Start <= 0 || active.Tag != "proxy" || active.Dest != "tcp:"+target || active.RDest != "" {
						test.Fatalf("incorrect TCP metadata: %+v", active)
					}
					connection.Close()
					closed := awaitConnections(test, implementation, func(items []connectionInfo) bool {
						return len(items) == 1 && items[0].End >= active.Start
					})[0]
					if closed.ID != active.ID || closed.Dest != active.Dest {
						test.Fatalf("closed connection lost identity: %+v -> %+v", active, closed)
					}
					runtime.GC()
					awaitConnections(test, implementation, func(items []connectionInfo) bool { return len(items) == 1 && items[0].ID == active.ID })
				} else {
					awaitConnections(test, implementation, empty)
					connection.Close()
				}
				testutil.TestUDP(test, address)
				if enabled {
					awaitConnections(test, implementation, func(items []connectionInfo) bool {
						return len(items) == 2 && strings.HasPrefix(items[0].Dest, "udp:127.0.0.1:") && items[0].Tag == "proxy" && items[0].Start > 0
					})
					probe, err := currentInstance().DialContext(ctx, "tcp", target)
					if err != nil {
						test.Fatal(err)
					}
					probe.Close()
					awaitConnections(test, implementation, func(items []connectionInfo) bool { return len(items) == 2 })
				} else {
					awaitConnections(test, implementation, empty)
				}
				stop()
				awaitConnections(test, implementation, empty)
				start()
				awaitConnections(test, implementation, empty)
				trackedSOCKSConnection(test, address, target)
				if enabled {
					awaitConnections(test, implementation, func(items []connectionInfo) bool { return len(items) == 1 && items[0].End == 0 })
				} else {
					awaitConnections(test, implementation, empty)
				}
			})
		}
	}
}

func TestConnectionHistoryBoundedAndConcurrent(test *testing.T) {
	registry := newConnectionRegistry(nil)
	metadata := adapter.InboundContext{Network: "tcp", Destination: M.ParseSocksaddr("127.0.0.1:443")}
	active := registry.begin(metadata, "proxy")
	var snapshots sync.WaitGroup
	snapshots.Go(func() {
		for range 200 {
			registry.snapshot()
		}
	})
	for range 1105 {
		id := registry.begin(metadata, "proxy")
		registry.end(id)
		registry.end(id)
	}
	snapshots.Wait()
	runtime.GC()
	var items []connectionInfo
	if err := json.Unmarshal([]byte(registry.snapshot()), &items); err != nil {
		test.Fatal(err)
	}
	if len(items) != connectionHistorySize+1 || items[len(items)-1].ID != active || items[len(items)-1].End != 0 {
		test.Fatalf("bounded history must retain older active connections: %+v", items)
	}
	for index := 1; index < len(items); index++ {
		if items[index].ID >= items[index-1].ID {
			test.Fatal("IDs must be unique and ordered newest first")
		}
	}
	registry.close()
	registry.end(active)
	if registry.snapshot() != "[]" || registry.begin(metadata, "proxy") != 0 {
		test.Fatal("closed registry accepted or retained a connection")
	}
}

func TestConnectionFlowLifecycle(test *testing.T) {
	registry := newConnectionRegistry(nil)
	flow := &trackedFlow{registry: registry, tag: "proxy", metadata: adapter.InboundContext{Network: "udp", Destination: M.ParseSocksaddr("1.1.1.1:53")}}
	if registry.snapshot() != "[]" {
		test.Fatal("unattached flow must not appear")
	}
	flow.AttachFlow(nil)
	flow.AttachFlow(nil)
	flow.CountForward(100)
	flow.CountReverse(100)
	flow.FlowEstablished()
	flow.CloseFlow(tun.FlowCloseTimeout)
	flow.CloseFlow(tun.FlowCloseFinished)
	flow.AttachFlow(nil)
	var items []connectionInfo
	json.Unmarshal([]byte(registry.snapshot()), &items)
	if len(items) != 1 || items[0].Dest != "udp:1.1.1.1:53" || items[0].End == 0 {
		test.Fatalf("invalid flow history: %+v", items)
	}
}

func TestConnectionDestinationRewrite(test *testing.T) {
	for _, testCase := range []struct {
		name, destination, routeOriginal, origin, wantDest, wantRewrite string
		fakeIP                                                          bool
	}{
		{"unchanged IPv6", "[::1]:443", "", "", "tcp:[::1]:443", "", false},
		{"listener origin", "example.com:443", "", "127.0.0.1:10808", "tcp:example.com:443", "", false},
		{"route override", "127.0.0.1:8443", "example.com:443", "", "tcp:example.com:443", "tcp:127.0.0.1:8443", false},
		{"fake IP", "example.com:443", "", "198.18.0.1:443", "tcp:198.18.0.1:443", "tcp:example.com:443", true},
		{"both rewrites", "127.0.0.1:8443", "example.com:443", "198.18.0.1:443", "tcp:198.18.0.1:443", "tcp:127.0.0.1:8443", true},
	} {
		test.Run(testCase.name, func(test *testing.T) {
			item := connectionDetails(adapter.InboundContext{
				Network: "tcp", Destination: M.ParseSocksaddr(testCase.destination), FakeIP: testCase.fakeIP,
				RouteOriginalDestination: M.ParseSocksaddr(testCase.routeOriginal), OriginDestination: M.ParseSocksaddr(testCase.origin),
			}, "proxy")
			if item.Dest != testCase.wantDest || item.RDest != testCase.wantRewrite {
				test.Fatalf("incorrect destination mapping: %+v", item)
			}
		})
	}
}
