package main

import (
	"bytes"
	"context"
	"errors"
	"net"
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"

	"github.com/matsuridayo/libneko/iphlpapi"
	xnet "github.com/xtls/xray-core/common/net"
	"github.com/xtls/xray-core/transport/internet"
	"golang.org/x/sys/windows"
)

// Only the wrapper's in-memory route snapshot changes. No TUN, route, DNS or
// adapter settings are created or modified, and all traffic stays on loopback.
func syntheticTunRoute(t *testing.T) uint32 {
	t.Helper()
	if os.Getenv("NKR_CORE_RAY_WINDOWS_DISABLE_AUTO_INTERFACE") == "1" {
		t.Skip("Windows interface protection is disabled in this process")
	}
	available, err := net.Interfaces()
	if err != nil {
		t.Fatal(err)
	}
	var upstream net.Interface
	for _, candidate := range available {
		if candidate.Flags&net.FlagUp == 0 || candidate.Flags&net.FlagLoopback != 0 || candidate.Name == "nekoray-tun" {
			continue
		}
		addresses, err := candidate.Addrs()
		if err != nil {
			continue
		}
		for _, address := range addresses {
			ip, _, err := net.ParseCIDR(address.String())
			if err == nil && ip.To4() != nil && !ip.IsLoopback() {
				upstream = candidate
				break
			}
		}
		if upstream.Index != 0 {
			break
		}
	}
	if upstream.Index == 0 {
		t.Skip("no active non-loopback IPv4 interface is available for the socket-only binding check")
	}
	lock.Lock()
	savedInterfaces, savedRoutes := interfaces, routes
	interfaces = []net.Interface{{Index: 0x3fffffff, Name: "nekoray-tun"}, upstream}
	routes = []iphlpapi.RouteRow{{ForwardIfIndex: uint32(upstream.Index), ForwardType: 4, ForwardProto: 3}}
	lock.Unlock()
	t.Cleanup(func() {
		lock.Lock()
		interfaces, routes = savedInterfaces, savedRoutes
		lock.Unlock()
	})
	if got := getBindInterfaceIndex("203.0.113.33:19090"); got != uint32(upstream.Index) {
		t.Fatalf("synthetic route selection returned %d, want %d", got, upstream.Index)
	}
	return uint32(upstream.Index)
}

func TestWindowsProtectedUDPLoopback(t *testing.T) {
	syntheticTunRoute(t)
	for _, address := range []string{"127.0.0.1", "::1"} {
		t.Run(address, func(t *testing.T) {
			peer, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP(address)})
			if err != nil {
				t.Fatal(err)
			}
			defer peer.Close()
			peer.SetDeadline(time.Now().Add(2 * time.Second))
			port := peer.LocalAddr().(*net.UDPAddr).Port
			destination := xnet.UDPDestination(xnet.ParseAddress(address), xnet.Port(port))
			connection, err := internet.DialSystem(context.Background(), destination, nil)
			if err != nil {
				t.Fatal(err)
			}
			defer connection.Close()
			connection.SetDeadline(time.Now().Add(2 * time.Second))
			payload := []byte("protected-loopback-udp")
			if _, err := connection.Write(payload); err != nil {
				t.Fatalf("UDP to loopback while interface protection is active: %v", err)
			}
			buffer := make([]byte, 128)
			count, source, err := peer.ReadFromUDP(buffer)
			if err != nil || !bytes.Equal(buffer[:count], payload) {
				t.Fatalf("loopback peer receive: bytes=%d error=%v", count, err)
			}
			if !source.IP.IsLoopback() {
				t.Fatalf("loopback request used a non-loopback source: %s", source)
			}
			if _, err := peer.WriteToUDP(payload, source); err != nil {
				t.Fatal(err)
			}
			count, err = connection.Read(buffer)
			if err != nil || !bytes.Equal(buffer[:count], payload) {
				t.Fatalf("loopback UDP reply: bytes=%d error=%v", count, err)
			}
		})
	}
}

func TestWindowsProtectedUDPStillBindsNonLoopback(t *testing.T) {
	upstream := syntheticTunRoute(t)
	dialer := &windowsProtectedSystemDialer{SystemDialer: &internet.DefaultSystemDialer{}}
	for _, address := range []string{"203.0.113.33", "2001:db8::33"} {
		t.Run(address, func(t *testing.T) {
			// Creating this unconnected socket sends no packet to the reserved
			// address. Read back both Windows interface options before closing.
			connection, err := dialer.Dial(context.Background(), nil, xnet.UDPDestination(xnet.ParseAddress(address), 19090), nil)
			if err != nil {
				t.Fatal(err)
			}
			defer connection.Close()
			packet, ok := connection.(*internet.PacketConnWrapper)
			if !ok {
				t.Fatalf("expected an unconnected UDP socket, got %T", connection)
			}
			udp, ok := packet.Conn.(*net.UDPConn)
			if !ok {
				t.Fatalf("expected a system UDP socket, got %T", packet.Conn)
			}
			raw, err := udp.SyscallConn()
			if err != nil {
				t.Fatal(err)
			}
			var v4, v6 int
			var v4Error, v6Error error
			if err := raw.Control(func(fd uintptr) {
				v4, v4Error = windows.GetsockoptInt(windows.Handle(fd), syscall.IPPROTO_IP, IP_UNICAST_IF)
				v6, v6Error = windows.GetsockoptInt(windows.Handle(fd), syscall.IPPROTO_IPV6, IPV6_UNICAST_IF)
			}); err != nil {
				t.Fatal(err)
			}
			// Windows returns IP_UNICAST_IF in host byte order even though
			// setting its IPv4 value requires network byte order.
			if v4Error != nil || v6Error != nil || uint32(v4) != upstream || uint32(v6) != upstream {
				t.Fatalf("UDP interface protection changed: ipv4=%d (%v) ipv6=%d (%v), want index %d", v4, v4Error, v6, v6Error, upstream)
			}
		})
	}
}

