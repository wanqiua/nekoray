package testutil

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"grpc_server/gen"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// This is the transport portion of the Windows TUN acceptance path. All
// sockets and process working directories are private to the test. It never
// creates a TUN, changes routes, or uses the installed application settings.
func TestCoreUDPBridgeProcesses(t *testing.T) {
	directory := os.Getenv("NEKORAY_CORE_BIN_DIR")
	if directory == "" {
		t.Skip("set NEKORAY_CORE_BIN_DIR to test the real SOCKS UDP bridge")
	}
	directory, err := filepath.Abs(directory)
	if err != nil {
		t.Fatal(err)
	}
	peerAddress, received := udpBridgeEchoPeer(t)
	_, portText, _ := net.SplitHostPort(peerAddress)
	peerPort, _ := strconv.Atoi(portText)
	check := func(t *testing.T, address string) {
		t.Helper()
		for _, destination := range []string{"203.0.113.33", "2001:db8::33"} {
			t.Run(destination, func(t *testing.T) {
				before := received.Load()
				udpBridgeRoundTrip(t, address, net.ParseIP(destination), []byte("udp-bridge-"+destination))
				if received.Load() != before+1 {
					t.Fatalf("UDP did not reach the synthetic peer exactly once: before=%d after=%d", before, received.Load())
				}
			})
		}
	}
	t.Run("fixture", func(t *testing.T) { check(t, peerAddress) })
	xray := startBatchCore(t, directory, "nekoray", "xray", "1.8.4")
	xrayPort := FreePort(t)
	udpBridgeLoad(t, xray, fmt.Sprintf(`{"log":{"loglevel":"debug"},"inbounds":[{"tag":"tun-proxy-in","listen":"127.0.0.1","port":%d,"protocol":"socks","settings":{"udp":true}}],"outbounds":[{"tag":"proxy","protocol":"socks","settings":{"servers":[{"address":"127.0.0.1","port":%d}]}}],"routing":{"rules":[{"type":"field","inboundTag":["tun-proxy-in"],"outboundTag":"proxy"}]}}`, xrayPort, peerPort))
	t.Run("xray", func(t *testing.T) { check(t, fmt.Sprintf("127.0.0.1:%d", xrayPort)) })
	for _, detect := range []bool{false, true} {
		t.Run(fmt.Sprintf("sing-box-to-xray/auto-detect-%t", detect), func(t *testing.T) {
			box := startBatchCore(t, directory, "nekobox", "sing-box", "1.14.0")
			boxPort := FreePort(t)
			udpBridgeLoad(t, box, fmt.Sprintf(`{"log":{"level":"debug"},"inbounds":[{"type":"socks","tag":"test-in","listen":"127.0.0.1","listen_port":%d}],"outbounds":[{"type":"socks","tag":"proxy","server":"127.0.0.1","server_port":%d,"version":"5"}],"route":{"auto_detect_interface":%t,"final":"proxy"}}`, boxPort, xrayPort, detect))
			check(t, fmt.Sprintf("127.0.0.1:%d", boxPort))
		})
	}
}

func udpBridgeLoad(t *testing.T, core *batchCore, config string) {
	t.Helper()
	connection, err := grpc.NewClient(core.address, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer connection.Close()
	ctx, cancel := context.WithTimeout(core.authenticate(context.Background()), 5*time.Second)
	defer cancel()
	response, err := gen.NewLibcoreServiceClient(connection).Start(ctx, &gen.LoadConfigReq{CoreConfig: config})
	if err != nil || response.Error != "" {
		t.Fatalf("start UDP bridge: %v %v", response, err)
	}
}

// The peer implements RFC 1928 UDP ASSOCIATE and returns the original address
// and payload. Reserved destinations are only labels inside the SOCKS packet;
// neither this peer nor either core connects to those Internet addresses.
func udpBridgeEchoPeer(t *testing.T) (string, *atomic.Int32) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	relay, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		listener.Close()
		t.Fatal(err)
	}
	var connections sync.Map
	var received atomic.Int32
	var workers sync.WaitGroup
	t.Cleanup(func() {
		listener.Close()
		relay.Close()
		connections.Range(func(key, _ any) bool { key.(net.Conn).Close(); return true })
		workers.Wait()
	})
	workers.Add(1)
	go func() {
		defer workers.Done()
		packet := make([]byte, 65535)
		for {
			n, source, err := relay.ReadFromUDP(packet)
			if err != nil {
				return
			}
			if n < 4 || !bytes.Equal(packet[:3], []byte{0, 0, 0}) {
				continue
			}
			if _, err := udpBridgeReadAddress(bytes.NewReader(packet[3:n])); err != nil {
				continue
			}
			received.Add(1)
			if _, err := relay.WriteToUDP(packet[:n], source); err != nil {
				t.Errorf("synthetic UDP echo: %v", err)
				return
			}
		}
	}()
	workers.Add(1)
	go func() {
		defer workers.Done()
		for {
			control, err := listener.Accept()
			if err != nil {
				return
			}
			connections.Store(control, struct{}{})
			workers.Add(1)
			go func() {
				defer workers.Done()
				defer control.Close()
				defer connections.Delete(control)
				control.SetDeadline(time.Now().Add(10 * time.Second))
				greeting := make([]byte, 2)
				if _, err := io.ReadFull(control, greeting); err != nil || greeting[0] != 5 {
					return
				}
				methods := make([]byte, int(greeting[1]))
				if _, err := io.ReadFull(control, methods); err != nil || !bytes.Contains(methods, []byte{0}) {
					return
				}
				if _, err := control.Write([]byte{5, 0}); err != nil {
					return
				}
				header := make([]byte, 3)
				if _, err := io.ReadFull(control, header); err != nil || !bytes.Equal(header, []byte{5, 3, 0}) {
					return
				}
				if _, err := udpBridgeReadAddress(control); err != nil {
					return
				}
				port := relay.LocalAddr().(*net.UDPAddr).Port
				if _, err := control.Write([]byte{5, 0, 0, 1, 127, 0, 0, 1, byte(port >> 8), byte(port)}); err != nil {
					return
				}
				control.SetDeadline(time.Time{})
				_, _ = io.Copy(io.Discard, control)
			}()
		}
	}()
	return listener.Addr().String(), &received
}

func udpBridgeReadAddress(reader io.Reader) ([]byte, error) {
	typeByte := make([]byte, 1)
	if _, err := io.ReadFull(reader, typeByte); err != nil {
		return nil, err
	}
	length := 0
	switch typeByte[0] {
	case 1:
		length = 4
	case 4:
		length = 16
	case 3:
		nameLength := make([]byte, 1)
		if _, err := io.ReadFull(reader, nameLength); err != nil {
			return nil, err
		}
		typeByte = append(typeByte, nameLength...)
		length = int(nameLength[0])
	default:
		return nil, fmt.Errorf("invalid SOCKS address type %d", typeByte[0])
	}
	address := make([]byte, length+2)
	_, err := io.ReadFull(reader, address)
	return append(typeByte, address...), err
}

func udpBridgeRoundTrip(t *testing.T, socksAddress string, destination net.IP, payload []byte) {
	t.Helper()
	control, err := net.DialTimeout("tcp", socksAddress, 3*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer control.Close()
	control.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := control.Write([]byte{5, 1, 0}); err != nil {
		t.Fatal(err)
	}
	greeting := make([]byte, 2)
	if _, err := io.ReadFull(control, greeting); err != nil || !bytes.Equal(greeting, []byte{5, 0}) {
		t.Fatalf("SOCKS greeting %x: %v", greeting, err)
	}
	if _, err := control.Write([]byte{5, 3, 0, 1, 0, 0, 0, 0, 0, 0}); err != nil {
		t.Fatal(err)
	}
	header := make([]byte, 3)
	if _, err := io.ReadFull(control, header); err != nil || !bytes.Equal(header, []byte{5, 0, 0}) {
		t.Fatalf("UDP ASSOCIATE %x: %v", header, err)
	}
	bound, err := udpBridgeReadAddress(control)
	if err != nil || (bound[0] != 1 && bound[0] != 4) {
		t.Fatalf("UDP relay %x: %v", bound, err)
	}
	relay := &net.UDPAddr{IP: net.IP(bound[1 : len(bound)-2]), Port: int(binary.BigEndian.Uint16(bound[len(bound)-2:]))}
	if relay.IP.IsUnspecified() {
		relay.IP = net.IPv4(127, 0, 0, 1)
	}
	udp, err := net.DialUDP("udp", nil, relay)
	if err != nil {
		t.Fatal(err)
	}
	defer udp.Close()
	udp.SetDeadline(time.Now().Add(5 * time.Second))
	packet := []byte{0, 0, 0}
	if destination.To4() != nil {
		packet = append(packet, 1)
		packet = append(packet, destination.To4()...)
	} else {
		packet = append(packet, 4)
		packet = append(packet, destination.To16()...)
	}
	packet = append(packet, byte(19090>>8), byte(19090&255))
	packet = append(packet, payload...)
	if _, err := udp.Write(packet); err != nil {
		t.Fatal(err)
	}
	response := make([]byte, 65535)
	n, err := udp.Read(response)
	if err != nil {
		t.Fatalf("UDP via %s (relay %s): %v", socksAddress, relay, err)
	}
	if !bytes.Equal(response[:n], packet) {
		t.Fatalf("UDP address or payload changed: got %x want %x", response[:n], packet)
	}
}