type recordingSystemDialer struct {
	source      xnet.Address
	destination xnet.Destination
	options     *internet.SocketConfig
	calls       int
	error       error
}

func (d *recordingSystemDialer) Dial(_ context.Context, source xnet.Address, destination xnet.Destination, options *internet.SocketConfig) (net.Conn, error) {
	d.source, d.destination, d.options = source, destination, options
	d.calls++
	return nil, d.error
}

func TestWindowsProtectedDialerPreservesExplicitSettings(t *testing.T) {
	for _, test := range []struct {
		name        string
		source      xnet.Address
		destination xnet.Destination
		options     *internet.SocketConfig
	}{
		{"sendThrough", xnet.ParseAddress("192.0.2.44"), xnet.UDPDestination(xnet.LocalHostIP, 19090), &internet.SocketConfig{TcpNoDelay: true}},
		{"sendThroughIPv6", xnet.ParseAddress("2001:db8::44"), xnet.UDPDestination(xnet.LocalHostIPv6, 19090), &internet.SocketConfig{TcpNoDelay: true}},
		{"socketBind", xnet.AnyIP, xnet.UDPDestination(xnet.LocalHostIP, 19090), &internet.SocketConfig{BindAddress: []byte{192, 0, 2, 45}, BindPort: 19091}},
		{"tcp", xnet.AnyIP, xnet.TCPDestination(xnet.LocalHostIP, 19090), &internet.SocketConfig{TcpNoDelay: true}},
		{"nonLoopback", nil, xnet.UDPDestination(xnet.ParseAddress("203.0.113.33"), 19090), &internet.SocketConfig{}},
	} {
		t.Run(test.name, func(t *testing.T) {
			sentinel := errors.New("delegate result")
			delegate := &recordingSystemDialer{error: sentinel}
			dialer := &windowsProtectedSystemDialer{SystemDialer: delegate}
			_, err := dialer.Dial(context.Background(), test.source, test.destination, test.options)
			if !errors.Is(err, sentinel) || delegate.calls != 1 || delegate.source != test.source || delegate.destination != test.destination || delegate.options != test.options {
				t.Fatal("explicit source, destination, socket options or delegate result changed")
			}
		})
	}
}

func TestWindowsProtectionHonorsExistingDialerAndDisableOption(t *testing.T) {
	for _, mode := range []string{"custom", "disabled"} {
		t.Run(mode, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, os.Args[0], "-test.run=^TestWindowsProtectionProcessHelper$", "-test.v")
			command.Dir = t.TempDir()
			disabled := "0"
			if mode == "disabled" {
				disabled = "1"
			}
			command.Env = append(os.Environ(), "NEKORAY_BINDING_TEST_HELPER="+mode, "NKR_CORE_RAY_WINDOWS_DISABLE_AUTO_INTERFACE="+disabled)
			output, err := command.CombinedOutput()
			if err != nil {
				t.Fatalf("isolated %s option check: %v\n%s", mode, err, output)
			}
		})
	}
}

func TestWindowsProtectionProcessHelper(t *testing.T) {
	controller := func(string, string, syscall.RawConn) error { return nil }
	switch os.Getenv("NEKORAY_BINDING_TEST_HELPER") {
	case "disabled":
		if underlyingNetDialer != nil {
			t.Fatal("disabled interface protection still replaced the DNS dialer")
		}
		if err := internet.RegisterDialerController(controller); err != nil {
			t.Fatalf("disabled interface protection still replaced the default system dialer: %v", err)
		}
	case "custom":
		delegate := &recordingSystemDialer{error: errors.New("custom dialer")}
		internet.UseAlternativeSystemDialer(delegate)
		if err := installWindowsProtectedSystemDialer(controller); err == nil {
			t.Fatal("installing protection over a custom dialer was not rejected")
		}
		_, err := internet.DialSystem(context.Background(), xnet.UDPDestination(xnet.LocalHostIP, 19090), nil)
		if !errors.Is(err, delegate.error) || delegate.calls != 1 {
			t.Fatal("the existing custom system dialer was replaced")
		}
	}
}
